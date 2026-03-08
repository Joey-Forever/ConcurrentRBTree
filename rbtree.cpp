#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <random>
#include <thread>
#include <cassert>
#include <deque>
#include <iterator>
#include <mutex>
#include <memory>
#include "/Users/caijiajian/Joey-Project-For-Linux/test_perf_dir/test_perf.h"

#ifndef NDEBUG
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
  class NodeRecycler;
  class iterator;
  class Accessor;
  typedef Node* TreeRoot;
  typedef Node* Father;
  typedef Node* LeftSon;
  typedef Node* RightSon;
  typedef Node* ListHeader;
  typedef Node* ListTailer;
  typedef Node* ListNext;

  // using RBTreeType = RBTree<VALUE>;
  using Side = typename Node::Side;
  using Color = typename Node::Color;

  enum OperationType { READ, INSERT, ERASE };

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
        : info(write_type, init_data) {}
    WriteInfo info;
    WriteResult result;
    // std::atomic<bool> done;
  };

  struct Writer {
    explicit Writer()
        : batch_unit(nullptr), accessible(false) {}

    BatchWriteUnit* batch_unit;
    std::atomic<bool> accessible;
  };

 private:
  RBTree() {
    // root_ is never accessible to the user of rbtree.
    root_ = new Node();
    list_header_ = new Node();
    list_tailer_ = new Node();
    // 2. insert list_header_ and list_tailer_ into sorted_list.
    list_header_->setNext(list_tailer_);
    // 3. make list_header_ and list_tailer_ accessible.
    list_header_->setAccessibility(true);
    list_tailer_->setAccessibility(true);
  }

 public:
  ~RBTree() {
    recursiveDestruction(root_->leftSonNoBarrier());
    delete list_header_;
    delete list_tailer_;
    delete root_;
  }

  // caller only can use this static method to create a rbtree instance.
  static std::shared_ptr<RBTree> createInstance() {
    return std::shared_ptr<RBTree>(new RBTree());
  }

  size_t size() const { return size_.load(std::memory_order_relaxed); }
  bool empty() const { return size() == 0; }

  // find the accessible node == value.
  Node* find(const VALUE& value) {
    Node* lower_bound = internalFind(value);
    if (lower_bound == list_tailer_ || lower_bound->value() > value || !lower_bound->accessible()) {
      return nullptr;
    } else {
      // finally find the accessible node == value.
      return lower_bound;
    }
  }

  // find the first accessible node >= value.
  Node* lowerBound(const VALUE& value) {
    Node* lower_bound = internalFind(value);
    while(lower_bound != list_tailer_ && !lower_bound->accessible()) {
      lower_bound = lower_bound->next();
    }
    if (lower_bound == list_tailer_) {
      return nullptr;
    }
    return lower_bound;
  }

  // the insert operation would be ignored if key exists and return the Node* to the existed value,
  // otherwise execute insertion firstly.
  template<typename U>
  std::pair<Node*, bool> insert(U&& insert_value) {
    Node* insert_node = new Node(std::forward<U>(insert_value));
    while (true) {
      // 1. get the estimated_less_bound.
      BatchWriteUnit write_unit(OperationType::INSERT, (void*)insert_node);
      write_unit.info.estimated_less_bound = findEstimatedLessBoundForWrite(insert_node->value());
      while (write_leader_flag_.test_and_set(std::memory_order_acquire)) {
          std::this_thread::yield();
      }

        BatchWriteUnit* write_unitt = &write_unit;
        /* find the exact_less_bound as the insert position in the sorted-list according to estimated_less_bound */
        Node* exact_less_bound = findExactLessBoundForWrite(write_unitt->info.estimated_less_bound, insert_node->value());
        if (exact_less_bound == nullptr) {
          /* 1. fail to find the insert position, retry */
          write_unitt->result.status = WriteStatus::RETRY;
        } else {
          Node* no_less_bound = exact_less_bound->next();
          if (no_less_bound == list_tailer_ || no_less_bound->value() > insert_node->value()) {
            /* 2. insert_node's target_value doesn't exist, execute insert and return the insert_node */
            internalInsert(insert_node, exact_less_bound);
            write_unitt->result.status = WriteStatus::SUCCESS;
            write_unitt->result.magic_node = insert_node;
          } else {
            /* 3. insert_node's target_value already exists, abort insert and return the existed node */
            RB_ASSERT(no_less_bound->value() == insert_node->value());
            write_unitt->result.status = WriteStatus::ABORT;
            write_unitt->result.magic_node = no_less_bound;
          }
        }

      write_leader_flag_.clear(std::memory_order_release);

      if (write_unit.result.status == WriteStatus::RETRY) {
        continue;
      } else if (write_unit.result.status == WriteStatus::ABORT) {
        delete insert_node;
        return {write_unit.result.magic_node, false};
      } else {
        // insert success.
        size_.fetch_add(1, std::memory_order_relaxed);
        return {write_unit.result.magic_node, true};
      }
    }
  }

  // return false if the erase_key doesn't exist.
  bool erase(const VALUE& erase_value) {
    while (true) {
      // 1. get the estimated_less_bound.
      BatchWriteUnit write_unit(OperationType::ERASE, (void*)(&erase_value));
      write_unit.info.estimated_less_bound = findEstimatedLessBoundForWrite(erase_value);
      while (write_leader_flag_.test_and_set(std::memory_order_acquire)) {
          std::this_thread::yield();
      }

        BatchWriteUnit* write_unitt = &write_unit;
        /* find the exact_less_bound as the erase position in the sorted-list according to estimated_less_bound */
        Node* exact_less_bound = findExactLessBoundForWrite(write_unitt->info.estimated_less_bound, erase_value);
        if (exact_less_bound == nullptr) {
          /* 1. fail to find the erase position, retry */
          write_unitt->result.status = WriteStatus::RETRY;
        } else {
          Node* no_less_bound = exact_less_bound->next();
          if (no_less_bound == list_tailer_ || no_less_bound->value() > erase_value) {
            /* 2. target_erase_value doesn't exist, abort erase */
            write_unitt->result.status = WriteStatus::ABORT;
          } else {
            /* 3. target_erase_value exists, execute erase */
            RB_ASSERT(no_less_bound->value() == erase_value);
            internalErase(exact_less_bound);
            write_unitt->result.status = WriteStatus::SUCCESS;
          }
        }

      write_leader_flag_.clear(std::memory_order_release);

      if (write_unit.result.status == WriteStatus::RETRY) {
        continue;
      } else if (write_unit.result.status == WriteStatus::ABORT) {
        return false;
      } else {
        // erase success.
        size_.fetch_sub(1, std::memory_order_relaxed);
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
  }

  Node* getRootForTest() {
    return root_;
  }

 private:
  static const int MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND = 3;
  // no-data node, left son is the true data root.
  TreeRoot root_;
  // no-data list node, as the header of the sorted list for all data node.
  ListHeader list_header_;
  // no-data list node, as the tailer of the sorted list for all data node.
  ListTailer list_tailer_;
  // a recycler for erase-nodes' GC in concurrent access cases.
  Node* place_holder_1_[20];
  NodeRecycler recycler_;
  // only the write thread who get this flag could execute true write operation.
  Node* place_holder_2_[20];
  alignas(64) std::atomic_flag write_leader_flag_ = ATOMIC_FLAG_INIT;
  // total data size.
  Node* place_holder_3_[20];
  alignas(64) std::atomic<size_t> size_{0};


  void recycle(Node* node) { recycler_.add(node); }

  // recursive destruct a sub-tree whose root is curr_node.
  void recursiveDestruction(Node* curr_node) {
    if (curr_node == nullptr) {
      return;
    }
    recursiveDestruction(curr_node->leftSonNoBarrier());
    recursiveDestruction(curr_node->rightSonNoBarrier());
    delete curr_node;
  }

  // return the lower bound of target_value.
  Node* internalFind(const VALUE& target_value) {
    Node* no_less_bound = list_header_;
    // we won't limit the try_times when the extra_steps_to_find_lower_bound over limit again and again.
    // in some very rare cases, try_times would be large but it would finally success finding lowerbound and stop.
    while (true) {
      // find the target_value from the rbtree, and record the less bound(the greatest node < target_value) inside the searching path at the same time.
      Node* less_bound = list_header_;
      Node* curr_node = root_->leftSonNoBarrier();
      while(curr_node != nullptr) {
        if (target_value < curr_node->value()) {
          curr_node = curr_node->leftSonNoBarrier();
        } else if (target_value > curr_node->value()) {
          less_bound = curr_node;
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
        return curr_node;
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
          return no_less_bound;
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
      if (target_value < curr_node->value()) {
        curr_node = curr_node->leftSonNoBarrier();
      } else if (target_value > curr_node->value()) {
        estimated_less_bound = curr_node;
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
    RB_ASSERT((predecessor != list_header_ && predecessor->rightSonNoBarrier() == nullptr) || (successor != list_tailer_ && successor->leftSonNoBarrier() == nullptr) || root_->leftSonNoBarrier() == nullptr);
    if (predecessor != list_header_ && predecessor->rightSonNoBarrier() == nullptr) {
      predecessor->setSonNoBarrier(Node::RIGHT, insert_node);
    } else if (successor != list_tailer_ && successor->leftSonNoBarrier() == nullptr) {
      successor->setSonNoBarrier(Node::LEFT, insert_node);
    } else {
      root_->setSonNoBarrier(Node::LEFT, insert_node);
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
      // 5. finally recycle erase_node.
      recycle(erase_node);
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
      // 7. finally recycle erase_node.
      recycle(erase_node);
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

template <typename VALUE>
class RBTree<VALUE>::Node {
 public:
  enum Side { RIGHT, LEFT };
  enum Color: uint8_t { RED, BLACK };

  // constructor for sentinel nodes, like root_, list_header_ and list_tailer_.
  explicit Node()
    : value_(), accessible_(false),
      is_sentinel_node_(true),
      father_(nullptr), left_son_(nullptr),
      right_son_(nullptr), next_(nullptr) {}

  // constructor for data nodes.
  template<typename U>
  explicit Node(U&& value)
    : value_(std::forward<U>(value)), accessible_(false),
      is_sentinel_node_(false),
      father_(nullptr), left_son_(nullptr),
      right_son_(nullptr), next_(nullptr) {}

  // when a node is going to be destructed, caller MUST make sure firstly that the node
  // is detached from rbtree and that the list-node is detached from sorted list.
  ~Node() = default;

  static void SwapColor(Node* node1, Node* node2) {
    if (node1 != nullptr && node2 != nullptr) {
      std::swap(node1->color_, node2->color_);
    }
  }

  inline VALUE& value() {
    RB_ASSERT(!is_sentinel_node_);
    return value_;
  }

  inline const VALUE& value() const {
    RB_ASSERT(!is_sentinel_node_);
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

  // for caller to find the next accessible data node.
  // return nullptr if next accessible data node is not existed.
  inline Node* accessibleNext() const {
    Node* next_node = next();
    while(!next_node->is_sentinel_node_ && !next_node->accessible()) {
      next_node = next_node->next();
    }
    if (next_node->is_sentinel_node_) {
      return nullptr;
    }
    return next_node;
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
  const bool is_sentinel_node_;

  // pointer region
  Father father_; // ! only used for write case's upward balance.
  std::atomic<LeftSon> left_son_;
  std::atomic<RightSon> right_son_;
  std::atomic<ListNext> next_;

};

// inspired by folly::ConcurrentSkipList.
template <typename VALUE>
class RBTree<VALUE>::NodeRecycler {
 public:
  explicit NodeRecycler() : refs_(0), dirty_(false) {}

  ~NodeRecycler() {
    RB_ASSERT(refs() == 0);
    if (nodes_) {
      for (auto& node : *nodes_) {
        delete node;
      }
    }
  }

  void add(Node* node) {
    std::lock_guard<std::mutex> g(lock_);
    if (nodes_.get() == nullptr) {
      nodes_ = std::make_unique<std::vector<Node*>>(1, node);
    } else {
      nodes_->push_back(node);
    }
    RB_ASSERT(refs() > 0);
    dirty_.store(true, std::memory_order_relaxed);
  }

  int addRef() { return refs_.fetch_add(1, std::memory_order_acq_rel); }

  int releaseRef() {
    // This if statement is purely an optimization. It's possible that this
    // misses an opportunity to delete, but that's OK, we'll try again at
    // the next opportunity. It does not harm the thread safety. For this
    // reason, we can use relaxed loads to make the decision.
    if (!dirty_.load(std::memory_order_relaxed) || refs() > 1) {
      return refs_.fetch_add(-1, std::memory_order_acq_rel);
    }

    std::unique_ptr<std::vector<Node*>> newNodes;
    int ret;
    {
      // The order at which we lock, add, swap, is very important for
      // correctness.
      std::lock_guard<std::mutex> g(lock_);
      ret = refs_.fetch_add(-1, std::memory_order_acq_rel);
      if (ret == 1) {
        // When releasing the last reference, it is safe to remove all the
        // current nodes in the recycler, as we already acquired the lock here
        // so no more new nodes can be added, even though new accessors may be
        // added after this.
        newNodes.swap(nodes_);
        dirty_.store(false, std::memory_order_relaxed);
      }
    }
    // TODO(Joey) should we spawn a thread to do this when there are large
    // number of nodes in the recycler?
    if (newNodes) {
      for (auto& node : *newNodes) {
        delete node;
      }
    }
    return ret;
  }

 private:
  int refs() const { return refs_.load(std::memory_order_relaxed); }

  std::unique_ptr<std::vector<Node*>> nodes_;
  std::atomic<int32_t> refs_; // current number of visitors to the list
  std::atomic<bool> dirty_; // whether *nodes_ is non-empty
  std::mutex lock_; // protects access to *nodes_
};

// Forward iterator for RBTree, following folly::ConcurrentSkipList::iterator interface
template <typename VALUE>
class RBTree<VALUE>::iterator {
 public:
  using value_type = VALUE;
  using reference = value_type&;
  using pointer = value_type*;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  // Default constructor - creates an end iterator
  explicit iterator(Node* node = nullptr) : node_(node) {}

  // Copy constructor
  iterator(const iterator& other) : node_(other.node_) {}

  // Assignment operator
  iterator& operator=(const iterator& other) {
    if (this != &other) {
      node_ = other.node_;
    }
    return *this;
  }

  // Destructor
  ~iterator() = default;

  // Prefix increment
  iterator& operator++() {
    if (node_ != nullptr) {
      // Use accessibleNext() for range iteration as specified
      // This skips to the next accessible data node
      node_ = node_->accessibleNext();
    }
    return *this;
  }

  // Postfix increment
  iterator operator++(int) {
    iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  // Dereference operator
  reference operator*() const {
    // RBTree node uses value() method which maps to SkipList's data()
    return node_->value();
  }

  // Arrow operator
  pointer operator->() const {
    return &(operator*());
  }

  // Equality comparison
  bool operator==(const iterator& other) const {
    return node_ == other.node_;
  }

  // Inequality comparison
  bool operator!=(const iterator& other) const {
    return !(*this == other);
  }

  // Returns the size of the node in bytes
  // Note: RBTree nodes don't have a height() method like SkipList nodes
  // This is a rough approximation based on node structure
  size_t nodeSize() const {
    if (node_ == nullptr) {
      return 0;
    }
    // RBTree node size approximation
    // Node contains: value_, color_, accessible_, is_sentinel_node_,
    // father_, left_son_, right_son_, next_
    return sizeof(Node);
  }

 private:
  Node* node_;
};

// inspired by folly::ConcurrentSkipList::Accessor.
template <typename VALUE>
class RBTree<VALUE>::Accessor {
 public:
  // using RBTreeType = RBTree<VALUE>;
  using value_type = VALUE;

  explicit Accessor(std::shared_ptr<RBTree> rbtree)
      : rbTreeHolder_(std::move(rbtree)) {
    rbtree_ = rbTreeHolder_.get();
    RB_ASSERT(rbtree_ != nullptr);
    rbtree_->recycler_.addRef();
  }

  // Unsafe initializer: the caller assumes the responsibility to keep
  // rbtree valid during the whole life cycle of the Accessor.
  explicit Accessor(RBTree* rbtree) : rbtree_(rbtree) {
    RB_ASSERT(rbtree_ != nullptr);
    rbtree_->recycler_.addRef();
  }

  Accessor(const Accessor& accessor)
      : rbtree_(accessor.rbtree_), rbTreeHolder_(accessor.rbTreeHolder_) {
    rbtree_->recycler_.addRef();
  }

  Accessor& operator=(const Accessor& accessor) {
    if (this != &accessor) {
      rbTreeHolder_ = accessor.rbTreeHolder_;
      rbtree_->recycler_.releaseRef();
      rbtree_ = accessor.rbtree_;
      rbtree_->recycler_.addRef();
    }
    return *this;
  }

  ~Accessor() { rbtree_->recycler_.releaseRef(); }

  bool empty() const { return rbtree_->empty(); }
  size_t size() const { return rbtree_->size(); }
  size_t max_size() const { return std::numeric_limits<size_t>::max(); }

  // returns end() if the value is not in the rbtree, otherwise returns an
  // iterator pointing to the data, and it's guaranteed that the data is valid
  // as far as the Accessor is hold.
  iterator find(const value_type& value) { return iterator(rbtree_->find(value)); }
  size_t count(const value_type& data) const { return find(data) != end(); }

  iterator begin() const { return iterator(rbtree_->list_header_->accessibleNext()); }
  iterator end() const { return iterator(nullptr); }

  // TODO(Joey): implement const_iterator.
  // const_iterator find(const value_type& value) const {
  //   return iterator(rbtree_->find(value));
  // }
  // const_iterator cbegin() const { return begin(); }
  // const_iterator cend() const { return end(); }

  template <typename U>
  std::pair<iterator, bool> insert(U&& data) {
    auto ret = rbtree_->insert(std::forward<U>(data));
    return {iterator(ret.first), ret.second};
  }
  size_t erase(const value_type& data) { return rbtree_->erase(data); }

  iterator lower_bound(const value_type& data) const {
    return iterator(rbtree_->lowerBound(data));
  }

  RBTree* raw_rbtree() const { return rbtree_; }

 private:
  RBTree* rbtree_;
  std::shared_ptr<RBTree> rbTreeHolder_;
};

static void TestSingleThreadAbility(bool sequential_insert) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "single thread test (sequential_insert = " << (sequential_insert ? "true" : "false") << "):\n";
  RBTree<int>::Accessor accessor(RBTree<int>::createInstance());
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
      if (accessor.find(a) != accessor.end()) i--;
      else accessor.insert(a), vec_for_map.push_back(a);
    }
    accessor.raw_rbtree()->checkIfSortedListValidForTest();
    int newest_max_height = INT32_MIN;
    int newest_min_height = INT32_MAX;
    int node_count = 0;
    accessor.raw_rbtree()->getHeightInfoForTest(accessor.raw_rbtree()->getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
    std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
    for (int i = 0; i < 128 * 1024; i++) {
      if (accessor.erase(vec_for_map.front()) == 0) i--;
      else vec_for_map.pop_front();
    }
    newest_max_height = INT32_MIN;
    newest_min_height = INT32_MAX;
    node_count = 0;
    accessor.raw_rbtree()->getHeightInfoForTest(accessor.raw_rbtree()->getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
    std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
    std::cout << "\n";
  }
}

static void TestMultiWriteConcurrentPerf() {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "concurrent test --- multi write" << "\n";
  RBTree<int>::Accessor accessor(RBTree<int>::createInstance());

  std::set<int> std_set;

  const int WRITE_THREAD_COUNT = 8;
  const int BATCH_SIZE_PER_THREAD = 1000;
  const int ELEMENT_SIZE_PER_BATCH = 1000;
  const int BATCH_SIZE_FOR_INIT_DATA = 50;
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
  auto gen_init_data = [&gen_a_batch, BATCH_SIZE_FOR_INIT_DATA]() -> std::vector<std::vector<int>> {
    std::vector<std::vector<int>> vec;
    for (int i = 0; i < BATCH_SIZE_FOR_INIT_DATA; i++) {
      vec.push_back(gen_a_batch());
    }
    return vec;
  };
  std::vector<std::vector<int>> init_data = gen_init_data();
  // init the rbtree, init data size = BATCH_SIZE_FOR_INIT_DATA * ELEMENT_SIZE_PER_BATCH.
  for (int i = 0; i < init_data.size(); i++) {
    const std::vector<int>& batch_data = init_data[i];
    for (int ele: batch_data) {
      accessor.insert(ele);
    }
  }
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
  auto write_ope = [&accessor, &thread_idx, &thread_datas]() {
    int idx = thread_idx.fetch_add(+1);
    RB_ASSERT(idx < thread_datas.size());
    const std::vector<std::vector<int>>& my_data = thread_datas[idx];
    for (int i = 0; i < my_data.size(); i++) {
      const std::vector<int>& batch_data = my_data[i];
      // 1. insert the batch data.
      for (int ele: batch_data) {
        accessor.insert(ele);
        // std_set.insert(ele);
      }
      // 2. find the insert batch data.
      for (int ele: batch_data) {
        auto result = accessor.find(ele);
        RB_ASSERT(result != accessor.end() && *result == ele);
      }
      // 3. erase 1 batch every 2 batches.
      if (i % 2 == 0) {
        for (int ele: batch_data) {
          accessor.erase(ele);
          // std_set.erase(ele);
        }
        // find again.
        for (int ele: batch_data) {
          auto result = accessor.find(ele);
          RB_ASSERT(result == accessor.end());
        }
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

  accessor.raw_rbtree()->checkIfSortedListValidForTest();
  int newest_max_height = INT32_MIN;
  int newest_min_height = INT32_MAX;
  int node_count = 0;
  accessor.raw_rbtree()->getHeightInfoForTest(accessor.raw_rbtree()->getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
  std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
}

static void checkCompileMode() {
#ifndef NDEBUG
  std::cout << "DEBUG MODE" << "\n";
#else
  std::cout << "PERF MODE" << "\n";
#endif // NDEBUG
}

int main() {
  std::cout << "RBTree Performance Test" << "\n";
  checkCompileMode();
  // RB_ASSERT(false);
  // TestSingleThreadAbility(false);
  // TestSingleThreadAbility(true);
  // TestMultiWriteConcurrentPerf();
  // TestCacheMissRatePerf<RBTree<int>>([]() {
  //   return std::make_unique<RBTree<int>>();
  // }, "RBTree");
  // TestJoeyDataPerf<RBTree<int>>([]() {
  //   return std::make_unique<RBTree<int>>();
  // }, "RBTree");
  TestMultiReadFewWriteConcurrentPerf<RBTree<int>::Accessor>([]() {
    auto rbtree = RBTree<int>::createInstance();
    return std::make_unique<RBTree<int>::Accessor>(rbtree);
  });
}
