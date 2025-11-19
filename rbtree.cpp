#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <chrono>
#include <random>

// type VALUE must define move constructor.
template <typename VALUE>
class RefCountedValue {
 public:
  RefCountedValue(VALUE&& val) : value_(std::move(val)), ref_count_(0U) {}

  void addRef() {
    ref_count_.fetch_add(1U);
  }

  void unref() {
    if (ref_count_.fetch_sub(1U) == ((1U << 31) | 1U)) {
      // last thread to access the value and the value is marked as destructible,
      // so the RefCountedValue can be deleted safely.
      delete this;
    }
  }

  // when a thread call this method, it should firstly make sure no other threads
  // will directly access this RefCountedValue instance (e.g. raw ptr) in the future.
  void markDestructible() {
    if ((ref_count_.fetch_or(1U << 31) & ~(1U << 31)) == 0U) {
      // every accesser-threads have already unrefed, so we could delete this
      // instance safely because it's destructible.
      delete this;
    }
  }

  VALUE* rawPtr() {
    return &value_;
  }

 private:
  RefCountedValue(const RefCountedValue&) = delete;
  RefCountedValue& operator=(const RefCountedValue&) = delete;
  RefCountedValue(RefCountedValue&&) = delete;
  RefCountedValue& operator=(RefCountedValue&&) = delete;

  VALUE value_;
  // top 1 bit used to mark if destructible or not, tail 31 bits used to
  // count the accessers.
  std::atomic<uint32_t> ref_count_;
};

// through ValueAccessPtr, each thread could access the value safely after finding it without worrying about deletion.
// before using, caller must firstly check if it is null or not, just like the raw ptr.
template <typename VALUE>
class ValueAccessPtr {
 public:
  ValueAccessPtr(RefCountedValue<VALUE>* ref_cnt_value) : ref_cnt_value_(ref_cnt_value) {
    if (ref_cnt_value_ != nullptr) {
      ref_cnt_value_->addRef();
    }
  }

  // copy constructor
  ValueAccessPtr(const ValueAccessPtr<VALUE>& other) : ref_cnt_value_(other.ref_cnt_value_) {
    if (ref_cnt_value_ != nullptr) {
      ref_cnt_value_->addRef();
    }
  }

  // move constructor
  ValueAccessPtr(ValueAccessPtr&& other) noexcept : ref_cnt_value_(other.ref_cnt_value_) {
    other.ref_cnt_value_ = nullptr;
  }

  ~ValueAccessPtr() {
    if (ref_cnt_value_ != nullptr) {
      ref_cnt_value_->unref();
    }
  }

  VALUE* operator->() const {
    return ref_cnt_value_->rawPtr();
  }

  VALUE& operator*() const {
    return *ref_cnt_value_->rawPtr();
  }

  operator bool() const {
    return ref_cnt_value_ != nullptr;
  }

 private:
  ValueAccessPtr& operator=(const ValueAccessPtr&) = delete;
  ValueAccessPtr& operator=(ValueAccessPtr&&) = delete;

  RefCountedValue<VALUE>* ref_cnt_value_;
};

enum RangeBoundaryStat { NONE, CLOSE, OPEN };

// type KEY must define default constructor.
template <typename KEY>
struct RangeBoundary {
  RangeBoundaryStat stat;
  KEY key;

  // if stat is NONE, key is prefered to be null.
  RangeBoundary(RangeBoundaryStat st, KEY k = KEY()) : stat(st), key(k) {}
};

// single thread rbtree.
// type KEY must implement operator< and operator==.
// type KEY and type VALUE must define default constructor.
// type VALUE must define move constructor.
template <typename KEY, typename VALUE>
class RBTree {
 private:
  class ListNode;
  class Node;
  typedef Node* TreeRoot;
  typedef Node* LeftSon;
  typedef Node* RightSon;
  typedef ListNode* ListHeader;
  typedef ListNode* ListTailer;
  typedef ListNode* ListNext;

 public:
  RBTree() {
    root_ = new Node();
    list_header_ = new ListNode();
    list_tailer_ = new ListNode();
    list_header_->setNext(list_tailer_);
  }

  ~RBTree() {
    recursiveDestruction(root_);
    delete list_header_;
    delete list_tailer_;
  }

  // return nullptr if key is not existed.
  ValueAccessPtr<VALUE> find(const KEY& key) {
    Node* result = internalFind(root_->leftSon(), key);
    if (result == nullptr) {
      return ValueAccessPtr<VALUE>(nullptr);
    }
    return ValueAccessPtr<VALUE>(result->refCntValue());
  }

  std::vector<ValueAccessPtr<VALUE>> rangeFind(const RangeBoundary<KEY>& left_bound, const RangeBoundary<KEY>& right_bound) {
    ListNode* start_list_node = nullptr;
    if (left_bound.stat == RangeBoundaryStat::NONE) {
      // the global min node as the range start.
      start_list_node = list_header_->next();
    } else if (left_bound.stat == RangeBoundaryStat::CLOSE) {
      Node* node = internalLowerBound(root_->leftSon(), left_bound.key, nullptr);
      start_list_node = (node == nullptr) ? nullptr : node->transferToListNode();
    } else if (left_bound.stat == RangeBoundaryStat::OPEN) {
      Node* node = internalUpperBound(root_->leftSon(), left_bound.key, nullptr);
      start_list_node = (node == nullptr) ? nullptr : node->transferToListNode();
    }
    std::vector<ValueAccessPtr<VALUE>> vec;
    while (start_list_node != nullptr && start_list_node != list_tailer_) {
      if ((right_bound.stat == RangeBoundaryStat::CLOSE && start_list_node->key() > right_bound.key) ||
          (right_bound.stat == RangeBoundaryStat::OPEN && start_list_node->key() >= right_bound.key)) {
        break;
      }
      vec.emplace_back(start_list_node->refCntValue());
      start_list_node = start_list_node->next();
    }
    // c++11 compiler RVO optimization.
    return vec;
  }

  // the insert operation would be ignored if key exists and return the ValueAccessPtr to the existed value,
  // otherwise execute insertion firstly.
  ValueAccessPtr<VALUE> insert(const KEY& insert_key, VALUE&& value) {
    std::vector<Node*> insert_path;
    bool should_insert = internalFindInsertPath(insert_key, root_->leftSon(), insert_path);
    if (!should_insert) {
      return ValueAccessPtr<VALUE>(insert_path.back()->refCntValue());
    }
    // insert_key doesn't exist, execute insertion.

    Node* insert_node = createAndAttachLeafNode(insert_key, std::move(value), insert_path);
    // execute upward balance across the insert_path.
    balanceTheTreeAfterInsert(insert_node, insert_path);
    return ValueAccessPtr<VALUE>(insert_node->refCntValue());
  }

  // return false if the erase_key doesn't exist.
  bool erase(const KEY& erase_key) {
    std::vector<Node*> erase_path;
    bool should_erase = internalFindErasePath(erase_key, erase_path);
    if (!should_erase) {
      // erase_key doesn't exist, abort erase.
      return false;
    }
    // erase_key exists, execute erase.

    Node* erase_node = erase_path.back();
    if (erase_node->leftSon() == nullptr && erase_node->rightSon() != nullptr) {
      // erase_node only has right son. And the right son must be a leaf node.
      // rotate left to make the erase_node to be a leaf node.
      Node* right_son = erase_node->rightSon();
      rotateLeft(erase_node, erase_path.size() >= 2 ? erase_path[erase_path.size() - 2] : root_);
      Node::SwapColor(erase_node, right_son); // ! swap color to balance the tree
      erase_path.pop_back();
      erase_path.push_back(right_son);
      erase_path.push_back(erase_node);
    }else if (erase_node->rightSon() == nullptr && erase_node->leftSon() != nullptr) {
      // erase_node only has left son. And the left son must be a leaf node.
      // rotate right to make the erase_node to be a leaf node.
      Node* left_son = erase_node->leftSon();
      rotateRight(erase_node, erase_path.size() >= 2 ? erase_path[erase_path.size() - 2] : root_);
      Node::SwapColor(erase_node, left_son); // ! swap color to balance the tree
      erase_path.pop_back();
      erase_path.push_back(left_son);
      erase_path.push_back(erase_node);
    }
    // here the erase_node must be a leaf node or a non-leaf node with two son.
    // And the erase_node is contained into erase_path too (e.g. erase_node = erase_path.back()).
    if (erase_node->leftSon() == nullptr && erase_node->rightSon() == nullptr) {
      // erase_node is a leaf node, erase directly.

      erase_path.pop_back(); // pop the erase_node
      ListNode* pre_list_node = findPreListNodeFromTreePath(erase_node, erase_path);
      Node* father_of_erase_node = erase_path.empty() ? root_ : erase_path.back();
      if (!erase_path.empty()) erase_path.pop_back();
      Side delete_side = detachAndDeleteLeafNode(erase_node, father_of_erase_node, pre_list_node);
      // upward balance the rbtree.
      balanceTheTreeAfterErase(father_of_erase_node, delete_side, erase_path);
    } else {
      // erase_node is a non-leaf node with two son.

      // push the path between erase_node and his after-node(must be the left-most node of erase_node's right-subtree) into erase_path.
      Node* curr_node = erase_node->rightSon();
      while (curr_node != nullptr) {
        erase_path.push_back(curr_node);
        curr_node = curr_node->leftSon();
      }
      // find the pre-node of erase_node, and the pre_node must be the right-most node of erase_node's left-subtree.
      curr_node = erase_node->leftSon();
      while (curr_node->rightSon() != nullptr) {
        curr_node = curr_node->rightSon();
      }
      ListNode* pre_list_node = curr_node->transferToListNode();
      // here the erase_path.back() is the min(e.g. left most) node of the erase_node's right-subtree.
      Node* left_most_node = erase_path.back();
      Node* right_son_of_left_most_node = left_most_node->rightSon();
      if (right_son_of_left_most_node != nullptr) {
        // rotate left to make the left-most node to be a leaf node.
        rotateLeft(left_most_node, erase_path[erase_path.size() - 2]); // left_most_node's father wouldn't be root_.
        Node::SwapColor(left_most_node, right_son_of_left_most_node); // ! swap color to balance the tree
        erase_path.pop_back();
        erase_path.push_back(right_son_of_left_most_node);
        erase_path.push_back(left_most_node);
      }
      // here the erase_path.back() is the min and left-most and leaf node of the erase_node's right-subtree.
      left_most_node = erase_path.back();
      Node::SwapData(erase_node, left_most_node); // swap the inside data but not change position of physical node.
      // erase_node is the logical node to erase, but the physical node to erase is the left_most_node.
      erase_path.pop_back(); // pop the left_most_node
      Node* father_of_left_most_node = erase_path.back();
      erase_path.pop_back();
      Side delete_side = detachAndDeleteLeafNode(left_most_node, father_of_left_most_node, pre_list_node);
      // upward balance the rbtree.
      balanceTheTreeAfterErase(father_of_left_most_node, delete_side, erase_path); 
    }
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

  Node* getRootForTest() {
    return root_;
  }

 private:
  enum Side { RIGHT, LEFT };

  // no-data node, left son is the true data root.
  TreeRoot root_;
  // no-data list node, as the header of the sorted list for all data node.
  ListHeader list_header_;
  // no-data list node, as the tailer of the sorted list for all data node.
  ListTailer list_tailer_;

  // recursive destruct a sub-tree whose root is curr_node.
  void recursiveDestruction(Node* curr_node) {
    if (curr_node == nullptr) {
      return;
    }
    recursiveDestruction(curr_node->leftSon());
    recursiveDestruction(curr_node->rightSon());
    delete curr_node;
  }

  // find the node == target_key.
  Node* internalFind(Node* curr_node, const KEY& target_key) {
    if (curr_node == nullptr || curr_node->key() == target_key) {
      return curr_node;
    }
    if (target_key < curr_node->key()) {
      return internalFind(curr_node->leftSon(), target_key);
    }
    return internalFind(curr_node->rightSon(), target_key);
  }

  // find first node >= target_key.
  Node* internalLowerBound(Node* curr_node, const KEY& target_key, Node* newest_bound) {
    if (curr_node == nullptr) {
      return newest_bound;
    }
    if (curr_node->key() == target_key) {
      // the lower bound exactly equals to target key.
      return curr_node;
    }
    if (curr_node->key() > target_key) {
      // newest bound updates to curr node.
      return internalLowerBound(curr_node->leftSon(), target_key, curr_node);
    }
    return internalLowerBound(curr_node->rightSon(), target_key, newest_bound);
  }

  // find first node > target_key.
  Node* internalUpperBound(Node* curr_node, const KEY& target_key, Node* newest_bound) {
    if (curr_node == nullptr) {
      return newest_bound;
    }
    if (curr_node->key() > target_key) {
      // newest bound updates to curr node.
      return internalUpperBound(curr_node->leftSon(), target_key, curr_node);
    }
    return internalUpperBound(curr_node->rightSon(), target_key, newest_bound);
  }

  // return false if the insert_key exists, and the existed node would be stored into the vector back.
  // return true if the insert_key doesn't exist, and father of leaf-insert-position would be stored into the vector back.
  bool internalFindInsertPath(const KEY& insert_key, Node* curr_node, std::vector<Node*>& insert_path) {
    if (curr_node == nullptr) {
      // finally find a leaf position to insert.
      return true;
    }
    insert_path.push_back(curr_node);
    if (insert_key == curr_node->key()) {
      // insert_key exists, abort search.
      return false;
    } else if (insert_key < curr_node->key()) {
      return internalFindInsertPath(insert_key, curr_node->leftSon(), insert_path);
    }
    return internalFindInsertPath(insert_key, curr_node->rightSon(), insert_path);
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
      return Side::RIGHT;
    }
    if (father->rightSon() == my_self) rotateLeft(father, grand_father);
    grand_father->setColor(Node::RED);
    grand_father->leftSon()->setColor(Node::BLACK);
    grand_father->leftSon()->leftSon()->setColor(Node::RED);
    return Side::LEFT;
  }

  // this method is a recursive method, and it would execute upward balance across the insert_path.
  void balanceTheTreeAfterInsert(Node* insert_node, std::vector<Node*>& insert_path) {
    if (insert_path.empty()) {
      // the insertion finally causes height increasing.
      insert_node->setColor(Node::BLACK);
      return;
    }
    Node* father = insert_path.back();
    insert_path.pop_back();
    if (father->color() == Node::BLACK) {
      insert_node->setColor(Node::RED);
      return;
    }
    // father color is RED, conditions are more complex.

    // because faher's color is RED, so grand_father must exist and be BLACK.
    Node* grand_father = insert_path.back();
    insert_path.pop_back();
    Node* uncle = getBro(father, grand_father);
    if (uncle == nullptr || uncle->color() == Node::BLACK) {
      Side side = makeTreeGenSameSide(insert_node, father, grand_father);
      if (side == Side::LEFT) rotateRight(grand_father, insert_path.empty() ? root_ : insert_path.back());
      else rotateLeft(grand_father, insert_path.empty() ? root_ : insert_path.back());
      return;
    }
    // uncle is RED too, which would cause upward balance.
    insert_node->setColor(Node::RED);
    father->setColor(Node::BLACK);
    uncle->setColor(Node::BLACK);
    // grand_father as the new insert node, execute upward balance.
    return balanceTheTreeAfterInsert(grand_father, insert_path);
  }

  // rotate-right the subtree rooted on node.
  // !only rotate, NOT change color.
  inline void rotateRight(Node* node, Node* father) {
    Node* left_son = node->leftSon();
    if (left_son == nullptr) {
      return;
    }
    node->setLeftSon(left_son->rightSon());
    left_son->setRightSon(node);
    if (father->leftSon() == node) father->setLeftSon(left_son);
    else father->setRightSon(left_son);
  }

  // rotate-left the subtree rooted on node.
  // !only rotate, NOT change color.
  inline void rotateLeft(Node* node, Node* father) {
    Node* right_son = node->rightSon();
    if (right_son == nullptr) {
      return;
    }
    node->setRightSon(right_son->leftSon());
    right_son->setLeftSon(node);
    if (father->leftSon() == node) father->setLeftSon(right_son);
    else father->setRightSon(right_son);
  }

  // find the nearest list node whose key is less than target_key.
  // caller MUST make sure the target_leaf_node is a LEAF node, otherwise the pre list node may not be inside tree_path.
  // tree_path means the path from tree root to leaf target node.
  // return value always be valid, and it equals to list_header_ if nobody is less than target_leaf_node.
  ListNode* findPreListNodeFromTreePath(Node* target_leaf_node, const std::vector<Node*>& tree_path) {
    ListNode* pre_list_node = list_header_;
    const KEY& target_key = target_leaf_node->key();
    for (Node* curr_node: tree_path) {
      if (curr_node->key() < target_key && (pre_list_node == list_header_ || curr_node->key() > pre_list_node->key())) {
        pre_list_node = curr_node->transferToListNode();
      }
    }
    return pre_list_node;
  }

  inline bool internalFindErasePath(const KEY& erase_key, std::vector<Node*>& erase_path) {
    return !internalFindInsertPath(erase_key, root_->leftSon(), erase_path);
  }

  inline Node* broOfDeleteSide(Node* father, Side delete_side) {
    if (delete_side == Side::LEFT) return father->rightSon();
    else return father->leftSon();
  }

  // recursive method. father_of_erase_node's delete_side-subtree has one node deleted.
  // erase_path doesn't contain father_of_erase_node.
  void balanceTheTreeAfterErase(Node* father_of_erase_node, Side delete_side, std::vector<Node*>& erase_path) {
    Node* bro_of_delete_side = broOfDeleteSide(father_of_erase_node, delete_side);
    if (bro_of_delete_side == nullptr) {
      // two conditions match this if-branch:
      // 1. father_of_erase_node is root_, and finally the height of rbtree reduces.
      // 2. father_of_erase_node and erase_node together form a leaf node in 2-3-4 tree.
      //    now the erase_node is destructed, so father_of_erase_node is the only member of the leaf node
      //    and wouldn't change the height of the 2-3-4 tree.
      return;
    }
    if (father_of_erase_node->color() == Node::BLACK) {
      // father is black.
      if (bro_of_delete_side->color() == Node::BLACK) {
        // reduce the subtree height and upward balance.
        bro_of_delete_side->setColor(Node::RED);
        Node* grand_father_of_erase_node = erase_path.empty() ? root_ : erase_path.back();
        if (!erase_path.empty()) erase_path.pop_back();
        return balanceTheTreeAfterErase(grand_father_of_erase_node,
                 grand_father_of_erase_node->leftSon() == father_of_erase_node ? Side::LEFT : Side::RIGHT, erase_path);
      } else {
        if (bro_of_delete_side->leftSon() == nullptr && bro_of_delete_side->rightSon() == nullptr) {
          // bro is red and leaf, so the erase node MUST be a red leaf too. erase_node and father and bro together
          // form a leaf node in 2-3-4 tree. do nothing and the height of 2-3-4 tree wouldn't change.
          return;
        }
        // here bro is red and MUST have two son.
        if (delete_side == Side::LEFT) {
          Node* left_son_of_bro = bro_of_delete_side->leftSon();
          rotateLeft(father_of_erase_node, erase_path.empty() ? root_ : erase_path.back());
          bro_of_delete_side->setColor(Node::BLACK);
          left_son_of_bro->setColor(Node::RED);
        } else {
          Node* right_son_of_bro = bro_of_delete_side->rightSon();
          rotateRight(father_of_erase_node, erase_path.empty() ? root_ : erase_path.back());
          bro_of_delete_side->setColor(Node::BLACK);
          right_son_of_bro->setColor(Node::RED);
        }
        return;
      }
    } else {
      // father is red.
      Node::SwapColor(father_of_erase_node, bro_of_delete_side);
      return;
    }
  }

  // create a new insert_node, and then attach the node into sorted list and rbtree.
  // caller MUST make sure the insert_key doesn't exist before insert.
  // caller MUST make sure the insert_path includes from root to a leaf node and the leaf node
  // stored into insert_path.back() would become the father of the new insert_node.
  // return the new insert_node.
  Node* createAndAttachLeafNode(const KEY& insert_key, VALUE&& value, const std::vector<Node*>& insert_path) {
    Node* insert_node = new Node(insert_key, std::move(value));
    // 1. insert node into sorted list.
    ListNode* pre_list_node = findPreListNodeFromTreePath(insert_node, insert_path);
    insert_node->transferToListNode()->setNext(pre_list_node->next());
    pre_list_node->setNext(insert_node->transferToListNode());
    // 2. insert node into rbtree.
    if (insert_path.empty()) root_->setLeftSon(insert_node);
    else if (insert_key < insert_path.back()->key()) insert_path.back()->setLeftSon(insert_node);
    else if (insert_key > insert_path.back()->key()) insert_path.back()->setRightSon(insert_node);
    return insert_node;
  }

  // detach the delete_node from sorted list and rbtree, and then destruct the physical node.
  // caller MUST make sure the right relationship between all incoming nodes.
  // caller MUST make sure the delete_node is a leaf node.
  // return which side the delete_node is inside his father.
  Side detachAndDeleteLeafNode(Node* delete_node, Node* father_node, ListNode* pre_list_node) {
    Side delete_side;
    pre_list_node->setNext(delete_node->transferToListNode()->next());
    if (father_node->leftSon() == delete_node) {
      delete_side = Side::LEFT;
      father_node->setLeftSon(nullptr);
    } else {
      delete_side = Side::RIGHT;
      father_node->setRightSon(nullptr);
    }
    delete delete_node;
    return delete_side;
  }
};

// when a ListNode is going to be deleted, the RefCountedValue should be marked as destructible and
// for other thread's safely-access to value, the RefCountedValue wouldn't be deleted immediately
// if the ref-count equals to non-zero.
template <typename KEY, typename VALUE>
class RBTree<KEY, VALUE>::ListNode {
 public:
  ListNode() : key_(), ref_cnt_value_(nullptr), next_(nullptr) {}

  ListNode(const KEY& k, VALUE&& value)
    : key_(k), ref_cnt_value_(new RefCountedValue<VALUE>(std::move(value))), next_(nullptr) {}

  ~ListNode() {
    if (ref_cnt_value_ != nullptr) {
      ref_cnt_value_->markDestructible();
    }
    next_ = nullptr;
  }

  inline const KEY& key() const { return key_; }

  inline RefCountedValue<VALUE>* refCntValue() const { return ref_cnt_value_; }

  inline ListNext next() const { return next_; }

  inline void setNext(ListNext new_next) { next_ = new_next; }

 private:
  const KEY key_;
  // because ref_cnt_value will may be hold for a long time by ValueAccessPtr, but
  // we don't want it effecting the destruction for ListNode.
  RefCountedValue<VALUE>* ref_cnt_value_;
  // // when a list-node need to be deleted, this value provides an atomical way to make
  // // it invisible at the same time in all data-structure . 
  // std::atomic<bool> accessible;

  // next node in the sorted list and also the next kv in rbtree's mid-order-search.
  // for safe concurrent access when erase is happening, each thread accesses the list-node when searching into rbtree
  // should firstly access the next list-node and check its key.
  // when the rbtree node owning the list-node has an right son, the next exactly in it's right subtree,
  // and in that condition the next ptr would act as a backup kv when the list-node is marked as inaccessible.
  ListNext next_;
};

template <typename KEY, typename VALUE>
class RBTree<KEY, VALUE>::Node {
 public:
  enum Color { RED, BLACK };

  Node() : list_node_(nullptr), left_son_(nullptr), right_son_(nullptr) {}

  Node(const KEY& k, VALUE&& value)
    : list_node_(new ListNode(k, std::move(value))),
      left_son_(nullptr), right_son_(nullptr) {}

  // when a node is going to be destructed, caller MUST make sure firstly that the node
  // is detached from rbtree and that the list-node is detached from sorted list.
  ~Node() {
    if (list_node_ != nullptr) {
      delete list_node_;
    }
    left_son_ = nullptr;
    right_son_ = nullptr;
  }

  // we only need to swap the value of list_node_ptr for two nodes, which wouldn't effect the sorted-list's structure.
  static void SwapData(Node* node1, Node* node2) {
    std::swap(node1->list_node_, node2->list_node_);
  }

  static void SwapColor(Node* node1, Node* node2) {
    std::swap(node1->color_, node2->color_);
  }

  // when callers need to access the sorted list, they can call this method to get the list node.
  inline ListNode* transferToListNode() const { return list_node_; }

  inline const KEY& key() const { return list_node_->key(); }

  inline RefCountedValue<VALUE>* refCntValue() const { return list_node_->refCntValue(); }

  inline Color color() const { return color_; }

  inline void setColor(Color new_color) { color_ = new_color; }

  inline LeftSon leftSon() const { return left_son_; }

  inline void setLeftSon(LeftSon new_left_son) { left_son_ = new_left_son; }

  inline RightSon rightSon() const { return right_son_; }

  inline void setRightSon(RightSon new_right_son) { right_son_ = new_right_son; }

 private:
  // data region
  ListNode* list_node_; // the kv data is stored into ListNode.
  Color color_;

  // pointer region
  LeftSon left_son_;
  RightSon right_son_;
};

int main() {
  RBTree<int, int> my_map;
  for (int i = 0; i < 100; i++) my_map.insert(i, std::move(i));
  my_map.erase(28);
  my_map.erase(25);
  auto vec = my_map.rangeFind(RangeBoundary<int>(RangeBoundaryStat::CLOSE, 21), RangeBoundary<int>(RangeBoundaryStat::OPEN, 30));
  for (ValueAccessPtr<int>& ptr: vec) {
    std::cout << *ptr << " ";
  }
  std::cout << "\n";
}
