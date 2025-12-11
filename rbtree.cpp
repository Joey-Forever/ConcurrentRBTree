#include <iostream>
#include <vector>
#include <atomic>
#include <map>
#include <chrono>
#include <random>
#include <shared_mutex>

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
    Node* node_no_greater_than_value = internalNoGreaterBound(root_->leftSon(), value, nullptr);
    node_no_greater_than_value = node_no_greater_than_value == nullptr ? list_header_->next() : node_no_greater_than_value;
    if (node_no_greater_than_value == list_tailer_ || node_no_greater_than_value->value() > value) {
      // exactly all nodes are greater than value.
      return nullptr;
    }
    if (node_no_greater_than_value->value() == value) {
      return node_no_greater_than_value;
    }
    // here node_no_greater_than_value < value
    // find the target node across the sorted-list.
    // very very bad case is that we should search from the first node.
    while (node_no_greater_than_value->next() != list_tailer_ && node_no_greater_than_value->next()->value() < value) {
      node_no_greater_than_value = node_no_greater_than_value->next();
    }
    Node* node_to_find =  node_no_greater_than_value->next();
    if (node_to_find == list_tailer_ || node_to_find->value() > value) return nullptr;
    else return node_to_find;
  }

  // find the first node >= value.
  // Node* lowerBound(const VALUE& value) {
  //   Node* node_no_greater_than_value = internalNoGreaterBound(root_->leftSon(), value, nullptr);
  // }

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
      Side delete_side = detachAndDeleteLeafNode(erase_node, father_of_erase_node, pre_list_node);
      // upward balance the rbtree.
      balanceTheTreeAfterErase(father_of_erase_node, delete_side, erase_path);
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
      // 1. temporarily detach right_most_node from rbtree but keep being attached into sorted-list.
      Side delete_side;
      if (father_of_right_most_node->leftSon() == right_most_node) {
        father_of_right_most_node->setLeftSon(nullptr);
        delete_side = Side::LEFT;
      } else {
        father_of_right_most_node->setRightSon(nullptr);
        delete_side = Side::RIGHT;
      }
      // 2. point right_most_node's two sons to erase_node's two sons.
      right_most_node->setLeftSon(erase_node->leftSon());
      right_most_node->setRightSon(erase_node->rightSon());
      // 3. attach the right_most_node to rbtree by being a son of father_of_erase_node
      if (father_of_erase_node->leftSon() == erase_node) father_of_erase_node->setLeftSon(right_most_node);
      else father_of_erase_node->setRightSon(right_most_node);
      // 4. here the erase_node is detached from rbtree, detach it from sorted-list below.
      right_most_node->setNext(erase_node->next());
      // 5. here the erase_node is detached from rbtree and sorted-list, delete it. before delete erase_node, exchange it from erase_path by right_most_node
      for (auto& node: erase_path) {
        if (node == erase_node) {
          node = right_most_node;
          break;
        }
      }
      delete erase_node;
      // 6. upward balance the rbtree.
      balanceTheTreeAfterErase(father_of_right_most_node, delete_side, erase_path);
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
  enum Side { NODE, RIGHT, LEFT };

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
  inline Node* rotateRight(Node* node, Node* father) {
    Node* left_son = node->leftSon();
    if (left_son == nullptr) {
      return nullptr;
    }
    node->setLeftSon(left_son->rightSon());
    left_son->setRightSon(node);
    if (father->leftSon() == node) {
      father->setLeftSon(left_son);
      return left_son;
    } else {
      father->setRightSon(left_son);
      return left_son;
    }
  }

  // rotate-left the subtree rooted on node.
  // !only rotate, NOT change color.
  inline Node* rotateLeft(Node* node, Node* father) {
    Node* right_son = node->rightSon();
    if (right_son == nullptr) {
      return nullptr;
    }
    node->setRightSon(right_son->leftSon());
    right_son->setLeftSon(node);
    if (father->leftSon() == node) {
      father->setLeftSon(right_son);
      return right_son;
    } else {
      father->setRightSon(right_son);
      return right_son;
    }
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
    if (bro_of_delete_side->color() == Node::BLACK) {
      // bro_of_delete_side won't be together with father_of_erase_node to as a node in 2-3-4 tree.
      if (bro_of_delete_side->leftSon() != nullptr && bro_of_delete_side->leftSon()->color() == Node::RED) {
        // we can use the left son of bro_of_delete_side to balance the tree.
        Side side = makeTreeGenSameSide(bro_of_delete_side->leftSon(), bro_of_delete_side, father_of_erase_node);
        Node* grand_father = erase_path.empty() ? root_ : erase_path.back();
        Node* node;
        if (side == Side::LEFT) node = rotateRight(father_of_erase_node, grand_father);
        else node = rotateLeft(father_of_erase_node, grand_father);
        node->setColor(father_of_erase_node->color());
        node->leftSon()->setColor(Node::BLACK);
        node->rightSon()->setColor(Node::BLACK);
        return;
      }
      if (bro_of_delete_side->rightSon() != nullptr && bro_of_delete_side->rightSon()->color() == Node::RED) {
        // we can use the right son of bro_of_delete_side to balance the tree.
        Side side = makeTreeGenSameSide(bro_of_delete_side->rightSon(), bro_of_delete_side, father_of_erase_node);
        Node* grand_father = erase_path.empty() ? root_ : erase_path.back();
        Node* node;
        if (side == Side::LEFT) node = rotateRight(father_of_erase_node, grand_father);
        else node = rotateLeft(father_of_erase_node, grand_father);
        node->setColor(father_of_erase_node->color());
        node->leftSon()->setColor(Node::BLACK);
        node->rightSon()->setColor(Node::BLACK);
        return;
      }
      // bro_of_delete_side can not help, try to ask father_of_erase_node.
      if (father_of_erase_node->color() == Node::RED) {
        // father_of_erase_node can help.
        Node::SwapColor(father_of_erase_node, bro_of_delete_side);
        return;
      } else {
        // father_of_erase_node can not help;
        bro_of_delete_side->setColor(Node::RED);
        Node* grand_father = erase_path.empty() ? root_ : erase_path.back();
        if (!erase_path.empty()) erase_path.pop_back();
        return balanceTheTreeAfterErase(grand_father, grand_father->leftSon() == father_of_erase_node ? Side::LEFT : Side::RIGHT, erase_path);
      }
    } else {
      // bro_of_delete_side must be together with father_of_erase_node to as a node in 2-3-4 tree.
      // father_of_erase_node must be BLACK.
      // bro_of_delete_side must have 0 son or 2 sons.
      if (bro_of_delete_side->leftSon() == nullptr && bro_of_delete_side->rightSon() == nullptr) {
        // the erase node must be RED and together with bro_of_delete_side and father_of_erase_node to be a node in 2-3-4 tree.
        return;
      } else {
        // bro_of_delete_side must have 2 sons here.
        Node* grand_father = erase_path.empty() ? root_ : erase_path.back();
        Node* node;
        Side bro_side;
        if (father_of_erase_node->leftSon() == bro_of_delete_side) node = rotateRight(father_of_erase_node, grand_father), bro_side = Side::LEFT;
        else node = rotateLeft(father_of_erase_node, grand_father), bro_side = Side::RIGHT;
        node->setColor(Node::BLACK);
        node->leftSon()->setColor(Node::BLACK);
        node->rightSon()->setColor(Node::BLACK);
        if (bro_side == Side::RIGHT) node->leftSon()->rightSon()->setColor(Node::RED);
        else node->rightSon()->leftSon()->setColor(Node::RED);
        return;
      }
    }
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
    if (insert_path.empty()) root_->setLeftSon(insert_node);
    else if (value < insert_path.back()->value()) insert_path.back()->setLeftSon(insert_node);
    else if (value > insert_path.back()->value()) insert_path.back()->setRightSon(insert_node);
    return insert_node;
  }

  // detach the delete_node from sorted list and rbtree, and then destruct the physical node.
  // caller MUST make sure the right relationship between all incoming nodes.
  // caller MUST make sure the delete_node is a leaf node.
  // return which side the delete_node is inside his father.
  Side detachAndDeleteLeafNode(Node* delete_node, Node* father_node, Node* pre_list_node) {
    Side delete_side;
    pre_list_node->setNext(delete_node->next());
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

template <typename VALUE>
class RBTree<VALUE>::Node {
 public:
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
    left_son_ = nullptr;
    right_son_ = nullptr;
    next_ = nullptr;
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
    return left_son_;
  }

  inline void setLeftSon(LeftSon new_left_son) {
    left_son_ = new_left_son;
  }

  inline Node* rightSon() const {
    return right_son_;
  }

  inline void setRightSon(RightSon new_right_son) {
    right_son_ = new_right_son;
  }

  inline Node* next() const {
    return next_;
  }

  inline void setNext(ListNext new_next) {
    next_ = new_next;
  }

 private:
  // data region
  VALUE value_;
  Color color_;

  // pointer region
  LeftSon left_son_;
  RightSon right_son_;
  ListNext next_;
};

int main() {
  RBTree<int> my_map;
  std::random_device rd;          // Áî®‰∫éÁîüÊàêÁúüÈöèÊú∫ÁßçÂ≠êÔºàÂ¶? /dev/urandomÔº?
  std::mt19937 gen(rd());         // ‰ΩøÁî®Ê¢ÖÊ£ÆÊóãËΩ¨ÁÆóÊ≥ï‰Ωú‰∏∫ÂºïÊìé
  std::uniform_int_distribution<> dis(0, 10000000); // ÁîüÊàê [1, 100] ÁöÑÂùáÂåÄÊï¥Êï∞
  for (int i = 0; i < 1000000; i++) {
    int a = dis(gen);
    if (my_map.find(a) != nullptr) i--;
    else my_map.insert(a);
  }
  int newest_max_height = INT32_MIN;
  int newest_min_height = INT32_MAX;
  int node_count = 0;
  my_map.getHeightInfoForTest(my_map.getRootForTest(), 0, newest_max_height, newest_min_height, node_count);
  std::cout << newest_max_height << " " << newest_min_height << " " << node_count - 1 << "\n";
  RBTree<int>::Node* start_node = my_map.getRootForTest()->leftSon();
  int end_value = start_node->value() + 100086;
  // while(start_node != my_map.getTailer() && start_node->value() <= end_value) {
  //   std::cout << start_node->value() << " ";
  //   start_node = start_node->next();
  // }
  // std::cout << "\n";
  for (int i = start_node->value() + 1000; i <= start_node->value() + 10000; i++) my_map.erase(i);
  // while(start_node != my_map.getTailer() && start_node->value() <= end_value) {
  //   std::cout << start_node->value() << " ";
  //   start_node = start_node->next();
  // }
  // std::cout << "\n";
}
