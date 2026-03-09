/*
 * ConcurrentSkipList Performance Test
 *
 * Build for mac:
  g++ concurrent_skiplist_perf_test.cpp -o skiplist_test -std=c++17 -pthread -O2 -DNDEBUG\
    -DGLOG_USE_GLOG_EXPORT \
    -I/opt/homebrew/Cellar/folly/2026.01.12.00_1/include \
    -I/opt/homebrew/Cellar/glog/0.7.1/include \
    -I/opt/homebrew/Cellar/fmt/12.1.0/include \
    -I/opt/homebrew/Cellar/double-conversion/3.4.0/include \
    -I/opt/homebrew/Cellar/libevent/2.1.12_1/include \
    -I/opt/homebrew/Cellar/boost/1.90.0/include \
    -L/opt/homebrew/Cellar/folly/2026.01.12.00_1/lib \
    -L/opt/homebrew/Cellar/glog/0.7.1/lib \
    -L/opt/homebrew/Cellar/fmt/12.1.0/lib \
    -L/opt/homebrew/Cellar/double-conversion/3.4.0/lib \
    -L/opt/homebrew/Cellar/libevent/2.1.12_1/lib \
    -lfolly -lglog -lfmt -ldouble-conversion -levent
 *
 * Build for Linux (with folly built from source, C++17, -O2, no LTO):
 * folly build: cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O2 -march=x86-64" ..
  g++ concurrent_skiplist_perf_test.cpp -o concurrent_skiplist_perf_test \
    -std=c++17 -pthread -O2 -march=x86-64 -DNDEBUG \
    -DGLOG_USE_GLOG_EXPORT \
    -I/home/joey/Documents/joey_project/folly \
    -I/home/joey/Documents/joey_project/folly/_build \
    -I/usr/include/glog \
    -L/home/joey/Documents/joey_project/folly/_build \
    -L/usr/lib/x86_64-linux-gnu \
    -lfolly -lglog -lgflags -lfmt -ldouble-conversion -levent -lpthread -ldl -lz -lbz2 -llz4 -lzstd -lunwind -liberty
 */

#include <folly/ConcurrentSkipList.h>
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

using SkipListT = folly::ConcurrentSkipList<int, std::less<int> >;
using SkipListAccessor = SkipListT::Accessor;

static void checkCompileMode() {
  // always perf mode due to remote folly's built product, which is release. 
  std::cout << "PERF MODE" << "\n";
}

int main() {
  std::cout << "SkipList Performance Test" << "\n";
  checkCompileMode();
  TestMultiReadFewWriteConcurrentPerf<SkipListAccessor>([]() {
    // Create a skip list with initial height of 10
    auto skipList = SkipListT::createInstance(10);
    return std::make_unique<SkipListAccessor>(skipList);
  });
}
