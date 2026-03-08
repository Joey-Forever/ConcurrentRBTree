/*
 * ConcurrentRBTree Performance Test
 *
 * Build:
  g++ concurrent_rbtree_perf_test.cpp -o rbtree_test -std=c++17 -pthread -O2 -DNDEBUG
 */

#include "../rbtree.h"
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <set>
#include <cassert>
#include <climits>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <tuple>
#include "test_perf.h"

using RBTreeT = RBTree<int>;
using RBTreeAccessor = RBTreeT::Accessor;

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
  TestMultiReadFewWriteConcurrentPerf<RBTreeAccessor>([]() {
    auto rbtree = RBTreeT::createInstance();
    return std::make_unique<RBTreeAccessor>(rbtree);
  });
}
