#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <random>
#include <mutex>
#include <deque>
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

 public:
  RBTree() {
    // root_ is never accessible to the user of rbtree.
    root_ = new Node();
    list_header_ = new Node();
    list_tailer_ = new Node();
    // 1. insert list_header_ and list_tailer_ into rbtree.
    root_->setSon(Node::LEFT, list_header_);
    list_header_->setSon(Node::RIGHT, list_tailer_);
    list_header_->setColor(Node::BLACK);
    list_tailer_->setColor(Node::RED);
    // 2. insert list_header_ and list_tailer_ into sorted_list.
    list_header_->setNext(list_tailer_);
    // 3. make list_header_ and list_tailer_ accessible.
    list_header_->setAccessibility(true);
    list_tailer_->setAccessibility(true);
  }

  ~RBTree() {
    recursiveDestruction(root_);
  }

  // find the accessible node == value.
  Node* find(const VALUE& value) {
    Node* lower_bound = internalFind(value, OperationType::READ).magic_node;
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
    InternalFindResult result = internalFind(value, OperationType::READ);
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
    Node* lower_bound = internalFind(value, OperationType::READ).magic_node;
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
  Node* insert(U&& insert_value) {
    InternalFindResult result = internalFind(insert_value, OperationType::INSERT);
    bool should_insert = result.should_execute;
    Node* predecessor = result.magic_node;
    if (!should_insert) {
      return predecessor;
    }
    // insert_key doesn't exist, execute insertion.

    Node* successor = predecessor->next();
    Node* insert_node = new Node(std::forward<U>(insert_value));
    insert_node->lock();
    // 1. insert node into sorted list.
    insert_node->setNext(successor);
    predecessor->setNext(insert_node);
    // 2. acquire lock before modifying rbtree's structure.
    lockForModifyTreeStruct();
    // 3. insert node into rbtree.
    RB_ASSERT(predecessor->rightSon() == nullptr || successor->leftSon() == nullptr);
    if (predecessor->rightSon() == nullptr) {
      predecessor->setSon(Node::RIGHT, insert_node);
    } else {
      successor->setSon(Node::LEFT, insert_node);
    }
    // 4. set insert_node accessible after it's existed into sorted-list and rbtree.
    insert_node->setAccessibility(true);
    // 5. execute upward balance.
    balanceTheTreeAfterInsert(insert_node);
    // 6. finally release all locks.
    unlockForModifyTreeStruct();
    predecessor->unlock();
    insert_node->unlock();
    successor->unlock();
    return insert_node;
  }

  // return false if the erase_key doesn't exist.
  bool erase(const VALUE& erase_value) {
    InternalFindResult result = internalFind(erase_value, OperationType::ERASE);
    bool should_erase = result.should_execute;
    Node* predecessor = result.magic_node;
    if (!should_erase) {
      // erase_key doesn't exist, abort erase.
      return false;
    }
    // erase_key exists, execute erase.

    lockForModifyTreeStruct();

    Node* erase_node = predecessor->next();
    if (erase_node->leftSon() == nullptr && erase_node->rightSon() != nullptr) {
      // erase_node only has right son. And the right son must be a leaf node.
      // rotate left to make the erase_node to be a leaf node.
      Node* right_son = erase_node->rightSon();
      rotateLeft(erase_node, erase_node->father());
      Node::SwapColor(erase_node, right_son); // ! swap color to balance the tree
    }else if (erase_node->rightSon() == nullptr && erase_node->leftSon() != nullptr) {
      // erase_node only has left son. And the left son must be a leaf node.
      // rotate right to make the erase_node to be a leaf node.
      Node* left_son = erase_node->leftSon();
      rotateRight(erase_node, erase_node->father());
      Node::SwapColor(erase_node, left_son); // ! swap color to balance the tree
    }
    // here the erase_node must be a leaf node or a non-leaf node with two son.
    if (erase_node->leftSon() == nullptr && erase_node->rightSon() == nullptr) {
      // erase_node is a leaf node, erase directly.

      Node* father_of_erase_node = erase_node->father();
      // 1. make erase_node inaccessible.
      erase_node->setAccessibility(false);
      // 2. detach erase_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_erase_node->setSon(
        father_of_erase_node->leftSon() == erase_node ? Node::LEFT : Node::RIGHT, nullptr);
      // 3. upward balance the rbtree.
      if (erase_node->color() == Node::BLACK && father_of_erase_node != root_) {
        // ensure bro_of_delete_side must exist.
        balanceTheTreeAfterErase(father_of_erase_node, delete_side);
      }
      // 4. detach erase_node from sorted-list.
      predecessor->setNext(erase_node->next());
      // 5. finally delete erase_node.
      // delete erase_node;
    } else {
      // erase_node is a non-leaf node with two son.

      Node* father_of_erase_node = erase_node->father();
      // here right_most_node is the max(e.g. right most) node of the erase_node's left-subtree.
      Node* right_most_node = predecessor;
      Node* left_son_of_right_most_node = right_most_node->leftSon();
      if (left_son_of_right_most_node != nullptr) {
        // rotate right to make the right-most node to be a leaf node.
        rotateRight(right_most_node, right_most_node->father()); // right_most_node's father wouldn't be root_.
        Node::SwapColor(right_most_node, left_son_of_right_most_node); // ! swap color to balance the tree
      }
      // here right_most_node is the max and right-most and leaf node of the erase_node's left-subtree.
      Node* father_of_right_most_node = right_most_node->father();
      // 1. detach right_most_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_right_most_node->setSon(
        father_of_right_most_node->leftSon() == right_most_node ? Node::LEFT : Node::RIGHT, nullptr);
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
      right_most_node->setSon(Node::LEFT, erase_node->leftSon());
      right_most_node->setSon(Node::RIGHT, erase_node->rightSon());
      right_most_node->setColor(erase_node->color());
      father_of_erase_node->setSon(
        father_of_erase_node->leftSon() == erase_node ? Node::LEFT : Node::RIGHT, right_most_node);
      // 6. detach erase_node from sorted-list.
      right_most_node->setNext(erase_node->next());
      // 7. finally delete erase_node.
      // delete erase_node;
    }
    // finally release all locks.
    unlockForModifyTreeStruct();
    predecessor->unlock();
    erase_node->unlock();
    return true;
  }

  void getHeightInfoForTest(Node* curr_node, int curr_height, int& newest_max_height, int& newest_min_height, int& node_count) {
    if (curr_node == nullptr) {
      return;
    }
    node_count++;
    if (curr_node->leftSon() == nullptr && curr_node->rightSon() == nullptr) {
      newest_max_height = std::max(curr_height, newest_max_height);
      newest_min_height = std::min(curr_height, newest_min_height);
      return;
    }
    getHeightInfoForTest(curr_node->leftSon(), curr_height + 1, newest_max_height, newest_min_height, node_count);
    getHeightInfoForTest(curr_node->rightSon(), curr_height + 1, newest_max_height, newest_min_height, node_count);
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
      curr_node->lock();
      curr_node->unlock();
      curr_node = curr_node->next();
    }
    curr_node->lock();
    curr_node->unlock();
  }

  Node* getRootForTest() {
    return root_;
  }

  Node* getTailer() {
    return list_tailer_;
  }

 private:
  static const int MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND = 3;
  // no-data node, left son is the true data root.
  TreeRoot root_;
  // no-data list node, as the header of the sorted list for all data node.
  ListHeader list_header_;
  // no-data list node, as the tailer of the sorted list for all data node.
  ListTailer list_tailer_;
  // any write thread should firstly acquire this mutex before modifying rbtree's structure.
  mutable std::mutex lock_for_modify_tree_struct_;

  friend class Node;

  void lockForModifyTreeStruct() const {
    lock_for_modify_tree_struct_.lock();
  }

  void unlockForModifyTreeStruct() const {
    lock_for_modify_tree_struct_.unlock();
  }

  // recursive destruct a sub-tree whose root is curr_node.
  void recursiveDestruction(Node* curr_node) {
    if (curr_node == nullptr) {
      return;
    }
    recursiveDestruction(curr_node->leftSon());
    recursiveDestruction(curr_node->rightSon());
    delete curr_node;
  }

  // return an InternalFindResult:
  //  read case:
  //    should_execute is always true, magic_node means the lower bound of target_value.
  //  write case:
  //    when should_execute is true, magic_node would store the predecessor. (return *WITH* lock)
  //    when should_execute is false, in insert case magic_node is the existed node. (return *WITHOUT* lock)
  InternalFindResult internalFind(const VALUE& target_value, OperationType type) {
    Node* predecessor_of_no_less_bound = list_header_;
    Node* no_less_bound = list_header_;
    int try_times = 0;
    // we won't limit the try_times when the extra_steps_to_find_lower_bound over limit again and again.
    // in some very rare cases, try_times would be large but it would finally success finding lowerbound and stop.
    while (true) {
      try_times++;
      // find the target_value from the rbtree, and record the less bound(the greatest node < target_value) inside the searching path at the same time.
      Node* less_bound = list_header_;
      Node* curr_node = root_->leftSon();
      while(curr_node != nullptr) {
        if (curr_node == list_header_) {
          curr_node = curr_node->rightSon();
        } else if (curr_node == list_tailer_) {
          curr_node = curr_node->leftSon();
        } else if (target_value < curr_node->value()) {
          curr_node = curr_node->leftSon();
        } else if (target_value > curr_node->value()) {
          if (less_bound == list_header_ || curr_node->value() > less_bound->value()) less_bound = curr_node;
          curr_node = curr_node->rightSon();
        } else {
          // curr_node's value == target_value.
          break;
        }
      }
      // here curr_node is nullptr or equals to target_value.
      RB_ASSERT((curr_node == nullptr || curr_node->value() == target_value) &&
             (less_bound == list_header_ || less_bound->value() < target_value));
      // ! specially optimize for read case.
      if (curr_node != nullptr && type == OperationType::READ) {
        no_less_bound = curr_node;
        break;
      }
      Node* left_son_of_curr_node = curr_node == nullptr ? nullptr : curr_node->leftSon();
      if (curr_node == nullptr || (type != OperationType::READ && left_son_of_curr_node == nullptr)) {
        // find the first node >= target_value across sorted-list.
        predecessor_of_no_less_bound = less_bound;
        if (type != OperationType::READ) predecessor_of_no_less_bound->lock();
        no_less_bound = predecessor_of_no_less_bound->next();
        if (type != OperationType::READ) no_less_bound->lock();
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
          if (type != OperationType::READ) predecessor_of_no_less_bound->unlock();
          predecessor_of_no_less_bound = no_less_bound;
          // here curr thread could safely access the next node because it would never change due to insert/erase by other threads since no_less_bound is locked by curr thread.
          no_less_bound = no_less_bound->next();
          if (type != OperationType::READ) no_less_bound->lock();
        }
        // here no_less_bound never be list_header_
        if ((no_less_bound == list_tailer_ || no_less_bound->value() >= target_value) &&
            (type == OperationType::READ || (predecessor_of_no_less_bound->accessible() && no_less_bound->accessible()))) {
          // all nodes < target_key or finally find the first node >= target_key.
          break;
        } else {
          // extra_steps_to_find_lower_bound over limit. we consider that searching into rbtree failed due to rotate operation.
          if (type != OperationType::READ) {
            predecessor_of_no_less_bound->unlock();
            no_less_bound->unlock();
          }
          predecessor_of_no_less_bound = list_header_;
          no_less_bound = list_header_;
          continue;
        }
      } else {
        RB_ASSERT(type != OperationType::READ); // read thread never be in here.
        // here target_value must equal to curr_node and predecessor must exist inside curr_node's left subtree.
        curr_node = left_son_of_curr_node;
        while (curr_node != nullptr) {
          predecessor_of_no_less_bound = curr_node;
          curr_node = curr_node->rightSon();
        }
        predecessor_of_no_less_bound->lock();
        no_less_bound = predecessor_of_no_less_bound->next();
        no_less_bound->lock();
        if (no_less_bound != list_tailer_ && no_less_bound->value() == target_value && predecessor_of_no_less_bound->accessible() && no_less_bound->accessible()) {
          break;
        } else {
          // here we consider that searching into rbtree failed due to rotate operation.
          predecessor_of_no_less_bound->unlock();
          no_less_bound->unlock();
          predecessor_of_no_less_bound = list_header_;
          no_less_bound = list_header_;
          continue;
        }
      }
    }
    if (type == OperationType::READ) {
      RB_ASSERT(no_less_bound == list_tailer_ || no_less_bound->value() >= target_value);
      return {true, try_times, no_less_bound};
    }
    // ! for write case: here curr thread must lock predecessor_of_no_less_bound and no_less_bound, and both of them are accessible.
    RB_ASSERT(predecessor_of_no_less_bound->next() == no_less_bound &&
           (no_less_bound == list_tailer_ || no_less_bound->value() >= target_value) &&
           predecessor_of_no_less_bound->accessible() && no_less_bound->accessible());
    if (no_less_bound == list_tailer_ || no_less_bound->value() > target_value) {
      // target_value does not exist.
      if (type == OperationType::INSERT) {
        // return with locks.
        return {true, try_times, predecessor_of_no_less_bound};
      } else {
        // abort erase, release all locks.
        predecessor_of_no_less_bound->unlock();
        no_less_bound->unlock();
        return {false, try_times, nullptr};
      }
    } else {
      // target_value exists.
      if (type == OperationType::INSERT) {
        // abort insert, release all locks.
        predecessor_of_no_less_bound->unlock();
        no_less_bound->unlock();
        return {false, try_times, no_less_bound};
      } else {
        // return with locks.
        return {true, try_times, predecessor_of_no_less_bound};
      }
    }
  }

  inline Node* getBro(Node* my_self, Node* father) const {
    return father->leftSon() == my_self ? father->rightSon() : father->leftSon();
  }

  // after making same side, this method would change color to grand-fa(red), father(black) and son(red).
  // same side means:
  // 1. father's left son is my_self and grand_father's left son is father.
  // 2. father's right son is my_self and grand_father's right son is father.
  inline Side makeTreeGenSameSide(Node* my_self, Node* father, Node* grand_father) {
    if (grand_father->rightSon() == father) {
      if (father->leftSon() == my_self) rotateRight(father, grand_father);
      grand_father->setColor(Node::RED);
      grand_father->rightSon()->setColor(Node::BLACK);
      grand_father->rightSon()->rightSon()->setColor(Node::RED);
      return Node::RIGHT;
    }
    if (father->rightSon() == my_self) rotateLeft(father, grand_father);
    grand_father->setColor(Node::RED);
    grand_father->leftSon()->setColor(Node::BLACK);
    grand_father->leftSon()->leftSon()->setColor(Node::RED);
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
    Node* left_son = node->leftSon();
    if (left_son == nullptr) {
      return nullptr;
    }
    node->setSon(Node::LEFT, left_son->rightSon());
    left_son->setSon(Node::RIGHT, node);
    father->setSon(
      father->leftSon() == node ? Node::LEFT : Node::RIGHT, left_son);
    return left_son;
  }

  // rotate-left the subtree rooted on node.
  // !only rotate, NOT change color.
  inline Node* rotateLeft(Node* node, Node* father) {
    Node* right_son = node->rightSon();
    if (right_son == nullptr) {
      return nullptr;
    }
    node->setSon(Node::RIGHT, right_son->leftSon());
    right_son->setSon(Node::LEFT, node);
    father->setSon(father->leftSon() == node ? Node::LEFT : Node::RIGHT, right_son);
    return right_son;
  }

  inline Node* broOfDeleteSide(Node* father, Side delete_side) {
    if (delete_side == Node::LEFT) return father->rightSon();
    else return father->leftSon();
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
    if ((bro_of_delete_side->leftSon() == nullptr || bro_of_delete_side->leftSon()->color() == Node::BLACK) &&
        (bro_of_delete_side->rightSon() == nullptr || bro_of_delete_side->rightSon()->color() == Node::BLACK)) {
      // bro can safely be colored with RED.
      bro_of_delete_side->setColor(Node::RED);
      if (increased_height || grand_fa == root_) {
        return;
      } else {
        return balanceTheTreeAfterErase(grand_fa, grand_fa->leftSon() == father_of_erase_node ? Node::LEFT : Node::RIGHT);
      }
    }
    // here either bro's left son or right son is RED (or both RED).
    // for example, if delete_side is LEFT, we need to ensure the RED node is bro's right son, and vice versa.
    // and then we can use the RED node to balance the fa subtree.
    if (delete_side == Node::LEFT && (bro_of_delete_side->rightSon() == nullptr || bro_of_delete_side->rightSon()->color() == Node::BLACK)) {
      Node::SwapColor(bro_of_delete_side, bro_of_delete_side->leftSon());
      rotateRight(bro_of_delete_side, father_of_erase_node);
      bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
      // now bro's right son is RED, we use it.
    } else if (delete_side == Node::RIGHT && (bro_of_delete_side->leftSon() == nullptr || bro_of_delete_side->leftSon()->color() == Node::BLACK)) {
      Node::SwapColor(bro_of_delete_side, bro_of_delete_side->rightSon());
      rotateLeft(bro_of_delete_side, father_of_erase_node);
      bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
      // now bro's left son is RED, we use it.
    }
    if (delete_side == Node::LEFT) {
      bro_of_delete_side->rightSon()->setColor(Node::BLACK);
      rotateLeft(father_of_erase_node, grand_fa);
    } else {
      bro_of_delete_side->leftSon()->setColor(Node::BLACK);
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
    father_.store(nullptr);
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

  inline bool accessible() const { return accessible_.load(); }

  inline void setAccessibility(bool accessible) { accessible_.store(accessible); }

  inline void lock() const {
    mutex_.lock();
  }

  inline void unlock() const {
    mutex_.unlock();
  }

  inline Node* father() const {
    return father_.load();
  }

  // inline void setFather(Node* new_father) {
  //   father_.store(new_father);
  // }

  inline Node* leftSon() const {
    return left_son_.load();
  }

  inline Node* rightSon() const {
    return right_son_.load();
  }

  inline Side setSon(Side side, Node* new_son) {
    if (side == LEFT) left_son_.store(new_son);
    else right_son_.store(new_son);
    // ! In fact, when this node's son changes to new_son, new_son's father would change to this node too,
    // ! so we could update new_son's father here and it's no need to define a public setFather method.
    // ! we have no need to worry about old_son's new father, because when we insert it into rbtree again,
    // ! its new_father would call setSon method and then set old_son's new father at the same time.
    // ! *DO NOT* set old_son's father to nullptr !!!
    if (new_son != nullptr) new_son->father_.store(this);
    return side;
  }

  inline Node* next() const {
    return next_.load();
  }

  inline void setNext(ListNext new_next) {
    next_.store(new_next);
  }

 private:
  // data region
  VALUE value_;
  Color color_;
  std::atomic<bool> accessible_; // whether the node is accessible to the user of rbtree.
  mutable std::mutex mutex_; // lock for sorted-list searching condition.

  // pointer region
  std::atomic<Father> father_; // ! only used for write case's upward balance.
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
    // for (int i = random_list_for_insert.size() - 1; i >= 0; i--) {
    //   my_map.erase(random_list_for_insert[i]);
    //   my_map_size.fetch_add(-1);
    // }
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
      RB_ASSERT(result.second != nullptr && result.second->value() == value);
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

  const int WRITE_THREAD_COUNT = 5;
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
      }
      // 2. find the insert batch data.
      for (int ele: batch_data) {
        auto result = my_map.findForConcurrentTest(ele);
        RB_ASSERT(result.second != nullptr && result.second->value() == ele);
      }
      // 3. erase 1 batch every 2 batches.
      if (i % 2 != 0) {
        for (int ele: batch_data) {
          my_map.erase(ele);
        }
        // find again.
        for (int ele: batch_data) {
          auto result = my_map.findForConcurrentTest(ele);
          RB_ASSERT(result.second == nullptr);
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

  my_map.checkIfSortedListValidForTest();
  int newest_max_height = INT32_MIN;
  int newest_min_height = INT32_MAX;
  int node_count = 0;
  my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
  std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
}

int main() {
  // RB_ASSERT(false);
  // TestOneWriteMultiReadConcurrentPerf(2, false);
  // TestOneWriteMultiReadConcurrentPerf(2, true);
  // TestSingleThreadAbility(false);
  // TestSingleThreadAbility(true);
  TestMultiWriteConcurrentPerf();
}
