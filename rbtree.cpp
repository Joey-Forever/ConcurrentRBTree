#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <random>
#include <thread>
#include <cassert>

#ifdef DEBUG
    #define RB_ASSERT(condition) \
        do { assert(condition); } while (false)
#else
    #define RB_ASSERT(condition) do { } while (false)
#endif

// single thread rbtree.
// type KEY must implement operator< and operator==.
// type KEY and type VALUE must define default constructor.
// type VALUE must define move constructor.
template <typename VALUE>
class RBTree {
 public:
  class Node;
  typedef Node* TreeRoot;
  typedef Node* Father;
  typedef Node* LeftSon;
  typedef Node* RightSon;
  typedef Node* ListHeader;
  typedef Node* ListTailer;
  typedef Node* ListNext;

  using Side = typename Node::Side;
  using Color = typename Node::Color;

  enum OperationType { READ, INSERT, ERASE };

  struct InternalFindResult {
    // whether we should execute read/insert/erase and it's always true in read case.
    bool should_execute;
    // total searching times that current find has tried.
    int try_times;
    // pointer to the node that current case needs to execute next step.
    Node* magic_node;
  };

  enum WriteStatus { SUCCESS, RETRY, ABORT };
  struct WriteResult {
    WriteStatus status;
    // pointer to the node that should return to Writer after true write.
    Node* magic_node;
  };

  using WriteData = void*;
  struct WriteInfo {
    explicit WriteInfo(OperationType write_type, WriteData init_data)
        : estimated_less_bound(nullptr), type(write_type), data(init_data) {}
    Node* estimated_less_bound;
    OperationType type;
    WriteData data;
  };

  struct BatchWriteUnit {
    explicit BatchWriteUnit(OperationType write_type, WriteData init_data)
        : info(write_type, init_data), done(false) {}
    WriteInfo info;
    WriteResult result;
    std::atomic<bool> done;
  };

  struct Writer {
    explicit Writer()
        : batch_unit(nullptr), accessible(false) {}

    BatchWriteUnit* batch_unit;
    std::atomic<bool> accessible;
  };

 public:
  RBTree() {
    // root_ is never accessible to the user of rbtree.
    root_ = new Node();
    list_header_ = new Node();
    list_tailer_ = new Node();
    // 1. insert list_header_ and list_tailer_ into rbtree.
    root_->setSonNoBarrier(Node::LEFT, list_header_);
    list_header_->setSonNoBarrier(Node::RIGHT, list_tailer_);
    list_header_->setColor(Node::BLACK);
    list_tailer_->setColor(Node::RED);
    // 2. insert list_header_ and list_tailer_ into sorted_list.
    list_header_->setNext(list_tailer_);
    // 3. make list_header_ and list_tailer_ accessible.
    list_header_->setAccessibility(true);
    list_tailer_->setAccessibility(true);
    // init the write batch memory block.
    curr_write_batch_info_.store(0);
  }

  ~RBTree() {
    recursiveDestruction(root_);
  }

  // find the accessible node == value.
  Node* find(const VALUE& value) {
    Node* lower_bound = internalFind(value).magic_node;
    if (lower_bound == list_tailer_ || lower_bound->value() > value || !lower_bound->accessible()) {
      return nullptr;
    } else {
      // finally find the accessible node == value.
      return lower_bound;
    }
  }

  // return a std::pair:
  //   the first element means the try_times to find the value,
  //   the second element means the found accessible node.
  std::pair<int, Node*> findForConcurrentTest(const VALUE& value) {
    InternalFindResult result = internalFind(value);
    int try_times = result.try_times;
    Node* lower_bound = result.magic_node;
    if (lower_bound == list_tailer_ || lower_bound->value() > value || !lower_bound->accessible()) {
      return {try_times, nullptr};
    } else {
      // finally find the node == value.
      return {try_times, lower_bound};
    }
  }

  // find the first accessible node >= value.
  Node* lowerBound(const VALUE& value) {
    Node* lower_bound = internalFind(value).magic_node;
    while(lower_bound != list_tailer_ && !lower_bound->accessible()) {
      lower_bound = lower_bound->next();
    }
    if (lower_bound == list_tailer_) {
      return nullptr;
    }
    return lower_bound;
  }

  // Macro to handle write batch - will be inlined at call site
  #define HANDLE_WRITE_BATCH() \
    do { \
      /* reset the curr_write_batch_info_ */ \
      uint32_t old_write_batch_info = curr_write_batch_info_.load(); \
      int old_write_batch_id = (old_write_batch_info >> 31); \
      if (old_write_batch_id == 0) { \
        old_write_batch_info = curr_write_batch_info_.exchange(1U << 31, std::memory_order_acq_rel); \
      } else { \
        old_write_batch_info = curr_write_batch_info_.exchange(0, std::memory_order_acq_rel); \
      } \
      int old_write_batch_size = (old_write_batch_info & ((1U << 31) - 1)); \
      RB_ASSERT((old_write_batch_id == 0 || old_write_batch_id == 1) && old_write_batch_size <= WRITE_BATCH_MAX_SIZE); \
      Writer* old_write_batch_addr = write_batch_addr_[old_write_batch_id]; \
      \
      int erase_cnt = 0; \
      for (int i = 0; i < old_write_batch_size; i++) { \
        while(!old_write_batch_addr[i].accessible.load(std::memory_order_acquire)) {}  /* spin wait for accessible */ \
        BatchWriteUnit* write_unitt = old_write_batch_addr[i].batch_unit; \
        if (write_unitt->info.type == OperationType::ERASE) { \
          tmp_array_for_handle_write_batch_[erase_cnt++] = i; \
          continue; \
        } \
        Node* insert_node = (Node*)(write_unitt->info.data); \
        /* find the exact_less_bound as the insert position in the sorted-list according to estimated_less_bound */ \
        Node* exact_less_bound = findExactLessBoundForWrite(write_unitt->info.estimated_less_bound, insert_node->value()); \
        if (exact_less_bound == nullptr) { \
          /* 1. fail to find the insert position, retry */ \
          write_unitt->result.status = WriteStatus::RETRY; \
        } else { \
          Node* no_less_bound = exact_less_bound->next(); \
          if (no_less_bound == list_tailer_ || no_less_bound->value() > insert_node->value()) { \
            /* 2. insert_node's target_value doesn't exist, execute insert and return the insert_node */ \
            internalInsert(insert_node, exact_less_bound); \
            write_unitt->result.status = WriteStatus::SUCCESS; \
            write_unitt->result.magic_node = insert_node; \
          } else { \
            /* 3. insert_node's target_value already exists, abort insert and return the existed node */ \
            RB_ASSERT(no_less_bound->value() == insert_node->value()); \
            write_unitt->result.status = WriteStatus::ABORT; \
            write_unitt->result.magic_node = no_less_bound; \
          } \
        } \
        write_unitt->done.store(true, std::memory_order_release); \
        /* reset the writer slot */ \
        old_write_batch_addr[i].accessible.store(false, std::memory_order_release); \
        old_write_batch_addr[i].batch_unit = nullptr; \
      } \
      \
      for (int ii = 0; ii < erase_cnt; ii++) { \
        int i = tmp_array_for_handle_write_batch_[ii]; \
        BatchWriteUnit* write_unitt = old_write_batch_addr[i].batch_unit; \
        VALUE* target_erase_value = (VALUE*)(write_unitt->info.data); \
        /* find the exact_less_bound as the erase position in the sorted-list according to estimated_less_bound */ \
        Node* exact_less_bound = findExactLessBoundForWrite(write_unitt->info.estimated_less_bound, *target_erase_value); \
        if (exact_less_bound == nullptr) { \
          /* 1. fail to find the erase position, retry */ \
          write_unitt->result.status = WriteStatus::RETRY; \
        } else { \
          Node* no_less_bound = exact_less_bound->next(); \
          if (no_less_bound == list_tailer_ || no_less_bound->value() > *target_erase_value) { \
            /* 2. target_erase_value doesn't exist, abort erase */ \
            write_unitt->result.status = WriteStatus::ABORT; \
          } else { \
            /* 3. target_erase_value exists, execute erase */ \
            RB_ASSERT(no_less_bound->value() == *target_erase_value); \
            internalErase(exact_less_bound); \
            write_unitt->result.status = WriteStatus::SUCCESS; \
          } \
        } \
        write_unitt->done.store(true, std::memory_order_release); \
        /* reset the writer slot */ \
        old_write_batch_addr[i].accessible.store(false, std::memory_order_release); \
        old_write_batch_addr[i].batch_unit = nullptr; \
      } \
    } while (0)

  // the insert operation would be ignored if key exists and return the Node* to the existed value,
  // otherwise execute insertion firstly.
  template<typename U>
  Node* insert(U&& insert_value) {
    Node* insert_node = new Node(std::forward<U>(insert_value));
    int try_times = 0;
    while (true) {
      try_times++;
      // 1. get the estimated_less_bound.
      BatchWriteUnit write_unit(OperationType::INSERT, (void*)insert_node);
      write_unit.info.estimated_less_bound = findEstimatedLessBoundForWrite(insert_node->value());
      // 2. preserve a writer position.
      uint32_t curr_write_batch_info = curr_write_batch_info_.fetch_add(1U, std::memory_order_acq_rel);
      // 3. parse the write batch id and writer_idx.
      int curr_write_batch_id = (curr_write_batch_info >> 31);
      int writer_idx = (curr_write_batch_info & ((1U << 31) - 1));
      RB_ASSERT((curr_write_batch_id == 0 || curr_write_batch_id == 1) && writer_idx < WRITE_BATCH_MAX_SIZE);
      Writer* writer = &(write_batch_addr_[curr_write_batch_id][writer_idx]);
      RB_ASSERT(!writer->accessible.load() && writer->batch_unit == nullptr);
      // 4. use write_unit to set writer's batch_unit and then make writer accessible for leader writer to execute true write operation.
      writer->batch_unit = &write_unit;
      writer->accessible.store(true, std::memory_order_release);
      bool retry = false;
      while (write_leader_flag_.test_and_set(std::memory_order_acquire)) {
        // now we failed to get leader flag.
        if (write_unit.done.load(std::memory_order_acquire)) {
          // ! here curr thread is the follower thread in the writers_ queue, and write operation is done by leader thread.
          if (write_unit.result.status == WriteStatus::RETRY) {
            retry = true;
            break;
          } else {
            if (write_unit.result.status == WriteStatus::ABORT) delete insert_node;
            return write_unit.result.magic_node;
          }
        } else {
          // sleep for a while.
          // std::this_thread::sleep_for(std::chrono::nanoseconds(0));
          std::this_thread::yield();
        }
      }

      if (retry) {
        // it means our job was done by leader and we should retry.
        continue;
      }

      // ! here we got the leader flag but we still need to check if our job done or not.
      if (write_unit.done.load(std::memory_order_acquire)) {
        // ok, our job is done, it means curr thread is a follower.
        write_leader_flag_.clear(std::memory_order_release);
        if (write_unit.result.status == WriteStatus::RETRY) {
          continue;
        } else {
          if (write_unit.result.status == WriteStatus::ABORT) delete insert_node;
          return write_unit.result.magic_node;
        }
      }

      // ! here curr thread is the leader thread, execute write operation directly.
      // ! only one write thread could be here at the same time.

      HANDLE_WRITE_BATCH();

      write_leader_flag_.clear(std::memory_order_release);

      // handle leader's write result.
      RB_ASSERT(write_unit.done.load(std::memory_order_acquire));
      if (write_unit.result.status == WriteStatus::RETRY) {
        continue;
      } else {
        if (write_unit.result.status == WriteStatus::ABORT) delete insert_node;
        return write_unit.result.magic_node;
      }
    }
  }

  // return false if the erase_key doesn't exist.
  bool erase(const VALUE& erase_value) {
    int try_times = 0;
    while (true) {
      try_times++;
      // 1. get the estimated_less_bound.
      BatchWriteUnit write_unit(OperationType::ERASE, (void*)(&erase_value));
      write_unit.info.estimated_less_bound = findEstimatedLessBoundForWrite(erase_value);
      // 2. preserve a writer position.
      uint32_t curr_write_batch_info = curr_write_batch_info_.fetch_add(1U, std::memory_order_acq_rel);
      // 3. parse the write batch id and writer_idx.
      int curr_write_batch_id = (curr_write_batch_info >> 31);
      int writer_idx = (curr_write_batch_info & ((1U << 31) - 1));
      RB_ASSERT((curr_write_batch_id == 0 || curr_write_batch_id == 1) && writer_idx < WRITE_BATCH_MAX_SIZE);
      Writer* writer = &(write_batch_addr_[curr_write_batch_id][writer_idx]);
      RB_ASSERT(!writer->accessible.load() && writer->batch_unit == nullptr);
      // 4. use write_unit to set writer's batch_unit and then make writer accessible for leader writer to execute true write operation.
      writer->batch_unit = &write_unit;
      writer->accessible.store(true, std::memory_order_release);
      bool retry = false;
      while (write_leader_flag_.test_and_set(std::memory_order_acquire)) {
        // now we failed to get leader flag.
        if (write_unit.done.load(std::memory_order_acquire)) {
          // ! here curr thread is the follower thread in the writers_ queue, and write operation is done by leader thread.
          if (write_unit.result.status == WriteStatus::RETRY) {
            retry = true;
            break;
          } else if (write_unit.result.status == WriteStatus::ABORT) {
            return false;
          } else {
            // erase success.
            return true;
          }
        } else {
          // sleep for a while.
          // std::this_thread::sleep_for(std::chrono::nanoseconds(0));
          std::this_thread::yield();
        }
      }

      if (retry) {
        // it means our job was done by leader and we should retry.
        continue;
      }

      // ! here we got the leader flag but we still need to check if our job done or not.
      if (write_unit.done.load(std::memory_order_acquire)) {
        // ok, our job is done, it means curr thread is a follower.
        write_leader_flag_.clear(std::memory_order_release);
        if (write_unit.result.status == WriteStatus::RETRY) {
          continue;
        } else if (write_unit.result.status == WriteStatus::ABORT) {
          return false;
        } else {
          // erase success.
          return true;
        }
      }

      // ! here curr thread is the leader thread, execute write operation directly.
      // ! only one write thread could be here at the same time.

      HANDLE_WRITE_BATCH();

      write_leader_flag_.clear(std::memory_order_release);

      // handle leader's write result.
      RB_ASSERT(write_unit.done.load(std::memory_order_acquire));
      if (write_unit.result.status == WriteStatus::RETRY) {
        continue;
      } else if (write_unit.result.status == WriteStatus::ABORT) {
        return false;
      } else {
        // erase success.
        return true;
      }
    }
  }

  void getHeightInfoForTest(Node* curr_node, int curr_height, int& newest_max_height, int& newest_min_height, int& node_count) {
    if (curr_node == nullptr) {
      return;
    }
    node_count++;
    if (curr_node->leftSonNoBarrier() == nullptr && curr_node->rightSonNoBarrier() == nullptr) {
      newest_max_height = std::max(curr_height, newest_max_height);
      newest_min_height = std::min(curr_height, newest_min_height);
      return;
    }
    getHeightInfoForTest(curr_node->leftSonNoBarrier(), curr_height + 1, newest_max_height, newest_min_height, node_count);
    getHeightInfoForTest(curr_node->rightSonNoBarrier(), curr_height + 1, newest_max_height, newest_min_height, node_count);
  }

  void checkIfSortedListValidForTest() {
    // 1. check if sorted-list is increasing.
    Node* curr_node = list_header_->next()->next();
    Node* last_node = list_header_->next();
    while(curr_node != nullptr && curr_node != list_tailer_) {
      RB_ASSERT(last_node->value() < curr_node->value());
      last_node = curr_node;
      curr_node = curr_node->next();
    }
    // 2. check if all nodes are unlocked.
    curr_node = list_header_;
    while (curr_node != list_tailer_) {
      curr_node = curr_node->next();
    }
  }

  Node* getRootForTest() {
    return root_;
  }

  Node* getTailer() {
    return list_tailer_;
  }

 private:
  static const int MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND = 3;
  static const size_t WRITE_BATCH_MAX_SIZE = 50;
  // no-data node, left son is the true data root.
  TreeRoot root_;
  // no-data list node, as the header of the sorted list for all data node.
  ListHeader list_header_;
  // no-data list node, as the tailer of the sorted list for all data node.
  ListTailer list_tailer_;
  // used for leader write thread to execute true write operation.
  // it's no need to be guarded by lock due to it's at most one leader write thread at the same time.
  Writer write_batch_addr_[2][WRITE_BATCH_MAX_SIZE];
  // top 1 bit means curr write batch id,
  // low 31 bit means curr write batch size.
  std::atomic<uint32_t> curr_write_batch_info_;
  // only the write thread who get this flag could execute true write operation.
  std::atomic_flag write_leader_flag_ = ATOMIC_FLAG_INIT;
  // an assistant array for HANDLE_WRITE_BATCH to seperate insert operations and erase operations.
  int tmp_array_for_handle_write_batch_[WRITE_BATCH_MAX_SIZE];

  friend class Node;

  // recursive destruct a sub-tree whose root is curr_node.
  void recursiveDestruction(Node* curr_node) {
    if (curr_node == nullptr) {
      return;
    }
    recursiveDestruction(curr_node->leftSonNoBarrier());
    recursiveDestruction(curr_node->rightSonNoBarrier());
    delete curr_node;
  }

  // return an InternalFindResult:
  //  read case:
  //    should_execute is always true, magic_node means the lower bound of target_value.
  InternalFindResult internalFind(const VALUE& target_value) {
    Node* no_less_bound = list_header_;
    int try_times = 0;
    // we won't limit the try_times when the extra_steps_to_find_lower_bound over limit again and again.
    // in some very rare cases, try_times would be large but it would finally success finding lowerbound and stop.
    while (true) {
      try_times++;
      // find the target_value from the rbtree, and record the less bound(the greatest node < target_value) inside the searching path at the same time.
      Node* less_bound = list_header_;
      Node* curr_node = root_->leftSonNoBarrier();
      while(curr_node != nullptr) {
        if (curr_node == list_header_) {
          curr_node = curr_node->rightSonNoBarrier();
        } else if (curr_node == list_tailer_) {
          curr_node = curr_node->leftSonNoBarrier();
        } else if (target_value < curr_node->value()) {
          curr_node = curr_node->leftSonNoBarrier();
        } else if (target_value > curr_node->value()) {
          if (less_bound == list_header_ || curr_node->value() > less_bound->value()) less_bound = curr_node;
          curr_node = curr_node->rightSonNoBarrier();
        } else {
          // curr_node's value == target_value.
          break;
        }
      }
      // here curr_node is nullptr or equals to target_value.
      RB_ASSERT((curr_node == nullptr || curr_node->value() == target_value) &&
             (less_bound == list_header_ || less_bound->value() < target_value));
      if (curr_node != nullptr) {
        return {true, try_times, curr_node};
      } else {
        // find the first node >= target_value across sorted-list.
        no_less_bound = less_bound->next();
        int extra_steps_to_find_lower_bound = 0;
        // in concurrent read-write case, that exists 3 situations that we need to execute next-step across sorted-list:
        // 1. no rbtree rotate influence:
        //  1.1. target_value doesn't exist, we only need to next 1 step to find the lower bound.
        //  1.2. erase operation move the target node up to current-find's node. in most cases, we only need to next 1 step,
        //       but in very rare cases, multiple erase operations may execute faster than the current find operation and
        //       then we must need to next more step, when this case happen, we limit it up to MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND.
        // 2. rbtree rotate influence:
        //  2.1. in this case, it means the current find operation inside rbtree failed due to the wrong middle state of rbtree-structure causing by
        //       rotate operation. in most cases, we can't find the lower bound with MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND next steps.
        while (no_less_bound != list_tailer_ &&
               no_less_bound->value() < target_value &&
               extra_steps_to_find_lower_bound < MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND) {
          extra_steps_to_find_lower_bound++;
          // here curr thread could safely access the next node because it would never change due to insert/erase by other threads since no_less_bound is locked by curr thread.
          no_less_bound = no_less_bound->next();
        }
        // here no_less_bound never be list_header_
        if (no_less_bound == list_tailer_ || no_less_bound->value() >= target_value) {
          // all nodes < target_key or finally find the first node >= target_key.
          return {true, try_times, no_less_bound};
        } else {
          // extra_steps_to_find_lower_bound over limit. we consider that searching into rbtree failed due to rotate operation.
          no_less_bound = list_header_;
          continue;
        }
      }
    }
  }

  // return the exact less_bound curr_thread found and return nullptr if fail to find.
  // if return node is not nullptr, this method ensures that both exact_less_bound and no_less_bound are accessible.
  // ! only leader write thread could call this method with holding write_mutex_.
  Node* findExactLessBoundForWrite(Node* estimated_less_bound, const VALUE& target_value) {
    RB_ASSERT(estimated_less_bound == list_header_ || estimated_less_bound->value() < target_value);
    Node* exact_less_bound = estimated_less_bound;
    Node* no_less_bound = exact_less_bound->next();
    int extra_steps_to_find_lower_bound = 0;
    while (no_less_bound != list_tailer_ &&
           no_less_bound->value() < target_value &&
           extra_steps_to_find_lower_bound < MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND) {
      extra_steps_to_find_lower_bound++;
      exact_less_bound = no_less_bound;
      no_less_bound = no_less_bound->next();
    }
    RB_ASSERT((exact_less_bound == list_header_ || exact_less_bound->value() < target_value) &&
              exact_less_bound->next() == no_less_bound);
    // here no_less_bound never be list_header_
    if ((no_less_bound == list_tailer_ || no_less_bound->value() >= target_value) &&
        exact_less_bound->accessible() && no_less_bound->accessible()) {
      // all nodes < target_key or finally find the first node >= target_key.
      return exact_less_bound;
    } else {
      // extra_steps_to_find_lower_bound over limit. we consider that searching into rbtree failed due to rotate operation.
      return nullptr;
    }
  }

  // return the estimated less_bound curr thread found.
  // a LOCK-FREE find method and the found less_bound may be incorrect but it doesn't matter
  // because it's only an estimated value for true write thread to accelerate write operation.
  Node* findEstimatedLessBoundForWrite(const VALUE& target_value) {
    // find the target_value from the rbtree, and record the less bound(the greatest node < target_value) inside the searching path at the same time.
    Node* estimated_less_bound = list_header_;
    Node* curr_node = root_->leftSonNoBarrier();
    while(curr_node != nullptr) {
      if (curr_node == list_header_) {
        curr_node = curr_node->rightSonNoBarrier();
      } else if (curr_node == list_tailer_) {
        curr_node = curr_node->leftSonNoBarrier();
      } else if (target_value < curr_node->value()) {
        curr_node = curr_node->leftSonNoBarrier();
      } else if (target_value > curr_node->value()) {
        if (estimated_less_bound == list_header_ || curr_node->value() > estimated_less_bound->value()) estimated_less_bound = curr_node;
        curr_node = curr_node->rightSonNoBarrier();
      } else {
        // curr_node's value == target_value.
        curr_node = curr_node->leftSonNoBarrier();
      }
    }
    RB_ASSERT(curr_node == nullptr && (estimated_less_bound == list_header_ || estimated_less_bound->value() < target_value));
    return estimated_less_bound;
  }

  inline void internalInsert(Node* insert_node, Node* predecessor) {
    // insert_key doesn't exist, execute insertion.

    Node* successor = predecessor->next();
    // 1. insert node into sorted list.
    {
      insert_node->setNextNoBarrier(successor);
      predecessor->setNext(insert_node);
    }
    // 3. insert node into rbtree.
    RB_ASSERT(predecessor->rightSonNoBarrier() == nullptr || successor->leftSonNoBarrier() == nullptr);
    if (predecessor->rightSonNoBarrier() == nullptr) {
      predecessor->setSonNoBarrier(Node::RIGHT, insert_node);
    } else {
      successor->setSonNoBarrier(Node::LEFT, insert_node);
    }
    // 4. set insert_node accessible after it's existed into sorted-list and rbtree.
    insert_node->setAccessibility(true);
    // 5. execute upward balance.
    balanceTheTreeAfterInsert(insert_node);
  }

  inline void internalErase(Node* predecessor) {
    // erase_key exists, execute erase.

    Node* erase_node = predecessor->next();
    if (erase_node->leftSonNoBarrier() == nullptr && erase_node->rightSonNoBarrier() != nullptr) {
      // erase_node only has right son. And the right son must be a leaf node.
      // rotate left to make the erase_node to be a leaf node.
      Node* right_son = erase_node->rightSonNoBarrier();
      rotateLeft(erase_node, erase_node->father());
      Node::SwapColor(erase_node, right_son); // ! swap color to balance the tree
    }else if (erase_node->rightSonNoBarrier() == nullptr && erase_node->leftSonNoBarrier() != nullptr) {
      // erase_node only has left son. And the left son must be a leaf node.
      // rotate right to make the erase_node to be a leaf node.
      Node* left_son = erase_node->leftSonNoBarrier();
      rotateRight(erase_node, erase_node->father());
      Node::SwapColor(erase_node, left_son); // ! swap color to balance the tree
    }
    // here the erase_node must be a leaf node or a non-leaf node with two son.
    if (erase_node->leftSonNoBarrier() == nullptr && erase_node->rightSonNoBarrier() == nullptr) {
      // erase_node is a leaf node, erase directly.

      Node* father_of_erase_node = erase_node->father();
      // 1. make erase_node inaccessible.
      erase_node->setAccessibility(false);
      // 2. detach erase_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_erase_node->setSonNoBarrier(
        father_of_erase_node->leftSonNoBarrier() == erase_node ? Node::LEFT : Node::RIGHT, nullptr);
      // 3. detach erase_node from sorted-list.
      predecessor->setNext(erase_node->next());
      // 4. upward balance the rbtree.
      if (erase_node->color() == Node::BLACK && father_of_erase_node != root_) {
        // ensure bro_of_delete_side must exist.
        balanceTheTreeAfterErase(father_of_erase_node, delete_side);
      }
      // 5. finally delete erase_node.
      // delete erase_node;
    } else {
      // erase_node is a non-leaf node with two son.

      Node* father_of_erase_node = erase_node->father();
      // here right_most_node is the max(e.g. right most) node of the erase_node's left-subtree.
      Node* right_most_node = predecessor;
      Node* left_son_of_right_most_node = right_most_node->leftSonNoBarrier();
      if (left_son_of_right_most_node != nullptr) {
        // rotate right to make the right-most node to be a leaf node.
        rotateRight(right_most_node, right_most_node->father()); // right_most_node's father wouldn't be root_.
        Node::SwapColor(right_most_node, left_son_of_right_most_node); // ! swap color to balance the tree
      }
      // here right_most_node is the max and right-most and leaf node of the erase_node's left-subtree.
      Node* father_of_right_most_node = right_most_node->father();
      // 1. detach right_most_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_right_most_node->setSonNoBarrier(
        father_of_right_most_node->leftSonNoBarrier() == right_most_node ? Node::LEFT : Node::RIGHT, nullptr);
      // 2. upward balance the rbtree.
      if (right_most_node->color() == Node::BLACK && father_of_right_most_node != root_) {
        // ensure bro_of_delete_side must exist.
        balanceTheTreeAfterErase(father_of_right_most_node, delete_side);
      }
      // 3. find the erase_node for removing.
      father_of_erase_node = erase_node->father();
      // 4. make erase_node inaccessible.
      erase_node->setAccessibility(false);
      // 5. replace erase_node with right_most_node on erase_node's position in rbtree.
      right_most_node->setSonNoBarrier(Node::LEFT, erase_node->leftSonNoBarrier());
      right_most_node->setSonNoBarrier(Node::RIGHT, erase_node->rightSonNoBarrier());
      right_most_node->setColor(erase_node->color());
      father_of_erase_node->setSonNoBarrier(
        father_of_erase_node->leftSonNoBarrier() == erase_node ? Node::LEFT : Node::RIGHT, right_most_node);
      // 6. detach erase_node from sorted-list.
      right_most_node->setNext(erase_node->next());
      // 7. finally delete erase_node.
      // delete erase_node;
    }
  }

  inline Node* getBro(Node* my_self, Node* father) const {
    return father->leftSonNoBarrier() == my_self ? father->rightSonNoBarrier() : father->leftSonNoBarrier();
  }

  // after making same side, this method would change color to grand-fa(red), father(black) and son(red).
  // same side means:
  // 1. father's left son is my_self and grand_father's left son is father.
  // 2. father's right son is my_self and grand_father's right son is father.
  inline Side makeTreeGenSameSide(Node* my_self, Node* father, Node* grand_father) {
    if (grand_father->rightSonNoBarrier() == father) {
      if (father->leftSonNoBarrier() == my_self) rotateRight(father, grand_father);
      grand_father->setColor(Node::RED);
      grand_father->rightSonNoBarrier()->setColor(Node::BLACK);
      grand_father->rightSonNoBarrier()->rightSonNoBarrier()->setColor(Node::RED);
      return Node::RIGHT;
    }
    if (father->rightSonNoBarrier() == my_self) rotateLeft(father, grand_father);
    grand_father->setColor(Node::RED);
    grand_father->leftSonNoBarrier()->setColor(Node::BLACK);
    grand_father->leftSonNoBarrier()->leftSonNoBarrier()->setColor(Node::RED);
    return Node::LEFT;
  }

  // this method is a recursive method.
  void balanceTheTreeAfterInsert(Node* insert_node) {
    Node* father = insert_node->father();
    if (father == root_) {
      // the insertion finally causes height increasing.
      insert_node->setColor(Node::BLACK);
      return;
    }
    if (father->color() == Node::BLACK) {
      insert_node->setColor(Node::RED);
      return;
    }
    // father color is RED, conditions are more complex.

    // because faher's color is RED, so grand_father must exist and be BLACK.
    Node* grand_father = father->father();
    Node* uncle = getBro(father, grand_father);
    if (uncle == nullptr || uncle->color() == Node::BLACK) {
      Side side = makeTreeGenSameSide(insert_node, father, grand_father);
      if (side == Node::LEFT) rotateRight(grand_father, grand_father->father());
      else rotateLeft(grand_father, grand_father->father());
      return;
    }
    // uncle is RED too, which would cause upward balance.
    insert_node->setColor(Node::RED);
    father->setColor(Node::BLACK);
    uncle->setColor(Node::BLACK);
    // grand_father as the new insert node, execute upward balance.
    return balanceTheTreeAfterInsert(grand_father);
  }

  // rotate-right the subtree rooted on node.
  // !only rotate, NOT change color.
  inline Node* rotateRight(Node* node, Node* father) {
    Node* left_son = node->leftSonNoBarrier();
    if (left_son == nullptr) {
      return nullptr;
    }
    node->setSonNoBarrier(Node::LEFT, left_son->rightSonNoBarrier());
    left_son->setSonNoBarrier(Node::RIGHT, node);
    father->setSonNoBarrier(
      father->leftSonNoBarrier() == node ? Node::LEFT : Node::RIGHT, left_son);
    return left_son;
  }

  // rotate-left the subtree rooted on node.
  // !only rotate, NOT change color.
  inline Node* rotateLeft(Node* node, Node* father) {
    Node* right_son = node->rightSonNoBarrier();
    if (right_son == nullptr) {
      return nullptr;
    }
    node->setSonNoBarrier(Node::RIGHT, right_son->leftSonNoBarrier());
    right_son->setSonNoBarrier(Node::LEFT, node);
    father->setSonNoBarrier(father->leftSonNoBarrier() == node ? Node::LEFT : Node::RIGHT, right_son);
    return right_son;
  }

  inline Node* broOfDeleteSide(Node* father, Side delete_side) {
    if (delete_side == Node::LEFT) return father->rightSonNoBarrier();
    else return father->leftSonNoBarrier();
  }

  // recursive method. father_of_erase_node's delete_side-subtree has one node deleted.
  void balanceTheTreeAfterErase(Node* father_of_erase_node, Side delete_side) {
    // delete_side's subtree must be null or rooted with a BLACK node.
    // bro_of_delete_side must exist.
    bool increased_height;
    Node* grand_fa = father_of_erase_node->father();
    Node* bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
    if (father_of_erase_node->color() == Node::BLACK && bro_of_delete_side->color() == Node::RED) {
      // 1. bro is RED.
      increased_height = true;
      if (delete_side == Node::LEFT) rotateLeft(father_of_erase_node, grand_fa);
      else rotateRight(father_of_erase_node, grand_fa);
      bro_of_delete_side->setColor(Node::BLACK);
      grand_fa = bro_of_delete_side;
      // new bro must exist.
      bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
    } else if (father_of_erase_node->color() == Node::RED && bro_of_delete_side->color() == Node::BLACK) {
      // 2. fa is RED;
      increased_height = true;
      father_of_erase_node->setColor(Node::BLACK);
    } else {
      // 3. no-one is RED.
      increased_height = false;
    }
    // here both fa and bro are BLACK.
    // increased_height is true means we need to decrease bro's height.
    // increased_height is false means we need to increase delete_side's height.
    if ((bro_of_delete_side->leftSonNoBarrier() == nullptr || bro_of_delete_side->leftSonNoBarrier()->color() == Node::BLACK) &&
        (bro_of_delete_side->rightSonNoBarrier() == nullptr || bro_of_delete_side->rightSonNoBarrier()->color() == Node::BLACK)) {
      // bro can safely be colored with RED.
      bro_of_delete_side->setColor(Node::RED);
      if (increased_height || grand_fa == root_) {
        return;
      } else {
        return balanceTheTreeAfterErase(grand_fa, grand_fa->leftSonNoBarrier() == father_of_erase_node ? Node::LEFT : Node::RIGHT);
      }
    }
    // here either bro's left son or right son is RED (or both RED).
    // for example, if delete_side is LEFT, we need to ensure the RED node is bro's right son, and vice versa.
    // and then we can use the RED node to balance the fa subtree.
    if (delete_side == Node::LEFT && (bro_of_delete_side->rightSonNoBarrier() == nullptr || bro_of_delete_side->rightSonNoBarrier()->color() == Node::BLACK)) {
      Node::SwapColor(bro_of_delete_side, bro_of_delete_side->leftSonNoBarrier());
      rotateRight(bro_of_delete_side, father_of_erase_node);
      bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
      // now bro's right son is RED, we use it.
    } else if (delete_side == Node::RIGHT && (bro_of_delete_side->leftSonNoBarrier() == nullptr || bro_of_delete_side->leftSonNoBarrier()->color() == Node::BLACK)) {
      Node::SwapColor(bro_of_delete_side, bro_of_delete_side->rightSonNoBarrier());
      rotateLeft(bro_of_delete_side, father_of_erase_node);
      bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
      // now bro's left son is RED, we use it.
    }
    if (delete_side == Node::LEFT) {
      bro_of_delete_side->rightSonNoBarrier()->setColor(Node::BLACK);
      rotateLeft(father_of_erase_node, grand_fa);
    } else {
      bro_of_delete_side->leftSonNoBarrier()->setColor(Node::BLACK);
      rotateRight(father_of_erase_node, grand_fa);
    }
    if (increased_height) {
      bro_of_delete_side->setColor(Node::RED);
    }
    return;
  }
};

static RBTree<int>* g_rbtree;

template <typename VALUE>
class RBTree<VALUE>::Node {
 public:
  enum Side { RIGHT, LEFT };
  enum Color { RED, BLACK };

  explicit Node()
    : value_(), accessible_(false),
      father_(nullptr), left_son_(nullptr),
      right_son_(nullptr), next_(nullptr) {}

  template<typename U>
  explicit Node(U&& value)
    : value_(std::forward<U>(value)), accessible_(false),
      father_(nullptr), left_son_(nullptr),
      right_son_(nullptr), next_(nullptr) {}

  // when a node is going to be destructed, caller MUST make sure firstly that the node
  // is detached from rbtree and that the list-node is detached from sorted list.
  ~Node() {
    father_ = nullptr;
    left_son_.store(nullptr);
    right_son_.store(nullptr);
    next_.store(nullptr);
  }

  static void SwapColor(Node* node1, Node* node2) {
    if (node1 != nullptr && node2 != nullptr) {
      std::swap(node1->color_, node2->color_);
    }
  }

  inline VALUE& value() {
    RB_ASSERT(this != g_rbtree->list_header_ && this != g_rbtree->list_tailer_);
    return value_;
  }

  inline Color color() const { return color_; }

  inline void setColor(Color new_color) { color_ = new_color; }

  inline bool accessible() const {
    return accessible_.load(std::memory_order_acquire);
  }

  inline void setAccessibility(bool accessible) {
    accessible_.store(accessible, std::memory_order_release);
  }

  inline Node* father() const {
    return father_;
  }

  inline Node* leftSonNoBarrier() const {
    return left_son_.load(std::memory_order_relaxed);
  }

  inline Node* rightSonNoBarrier() const {
    return right_son_.load(std::memory_order_relaxed);
  }

  inline Side setSonNoBarrier(Side side, Node* new_son) {
    if (side == LEFT) left_son_.store(new_son, std::memory_order_relaxed);
    else right_son_.store(new_son, std::memory_order_relaxed);
    // ! In fact, when this node's son changes to new_son, new_son's father would change to this node too,
    // ! so we could update new_son's father here and it's no need to define a public setFather method.
    // ! we have no need to worry about old_son's new father, because when we insert it into rbtree again,
    // ! its new_father would call setSon method and then set old_son's new father at the same time.
    // ! *DO NOT* set old_son's father to nullptr !!!
    if (new_son != nullptr) new_son->father_ = this;
    return side;
  }

  inline Node* next() const {
    return next_.load(std::memory_order_acquire);
  }

  inline void setNext(ListNext new_next) {
    next_.store(new_next, std::memory_order_release);
  }

  inline void setNextNoBarrier(ListNext new_next) {
    next_.store(new_next, std::memory_order_relaxed);
  }

 private:
  // data region
  VALUE value_;
  Color color_;
  std::atomic<bool> accessible_; // whether the node is accessible to the user of rbtree.

  // pointer region
  Father father_; // ! only used for write case's upward balance.
  std::atomic<LeftSon> left_son_;
  std::atomic<RightSon> right_son_;
  std::atomic<ListNext> next_;

};

static void TestSingleThreadAbility(bool sequential_insert) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "single thread test (sequential_insert = " << (sequential_insert ? "true" : "false") << "):\n";
  RBTree<int> my_map;
  g_rbtree = &my_map;
  int times = 100;
  std::deque<int> vec_for_map;
  int max_val = 0;
  while (times--) {
    std::random_device rd;          // Áî®‰∫éÁîüÊàêÁúüÈöèÊú∫ÁßçÂ≠êÔºàÂ¶? /dev/urandomÔº?
    std::mt19937 gen(rd());         // ‰ΩøÁî®Ê¢ÖÊ£ÆÊóãËΩ¨ÁÆóÊ≥ï‰Ωú‰∏∫ÂºïÊìé
    std::uniform_int_distribution<> dis(0, 100000000); // ÁîüÊàê [1, 100] ÁöÑÂùáÂåÄÊï¥Êï∞
    for (int i = 0; i < 128 * 1024; i++) {
      int a;
      if (!sequential_insert) a = dis(gen);
      else a = max_val++;
      if (my_map.find(a) != nullptr) i--;
      else my_map.insert(a), vec_for_map.push_back(a);
    }
    my_map.checkIfSortedListValidForTest();
    int newest_max_height = INT32_MIN;
    int newest_min_height = INT32_MAX;
    int node_count = 0;
    my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
    std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
    for (int i = 0; i < 128 * 1024; i++) {
      if (!my_map.erase(vec_for_map.front())) i--;
      else vec_for_map.pop_front();
    }
    newest_max_height = INT32_MIN;
    newest_min_height = INT32_MAX;
    node_count = 0;
    my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
    std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
    std::cout << "\n";
  }
}

static void TestOneWriteMultiReadConcurrentPerf(int perf_max_try_times, bool is_worst_case) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "concurrent test --- one write, multi read (perf_max_try_times = " << perf_max_try_times << ", is_worst_case = " << (is_worst_case ? "true" : "false") << "):\n";
  RBTree<int> my_map;
  g_rbtree = &my_map;
  std::atomic<int> my_map_size(0);
  std::set<int> init_set, random_set_for_insert;
  std::vector<int> init_list, random_list_for_insert;
  const int INIT_DATA_COUNT = 1024;
  const int DATA_COUNT_FOR_INSERTION = 100000;
  const int MAX_TRY_TIMES = INT32_MAX;
  // x86-64 intel (11 read threads, MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND = 3):
  // random case (random insert value and random find value):
  // try_times = 1 : 99.9997%
  // try_times = 2 : 100%
  // worst case (sequential insert and always find the last insert value):
  // try_times = 1 : 97.6%
  // try_times = 2 : 99.99%
  const int PERF_MAX_TRY_TIMES = perf_max_try_times;
  const int READ_THREAD_COUNT = 11;
  std::random_device rd;          // Áî®‰∫éÁîüÊàêÁúüÈöèÊú∫ÁßçÂ≠êÔºàÂ¶? /dev/urandomÔº?
  std::mt19937 gen(rd());         // ‰ΩøÁî®Ê¢ÖÊ£ÆÊóãËΩ¨ÁÆóÊ≥ï‰Ωú‰∏∫ÂºïÊìé
  std::uniform_int_distribution<> dis(0, 100000000); // ÁîüÊàê [1, 100] ÁöÑÂùáÂåÄÊï¥Êï∞
  int value = 0;
  auto gen_value = [&gen, &dis, &value, is_worst_case]() -> int {
    int a;
    if (!is_worst_case) a = dis(gen);
    else a = value++; // worst case: sequential insert
    return a;
  };
  for (int i = 0; i < INIT_DATA_COUNT; i++) {
    int a = gen_value();
    if (init_set.find(a) != init_set.end()) i--;
    else init_set.insert(a), init_list.push_back(a), my_map.insert(a);
  }
  my_map_size.store(INIT_DATA_COUNT);
  for (int i = 0; i < DATA_COUNT_FOR_INSERTION; i++) {
    int a = gen_value();
    if (init_set.find(a) != init_set.end() || random_set_for_insert.find(a) != random_set_for_insert.end()) i--;
    else random_set_for_insert.insert(a), random_list_for_insert.push_back(a);
  }
  std::atomic<bool> write_over(false);
  std::atomic<bool> start_find(false);
  auto write_ope = [&my_map, &my_map_size, &random_list_for_insert, &write_over, &start_find]() {
    start_find.store(true);
    for (const auto ele: random_list_for_insert) {
      my_map.insert(ele);
      my_map_size.fetch_add(+1);
    }
    // start_find.store(true);
    for (int i = random_list_for_insert.size() - 1; i >= 0; i--) {
      my_map.erase(random_list_for_insert[i]);
      my_map_size.fetch_add(-1);
    }
    write_over.store(true);
  };
  std::atomic<int> tot_find_times(0);
  std::atomic<int> success_find_times(0);
  std::atomic<int> perf_find_times(0);
  std::atomic<int> max_try_times(0);
  auto find_ope = [&my_map, &my_map_size, &write_over, &start_find, &init_list, &random_list_for_insert,
                   &tot_find_times, &success_find_times, &perf_find_times, &max_try_times, PERF_MAX_TRY_TIMES, is_worst_case]() {
    while(!start_find.load()) {}
    int idx = 0;
    while(!write_over.load()) {
      int value;
      int local_my_map_size = my_map_size.load();
      if (is_worst_case) idx = local_my_map_size - 1; // worst case: always find the last insert value
      if (idx < INIT_DATA_COUNT) value = init_list[idx];
      else if (idx < local_my_map_size) value = random_list_for_insert[idx - INIT_DATA_COUNT];
      else idx = 0, value = init_list[idx];
      // try_times的次数越多，说明find操作受旋转操作而导致红黑树失效的次数越多，和value存在与否无关。当然，测试时为了方便，保证了value在find时必然存在的。
      int try_times = 0;
      auto result = my_map.findForConcurrentTest(value);
      // RB_ASSERT(result.second != nullptr && result.second->value() == value);
      try_times = result.first;
      tot_find_times.fetch_add(+1);
      success_find_times.fetch_add(+1);
      if (try_times <= PERF_MAX_TRY_TIMES) perf_find_times.fetch_add(+1);
      int local_max_try_times = max_try_times.load();
      do {
        if (local_max_try_times >= try_times) break;
      } while(!max_try_times.compare_exchange_strong(local_max_try_times, try_times));
      idx++;
    }
  };
  std::thread write_thread(write_ope);
  std::vector<std::thread> read_threads;

  for (int i = 0; i < READ_THREAD_COUNT; i++) {
    read_threads.emplace_back(find_ope);
  }

  write_thread.join();
  for (auto& t : read_threads) {
    t.join();
  }
  double tot = tot_find_times.load();
  double success = success_find_times.load();
  double perf = perf_find_times.load();

  double success_rate = success / tot;
  double perf_rate = perf / tot;
  std::cout << "tot = " << tot_find_times.load() << ", success = " << success_find_times.load() << ", perf = " << perf_find_times.load() << ", max try times = " << max_try_times.load() << "\n";
  std::cout << "success rate: " << success_rate * 100.0 << "%\n";
  std::cout << "perf rate: " << perf_rate * 100.0 << "%\n";

  my_map.checkIfSortedListValidForTest();

  int newest_max_height = INT32_MIN;
  int newest_min_height = INT32_MAX;
  int node_count = 0;
  my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
  std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
}

static void TestMultiWriteConcurrentPerf() {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "concurrent test --- multi write" << "\n";
  RBTree<int> my_map;
  g_rbtree = &my_map;

  std::set<int> std_set;

  const int WRITE_THREAD_COUNT = 10;
  const int BATCH_SIZE_PER_THREAD = 1000;
  const int ELEMENT_SIZE_PER_BATCH = 1000;
  std::random_device rd;          // Áî®‰∫éÁîüÊàêÁúüÈöèÊú∫ÁßçÂ≠êÔºàÂ¶? /dev/urandomÔº?
  std::mt19937 gen(rd());         // ‰ΩøÁî®Ê¢ÖÊ£ÆÊóãËΩ¨ÁÆóÊ≥ï‰Ωú‰∏∫ÂºïÊìé
  std::uniform_int_distribution<> dis(INT32_MIN, INT32_MAX); // ÁîüÊàê [1, 100] ÁöÑÂùáÂåÄÊï¥Êï∞
  int value = 0;
  auto gen_value = [&gen, &dis, &value]() -> int {
    int a;
    a = dis(gen);
    return a;
  };
  std::set<int> total_eles;
  auto gen_a_batch = [&total_eles, &gen_value, ELEMENT_SIZE_PER_BATCH]() -> std::vector<int> {
    std::vector<int> vec;
    for (int i = 0; i < ELEMENT_SIZE_PER_BATCH; i++) {
      int a = gen_value();
      if (total_eles.find(a) != total_eles.end()) i--;
      else vec.push_back(a), total_eles.insert(a);
    }
    return vec;
  };
  auto gen_a_thread_data = [&gen_a_batch, BATCH_SIZE_PER_THREAD]() -> std::vector<std::vector<int>> {
    std::vector<std::vector<int>> vec;
    for (int i = 0; i < BATCH_SIZE_PER_THREAD; i++) {
      vec.push_back(gen_a_batch());
    }
    return vec;
  };
  std::vector<std::vector<std::vector<int>>> thread_datas;
  for (int i = 0; i < WRITE_THREAD_COUNT; i++) {
    thread_datas.push_back(gen_a_thread_data());
  }
  std::atomic<int> thread_idx(0);
  auto write_ope = [&my_map, &thread_idx, &thread_datas]() {
    int idx = thread_idx.fetch_add(+1);
    RB_ASSERT(idx < thread_datas.size());
    const std::vector<std::vector<int>>& my_data = thread_datas[idx];
    for (int i = 0; i < my_data.size(); i++) {
      const std::vector<int>& batch_data = my_data[i];
      // 1. insert the batch data.
      for (int ele: batch_data) {
        my_map.insert(ele);
        // std_set.insert(ele);
      }
      // // 2. find the insert batch data.
      // for (int ele: batch_data) {
      //   auto result = my_map.findForConcurrentTest(ele);
      //   RB_ASSERT(result.second != nullptr && result.second->value() == ele);
      // }
      // 3. erase 1 batch every 2 batches.
      if (i % 2 == 0) {
        for (int ele: batch_data) {
          my_map.erase(ele);
          // std_set.erase(ele);
        }
        // // find again.
        // for (int ele: batch_data) {
        //   auto result = my_map.findForConcurrentTest(ele);
        //   RB_ASSERT(result.second == nullptr);
        // }
      }
    }
  };
  std::vector<std::thread> write_threads;

  auto t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < WRITE_THREAD_COUNT; i++) {
    write_threads.emplace_back(write_ope);
  }

  for (auto& t : write_threads) {
    t.join();
  }
  auto t2 = std::chrono::high_resolution_clock::now();
  int time = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  std::cout << "thread count = " << WRITE_THREAD_COUNT << ", total time = " << time << " ms, time per thread = " << time * 1.0 / WRITE_THREAD_COUNT << " ms."<< "\n";

  my_map.checkIfSortedListValidForTest();
  int newest_max_height = INT32_MIN;
  int newest_min_height = INT32_MAX;
  int node_count = 0;
  my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
  std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
}

static void checkCompileMode() {
#if defined(DEBUG)
  std::cout << "DEBUG MODE" << "\n";
#else
  std::cout << "PERF MODE" << "\n";
#endif // DEBUG
}

int main() {
  checkCompileMode();
  // RB_ASSERT(false);
  // TestOneWriteMultiReadConcurrentPerf(2, false);
  // TestOneWriteMultiReadConcurrentPerf(2, true);
  // TestSingleThreadAbility(false);
  // TestSingleThreadAbility(true);
  TestMultiWriteConcurrentPerf();
}
