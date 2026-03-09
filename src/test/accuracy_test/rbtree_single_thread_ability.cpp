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

#include <ConcurrentRBTree.h>

static void TestSingleThreadAbility(bool sequential_insert) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "single thread test (sequential_insert = " << (sequential_insert ? "true" : "false") << "):\n";
  gipsy_danger::ConcurrentRBTree<int>::Accessor accessor(gipsy_danger::ConcurrentRBTree<int>::createInstance());
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

static void checkCompileMode() {
#ifndef NDEBUG
  std::cout << "DEBUG MODE" << "\n";
#else
  std::cout << "PERF MODE" << "\n";
#endif // NDEBUG
}

int main() {
  std::cout << "ConcurrentRBTree Performance Test" << "\n";
  checkCompileMode();
  TestSingleThreadAbility(false);
  TestSingleThreadAbility(true);
}
