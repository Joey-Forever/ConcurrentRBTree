#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <random>
#include <shared_mutex>
#include <deque>
#include <thread>

// single thread rbtree.
// type KEY must implement operator< and operator==.
// type KEY and type VALUE must define default constructor.
// type VALUE must define move constructor.
template <typename VALUE>
class RBTree {
 public:
  class Node;
  typedef Node* TreeRoot;
  typedef Node* LeftSon;
  typedef Node* RightSon;
  typedef Node* ListHeader;
  typedef Node* ListTailer;
  typedef Node* ListNext;

  using Side = typename Node::Side;
  using Color = typename Node::Color;

 public:
  RBTree() {
    root_ = new Node();
    list_header_ = new Node();
    list_tailer_ = new Node();
    list_header_->setNext(list_tailer_);
  }

  ~RBTree() {
    recursiveDestruction(root_);
    delete list_header_;
    delete list_tailer_;
  }

  // find the node == value.
  Node* find(const VALUE& value) {
    // return internalFind(root_->leftSon(), value);
    Node* lower_bound = internalLowerbound(value);
    if (lower_bound == nullptr || lower_bound->value() > value) {
      return nullptr;
    } else {
      // finally find the node == value.
      return lower_bound;
    }
  }

  // value must exist.
  bool findForConcurrentTest(const VALUE& value) {
    Node* no_greater_bound = internalNoGreaterBound(root_->leftSon(), value, nullptr);
    if (no_greater_bound == nullptr || no_greater_bound->value() != value) {
      return false;
    } else {
      return true;
    }
  }

  // find the first node >= value.
  Node* lowerBound(const VALUE& value) {
    return internalLowerbound(value);
  }

  // the insert operation would be ignored if key exists and return the Node* to the existed value,
  // otherwise execute insertion firstly.
  template<typename U>
  Node* insert(U&& insert_value) {
    std::vector<Node*> insert_path;
    bool should_insert = internalFindInsertPath(insert_value, root_->leftSon(), insert_path);
    if (!should_insert) {
      return insert_path.back();
    }
    // insert_key doesn't exist, execute insertion.

    Node* insert_node = createAndAttachLeafNode(std::forward<U>(insert_value), insert_path);
    // execute upward balance across the insert_path.
    balanceTheTreeAfterInsert(insert_node, insert_path);
    return insert_node;
  }

  // return false if the erase_key doesn't exist.
  bool erase(const VALUE& erase_value) {
    std::vector<Node*> erase_path;
    bool should_erase = internalFindErasePath(erase_value, erase_path);
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
      Node* pre_list_node = findPreListNodeFromTreePath(erase_node, erase_path);
      Node* father_of_erase_node = erase_path.empty() ? root_ : erase_path.back();
      if (!erase_path.empty()) erase_path.pop_back(); // pop the father_of_erase_node
      // 1. detach erase_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_erase_node->setSon(
        father_of_erase_node->leftSon() == erase_node ? Node::LEFT : Node::RIGHT, nullptr);
      // 2. upward balance the rbtree.
      if (erase_node->color() == Node::BLACK && father_of_erase_node != root_) {
        // ensure bro_of_delete_side must exist.
        balanceTheTreeAfterErase(father_of_erase_node, delete_side, erase_path);
      }
      // 3. detach erase_node from sorted-list.
      pre_list_node->setNext(erase_node->next());
      // 4. finally delete erase_node.
      delete erase_node;
    } else {
      // erase_node is a non-leaf node with two son.

      Node* father_of_erase_node = erase_path.size() < 2 ? root_ : erase_path[erase_path.size() - 2];
      // push the path between erase_node and his pre-node(must be the right-most node of erase_node's left-subtree) into erase_path.
      Node* curr_node = erase_node->leftSon();
      while (curr_node != nullptr) {
        erase_path.push_back(curr_node);
        curr_node = curr_node->rightSon();
      }
      // here the erase_path.back() is the max(e.g. right most) node of the erase_node's left-subtree.
      Node* right_most_node = erase_path.back();
      Node* left_son_of_right_most_node = right_most_node->leftSon();
      if (left_son_of_right_most_node != nullptr) {
        // rotate right to make the right-most node to be a leaf node.
        rotateRight(right_most_node, erase_path[erase_path.size() - 2]); // right_most_node's father wouldn't be root_.
        Node::SwapColor(right_most_node, left_son_of_right_most_node); // ! swap color to balance the tree
        erase_path.pop_back();
        erase_path.push_back(left_son_of_right_most_node);
        erase_path.push_back(right_most_node);
      }
      // here the erase_path.back() is the max and right-most and leaf node of the erase_node's left-subtree.
      right_most_node = erase_path.back();
      erase_path.pop_back(); // pop the right_most_node
      Node* father_of_right_most_node = erase_path.back();
      erase_path.pop_back(); // pop the father_of_right_most_node
      // 1. detach right_most_node from rbtree but keep being attached into sorted-list.
      Side delete_side = father_of_right_most_node->setSon(
        father_of_right_most_node->leftSon() == right_most_node ? Node::LEFT : Node::RIGHT, nullptr);
      // 2. upward balance the rbtree.
      if (right_most_node->color() == Node::BLACK && father_of_right_most_node != root_) {
        // ensure bro_of_delete_side must exist.
        balanceTheTreeAfterErase(father_of_right_most_node, delete_side, erase_path);
      }
      // 3. find the erase_node for removing.
      erase_path.clear();
      internalFindErasePath(erase_value, erase_path);
      erase_node = erase_path.back();
      erase_path.pop_back();
      father_of_erase_node = erase_path.empty() ? root_ : erase_path.back();
      // 4. replace erase_node with right_most_node on erase_node's position in rbtree.
      right_most_node->setSon(Node::LEFT, erase_node->leftSon());
      right_most_node->setSon(Node::RIGHT, erase_node->rightSon());
      right_most_node->setColor(erase_node->color());
      father_of_erase_node->setSon(
        father_of_erase_node->leftSon() == erase_node ? Node::LEFT : Node::RIGHT, right_most_node);
      // 5. detach erase_node from sorted-list.
      right_most_node->setNext(erase_node->next());
      // 6. finally delete erase_node.
      delete erase_node;
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

  Node* getTailer() {
    return list_tailer_;
  }

 private:
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

  // find node == target_key.
  Node* internalFind(Node* curr_node, const VALUE& target_value) {
    if (curr_node == nullptr) {
      return nullptr;
    }
    if (curr_node->value() == target_value) {
      // the curr_node exactly equals to target key.
      return curr_node;
    } else if (curr_node->value() < target_value) {
      return internalFind(curr_node->rightSon(), target_value);
    }else {
      return internalFind(curr_node->leftSon(), target_value);
    }
  }

  // find the first node >= target_value.
  Node* internalLowerbound(const VALUE& target_value) {
    Node* no_greater_bound = internalNoGreaterBound(root_->leftSon(), target_value, nullptr);
    // find the first node >= target_value across sorted-list.
    Node* no_less_bound = no_greater_bound == nullptr ? list_header_ : no_greater_bound;
    while (no_less_bound != list_tailer_ &&
           (no_less_bound == list_header_ || no_less_bound->value() < target_value)) {
      no_less_bound = no_less_bound->next();
    }
    if (no_less_bound == list_tailer_) {
      return nullptr;
    } else {
      // finally find the first node >= target_key.
      return no_less_bound;
    }
  }

  // find last node <= target_key.
  Node* internalNoGreaterBound(Node* curr_node, const VALUE& target_value, Node* newest_bound) {
    if (curr_node == nullptr) {
      return newest_bound;
    }
    if (curr_node->value() == target_value) {
      // the no-greater bound exactly equals to target key.
      return curr_node;
    }
    if (curr_node->value() < target_value) {
      // newest bound updates to curr node.
      return internalNoGreaterBound(curr_node->rightSon(), target_value, curr_node);
    }
    return internalNoGreaterBound(curr_node->leftSon(), target_value, newest_bound);
  }

  // return false if the insert_key exists, and the existed node would be stored into the vector back.
  // return true if the insert_key doesn't exist, and father of leaf-insert-position would be stored into the vector back.
  bool internalFindInsertPath(const VALUE& insert_value, Node* curr_node, std::vector<Node*>& insert_path) {
    if (curr_node == nullptr) {
      // finally find a leaf position to insert.
      return true;
    }
    insert_path.push_back(curr_node);
    if (insert_value == curr_node->value()) {
      // insert_key exists, abort search.
      return false;
    } else if (insert_value < curr_node->value()) {
      return internalFindInsertPath(insert_value, curr_node->leftSon(), insert_path);
    }
    return internalFindInsertPath(insert_value, curr_node->rightSon(), insert_path);
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
      if (side == Node::LEFT) rotateRight(grand_father, insert_path.empty() ? root_ : insert_path.back());
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

  // find the nearest list node whose key is less than target_key.
  // caller MUST make sure the target_leaf_node is a LEAF node, otherwise the pre list node may not be inside tree_path.
  // tree_path means the path from tree root to leaf target node.
  // return value always be valid, and it equals to list_header_ if nobody is less than target_leaf_node.
  Node* findPreListNodeFromTreePath(Node* target_leaf_node, const std::vector<Node*>& tree_path) {
    Node* pre_list_node = list_header_;
    const VALUE& target_value = target_leaf_node->value();
    for (Node* curr_node: tree_path) {
      if (curr_node->value() < target_value && (pre_list_node == list_header_ || curr_node->value() > pre_list_node->value())) {
        pre_list_node = curr_node;
      }
    }
    return pre_list_node;
  }

  inline bool internalFindErasePath(const VALUE& erase_value, std::vector<Node*>& erase_path) {
    return !internalFindInsertPath(erase_value, root_->leftSon(), erase_path);
  }

  inline Node* broOfDeleteSide(Node* father, Side delete_side) {
    if (delete_side == Node::LEFT) return father->rightSon();
    else return father->leftSon();
  }

  // recursive method. father_of_erase_node's delete_side-subtree has one node deleted.
  // erase_path doesn't contain father_of_erase_node.
  void balanceTheTreeAfterErase(Node* father_of_erase_node, Side delete_side, std::vector<Node*>& erase_path) {
    // delete_side's subtree must be null or rooted with a BLACK node.
    // bro_of_delete_side must exist.
    bool increased_height;
    Node* grand_fa = erase_path.empty() ? root_ : erase_path.back();
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
      if (increased_height || erase_path.empty()) {
        return;
      } else {
        Node* grand_fa = erase_path.back();
        erase_path.pop_back();
        return balanceTheTreeAfterErase(grand_fa, grand_fa->leftSon() == father_of_erase_node ? Node::LEFT : Node::RIGHT, erase_path);
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

  // create a new insert_node, and then attach the node into sorted list and rbtree.
  // caller MUST make sure the insert_key doesn't exist before insert.
  // caller MUST make sure the insert_path includes from root to a leaf node and the leaf node
  // stored into insert_path.back() would become the father of the new insert_node.
  // return the new insert_node.
  template<typename U>
  Node* createAndAttachLeafNode(U&& value, const std::vector<Node*>& insert_path) {
    Node* insert_node = new Node(std::forward<U>(value));
    // 1. insert node into sorted list.
    Node* pre_list_node = findPreListNodeFromTreePath(insert_node, insert_path);
    insert_node->setNext(pre_list_node->next());
    pre_list_node->setNext(insert_node);
    // 2. insert node into rbtree.
    if (insert_path.empty()) root_->setSon(Node::LEFT, insert_node);
    else if (value < insert_path.back()->value()) insert_path.back()->setSon(Node::LEFT, insert_node);
    else if (value > insert_path.back()->value()) insert_path.back()->setSon(Node::RIGHT, insert_node);
    return insert_node;
  }
};

template <typename VALUE>
class RBTree<VALUE>::Node {
 public:
  enum Side { RIGHT, LEFT };
  enum Color { RED, BLACK };

  explicit Node()
    : value_(),
      left_son_(nullptr), right_son_(nullptr), next_(nullptr) {}

  template<typename U>
  explicit Node(U&& value)
    : value_(std::forward<U>(value)),
      left_son_(nullptr), right_son_(nullptr), next_(nullptr) {}

  // when a node is going to be destructed, caller MUST make sure firstly that the node
  // is detached from rbtree and that the list-node is detached from sorted list.
  ~Node() {
    left_son_.store(nullptr);
    right_son_.store(nullptr);
    next_.store(nullptr);
  }

  static void SwapColor(Node* node1, Node* node2) {
    if (node1 != nullptr && node2 != nullptr) {
      std::swap(node1->color_, node2->color_);
    }
  }

  inline VALUE& value() { return value_; }

  inline Color color() const { return color_; }

  inline void setColor(Color new_color) { color_ = new_color; }

  inline Node* leftSon() const {
    return left_son_.load();
  }

  inline Node* rightSon() const {
    return right_son_.load();
  }

  inline Side setSon(Side side, Node* new_son) {
    if (side == LEFT) left_son_.store(new_son);
    else right_son_.store(new_son);
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

  // pointer region
  std::atomic<LeftSon> left_son_;
  std::atomic<RightSon> right_son_;
  std::atomic<ListNext> next_;
};

static void TestSingleThreadAbility(bool sequential_insert) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "single thread test (sequential_insert = " << (sequential_insert ? "true" : "false") << "):\n";
  RBTree<int> my_map;
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
  std::atomic<int> my_map_size(0);
  std::set<int> init_set, random_set_for_insert;
  std::vector<int> init_list, random_list_for_insert;
  const int INIT_DATA_COUNT = 1024;
  const int DATA_COUNT_FOR_INSERTION = 100000;
  const int MAX_TRY_TIMES = INT32_MAX;
  // x86-64 intel (11 read threads):
  // random case (random insert value and random find value):
  // try_times = 1 : 99.9997%
  // try_times = 2 : 100%
  // worst case (sequential insert and always find the last insert value):
  // try_times = 1 : 95%
  // try_times = 2 : 99.995%
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
  std::atomic<bool> insert_over(false);
  auto insert_ope = [&my_map, &my_map_size, &random_list_for_insert, &insert_over]() {
    for (const auto ele: random_list_for_insert) {
      my_map.insert(ele);
      my_map_size.fetch_add(+1);
    }
    insert_over.store(true);
  };
  std::atomic<int> tot_find_times(0);
  std::atomic<int> success_find_times(0);
  std::atomic<int> perf_find_times(0);
  std::atomic<int> max_try_times(0);
  auto find_ope = [&my_map, &my_map_size, &insert_over, &init_list, &random_list_for_insert,
                   &tot_find_times, &success_find_times, &perf_find_times, &max_try_times, PERF_MAX_TRY_TIMES, is_worst_case]() {
    int idx = 0;
    while(!insert_over.load()) {
      int value;
      if (is_worst_case) idx = my_map_size.load() - 1; // worst case: always find the last insert value
      if (idx < INIT_DATA_COUNT) value = init_list[idx];
      else if (idx < my_map_size.load()) value = random_list_for_insert[idx - INIT_DATA_COUNT];
      else idx = 0, value = init_list[idx];
      bool result;
      // try_times的次数越多，说明find操作受旋转操作而导致红黑树失效的次数越多，和value存在与否无关。当然，测试时为了方便，保证了value在find时必然存在的。
      int try_times = 0;
      do {
        // value must exist.
        result = my_map.findForConcurrentTest(value);
        try_times++;
      } while(!result && try_times < MAX_TRY_TIMES);
      tot_find_times.fetch_add(+1);
      if (result) success_find_times.fetch_add(+1);
      if (result && try_times <= PERF_MAX_TRY_TIMES) perf_find_times.fetch_add(+1);
      int local_max_try_times = max_try_times.load();
      do {
        if (local_max_try_times >= try_times) break;
      } while(!max_try_times.compare_exchange_strong(local_max_try_times, try_times));
      idx++;
    }
  };
  std::thread write_thread(insert_ope);
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

int main() {
  TestOneWriteMultiReadConcurrentPerf(2, false);
  TestOneWriteMultiReadConcurrentPerf(2, true);
  TestSingleThreadAbility(false);
  TestSingleThreadAbility(true);
}
