/*
 * ConcurrentRBTree Performance Test
 *
 * Build for Linux (with C++17, -O2, no LTO):
  g++ concurrent_rbtree_perf_test.cpp -o concurrent_rbtree_perf_test \
    -std=c++17 -pthread -O2 -march=x86-64 -DNDEBUG \
    -I/home/joey/Documents/joey_project/ConcurrentRBTree/src/include
 */

#include <ConcurrentRBTree.h>
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

using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<int>;
using ConcurrentRBTreeAccessor = ConcurrentRBTreeT::Accessor;

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
  TestMultiReadFewWriteConcurrentPerf<ConcurrentRBTreeAccessor>([]() {
    auto rbtree = ConcurrentRBTreeT::createInstance();
    return std::make_unique<ConcurrentRBTreeAccessor>(rbtree);
  });
}
