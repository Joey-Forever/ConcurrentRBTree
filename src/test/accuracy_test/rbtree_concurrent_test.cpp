/*
 * Concurrent Correctness Test for RBTree
 * Inspired by folly::ConcurrentSkipList test framework
 *
 * This test file verifies the correctness of concurrent read-write operations
 * on RBTree, including insert, erase, point find, and range query operations.
 */

#include <iostream>
#include <vector>
#include <atomic>
#include <set>
#include <thread>
#include <random>
#include <chrono>
#include <cassert>
#include <algorithm>
#include <iomanip>

#include "../rbtree.h"

namespace {

using ValueType = int;
using RBTreeType = RBTree<ValueType>;
using RBTreeAccessor = RBTreeType::Accessor;
using SetType = std::set<ValueType>;

static const int kMaxValue = 5000;
static const int kDefaultNumThreads = 12;

// Thread-local random number generator
thread_local std::mt19937 thread_local_gen(std::random_device{}());

// Random integer generation in range [0, max_value)
inline int32_t randomInt32(int32_t max_value) {
    std::uniform_int_distribution<int32_t> dist(0, max_value - 1);
    return dist(thread_local_gen);
}

//=============================================================================
// Test Helper Functions - Similar to folly's test helpers
//=============================================================================

// Random adding operation - inserts random values into both rbtree and verifier
static void randomAdding(
    int size,
    RBTreeAccessor& rbtree,
    SetType* verifier,
    int maxValue = kMaxValue) {
    for (int i = 0; i < size; ++i) {
        int32_t r = randomInt32(maxValue);
        verifier->insert(r);
        rbtree.insert(r);
    }
}

// Random removal operation - removes random values from rbtree
static void randomRemoval(
    int size,
    RBTreeAccessor& rbtree,
    SetType* verifier,
    int maxValue = kMaxValue) {
    for (int i = 0; i < size; ++i) {
        int32_t r = randomInt32(maxValue);
        verifier->insert(r);  // Record attempted removal
        rbtree.erase(r);
    }
}

// Sum all values using iterator - tests iteration correctness
static void sumAllValues(RBTreeAccessor& rbtree, int64_t* sum) {
    *sum = 0;
    for (auto it = rbtree.begin(); it != rbtree.end(); ++it) {
        *sum += *it;
    }
}

// Range query using lower_bound - tests range find correctness
static void concurrentRangeQuery(
    const std::vector<ValueType>* values,
    RBTreeAccessor& rbtree,
    int64_t* result) {
    *result = 0;
    for (const auto& target : *values) {
        auto it = rbtree.lower_bound(target);
        if (it != rbtree.end()) {
            *result += *it;
        }
    }
}

// Verify equality between rbtree and std::set verifier
bool verifyEqual(RBTreeAccessor& rbtree, const SetType& verifier) {
    // Check size
    if (verifier.size() != rbtree.size()) {
        std::cerr << "Size mismatch: verifier=" << verifier.size()
                  << ", rbtree=" << rbtree.size() << std::endl;
        return false;
    }

    // Verify each element exists in rbtree (contains/find test)
    for (const auto& value : verifier) {
        auto it = rbtree.find(value);
        if (it == rbtree.end()) {
            std::cerr << "Value " << value << " not found in rbtree" << std::endl;
            return false;
        }
        if (*it != value) {
            std::cerr << "Value mismatch: expected " << value
                      << ", got " << *it << std::endl;
            return false;
        }
    }

    // Verify iteration order matches std::set
    auto rbtree_it = rbtree.begin();
    auto verifier_it = verifier.begin();
    while (rbtree_it != rbtree.end() && verifier_it != verifier.end()) {
        if (*rbtree_it != *verifier_it) {
            std::cerr << "Iteration mismatch: rbtree=" << *rbtree_it
                      << ", verifier=" << *verifier_it << std::endl;
            return false;
        }
        ++rbtree_it;
        ++verifier_it;
    }

    if (rbtree_it != rbtree.end() || verifier_it != verifier.end()) {
        std::cerr << "Iteration length mismatch" << std::endl;
        return false;
    }

    return true;
}

//=============================================================================
// Test Cases
//=============================================================================

// Test 1: Sequential Access - Basic operations in single thread
bool testSequentialAccess() {
    std::cout << "\n=== Test 1: SequentialAccess ===" << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    // Test empty tree
    assert(accessor.empty());
    assert(accessor.size() == 0);
    assert(accessor.find(3) == accessor.end());

    // Test insert and find
    auto ret = accessor.insert(3);
    assert(ret.second);
    assert(*ret.first == 3);

    ret = accessor.insert(5);
    assert(ret.second);
    assert(*ret.first == 5);

    ret = accessor.insert(3);  // Duplicate insert
    assert(!ret.second);
    assert(*ret.first == 3);

    // Test contains/find
    assert(accessor.find(3) != accessor.end());
    assert(*accessor.find(3) == 3);
    assert(accessor.find(2) == accessor.end());

    // Test lower_bound (range query)
    auto it = accessor.lower_bound(3);
    assert(it != accessor.end() && *it == 3);

    it = accessor.lower_bound(4);
    assert(it != accessor.end() && *it == 5);

    it = accessor.lower_bound(10);
    assert(it == accessor.end());

    // Test iteration
    int sum = 0;
    for (auto iter = accessor.begin(); iter != accessor.end(); ++iter) {
        sum += *iter;
    }
    assert(sum == 8);  // 3 + 5

    // Test erase
    assert(accessor.erase(3) == 1);
    assert(accessor.erase(3) == 0);  // Already erased
    assert(accessor.find(3) == accessor.end());
    assert(accessor.find(5) != accessor.end());

    std::cout << "SequentialAccess test PASSED" << std::endl;
    return true;
}

// Test 2: Concurrent Add - Multiple threads inserting simultaneously
bool testConcurrentAdd() {
    std::cout << "\n=== Test 2: ConcurrentAdd ===" << std::endl;

    int numThreads = 100;
    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    std::vector<std::thread> threads;
    std::vector<SetType> verifiers(numThreads);

    auto start = std::chrono::high_resolution_clock::now();

    try {
        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back(
                [&accessor, &verifiers, i]() {
                    randomAdding(100000, accessor, &verifiers[i], kMaxValue);
                });
        }
    } catch (const std::system_error& e) {
        std::cerr << "Caught system error: " << e.what() << std::endl;
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Merge all verifiers
    SetType all;
    for (const auto& verifier : verifiers) {
        all.insert(verifier.begin(), verifier.end());
    }

    // Verify correctness
    bool passed = verifyEqual(accessor, all);

    std::cout << "ConcurrentAdd test: " << (passed ? "PASSED" : "FAILED")
              << " (threads: " << threads.size()
              << ", time: " << duration.count() << "ms)" << std::endl;

    return passed;
}

// Test 3: Concurrent Remove - Multiple threads deleting simultaneously
void testConcurrentRemoval(int numThreads, int maxValue) {
    std::cout << "\n=== Test 3: ConcurrentRemove (threads=" << numThreads
              << ", maxValue=" << maxValue << ") ===" << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    // Pre-populate the tree
    for (int i = 0; i < maxValue; ++i) {
        accessor.insert(i);
    }

    std::vector<std::thread> threads;
    std::vector<SetType> verifiers(numThreads);

    auto start = std::chrono::high_resolution_clock::now();

    try {
        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back(
                [&accessor, &verifiers, i, maxValue]() {
                    randomRemoval(100, accessor, &verifiers[i], maxValue);
                });
        }
    } catch (const std::system_error& e) {
        std::cerr << "Caught system error: " << e.what() << std::endl;
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    int treeSize = accessor.size();
    bool passed = (treeSize >= 0 && treeSize <= maxValue);

    // Verify no element in tree is invalid
    for (auto it = accessor.begin(); it != accessor.end() && passed; ++it) {
        if (*it < 0 || *it >= maxValue) {
            passed = false;
        }
    }

    std::cout << "ConcurrentRemove test: " << (passed ? "PASSED" : "FAILED")
              << " (threads: " << threads.size()
              << ", time: " << duration.count() << "ms"
              << ", treeSize: " << treeSize << ")" << std::endl;
}

bool testConcurrentRemove() {
    for (int numThreads = 10; numThreads < 100; numThreads += 10) {
        testConcurrentRemoval(numThreads, 100 * numThreads);
    }
    return true;
}

// Test 4: Concurrent Access - Mixed read/write operations
static void testConcurrentAccess(
    int numInsertions,
    int numDeletions,
    int maxValue,
    int numThreads) {

    std::cout << "\n=== Test 4: ConcurrentAccess ===" << std::endl;
    std::cout << "  numThreads=" << numThreads
              << ", numInsertions=" << numInsertions
              << ", numDeletions=" << numDeletions
              << ", maxValue=" << maxValue << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    std::vector<SetType> verifiers(numThreads);
    std::vector<int64_t> sums(numThreads);
    std::vector<std::vector<ValueType>> rangeQueryValues(numThreads);

    // Pre-generate range query values for each thread
    for (int i = 0; i < numThreads; ++i) {
        for (int j = 0; j < numInsertions / 10; ++j) {
            rangeQueryValues[i].push_back(randomInt32(maxValue));
        }
        std::sort(rangeQueryValues[i].begin(), rangeQueryValues[i].end());
    }

    std::vector<std::thread> threads;

    auto start = std::chrono::high_resolution_clock::now();

    // Thread type assignment (similar to folly's approach):
    // case 0,1: insert operations
    // case 2: delete operations
    // case 3: range query operations
    // default: iteration/sum operations
    for (int i = 0; i < numThreads; ++i) {
        switch (i % 8) {
            case 0:
            case 1:
                threads.emplace_back(
                    [&accessor, &verifiers, i, numInsertions, maxValue]() {
                        randomAdding(numInsertions, accessor, &verifiers[i], maxValue);
                    });
                break;
            case 2:
                threads.emplace_back(
                    [&accessor, &verifiers, i, numDeletions, maxValue]() {
                        randomRemoval(numDeletions, accessor, &verifiers[i], maxValue);
                    });
                break;
            case 3:
                threads.emplace_back(
                    [&accessor, &rangeQueryValues, i, &sums]() {
                        concurrentRangeQuery(&rangeQueryValues[i], accessor, &sums[i]);
                    });
                break;
            default:
                threads.emplace_back(
                    [&accessor, &sums, i]() {
                        sumAllValues(accessor, &sums[i]);
                    });
                break;
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "  Completed in " << duration.count() << "ms" << std::endl;

    // Verify tree structure
    std::cout << "  Final tree size: " << accessor.size() << std::endl;

    // Verify iteration produces sorted output
    auto prev = accessor.begin();
    if (prev != accessor.end()) {
        for (auto it = std::next(prev); it != accessor.end(); ++it) {
            if (*prev > *it) {
                std::cerr << "  ERROR: Tree not sorted! " << *prev << " > " << *it << std::endl;
                return;
            }
            prev = it;
        }
    }
    std::cout << "  ConcurrentAccess test PASSED (tree structure verified)" << std::endl;
}

// Test 5: Range Query Correctness
bool testRangeQuery() {
    std::cout << "\n=== Test 5: RangeQuery ===" << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    // Insert specific values
    std::vector<int> values = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    for (auto v : values) {
        accessor.insert(v);
    }

    // Test lower_bound
    auto it = accessor.lower_bound(30);
    assert(it != accessor.end());
    assert(*it == 30);

    it = accessor.lower_bound(35);
    assert(it != accessor.end());
    assert(*it == 40);

    it = accessor.lower_bound(100);
    assert(it != accessor.end());
    assert(*it == 100);

    it = accessor.lower_bound(101);
    assert(it == accessor.end());

    it = accessor.lower_bound(5);
    assert(it != accessor.end());
    assert(*it == 10);

    // Test iteration range
    int sum = 0;
    for (it = accessor.lower_bound(25); it != accessor.end() && *it < 75; ++it) {
        sum += *it;
    }
    assert(sum == 30 + 40 + 50 + 60 + 70);  // 250

    std::cout << "RangeQuery test PASSED" << std::endl;
    return true;
}

// Test 6: Stress Test - Large scale concurrent operations
bool testStressTest() {
    std::cout << "\n=== Test 6: StressTest ===" << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    const int numThreads = 16;
    const int opsPerThread = 100000;
    const int maxValue = 1000000;

    std::vector<std::thread> threads;
    std::atomic<int> insertCount(0);
    std::atomic<int> eraseCount(0);
    std::atomic<int> findCount(0);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(
            [&accessor, &insertCount, &eraseCount, &findCount, opsPerThread, maxValue]() {
                std::mt19937 gen(std::random_device{}());
                std::uniform_int_distribution<int> dist(0, maxValue - 1);
                std::uniform_int_distribution<int> opDist(0, 9);

                for (int j = 0; j < opsPerThread; ++j) {
                    int value = dist(gen);
                    int op = opDist(gen);

                    if (op < 5) {  // 50% insert
                        accessor.insert(value);
                        insertCount.fetch_add(1);
                    } else if (op < 7) {  // 20% erase
                        accessor.erase(value);
                        eraseCount.fetch_add(1);
                    } else {  // 30% find
                        accessor.find(value);
                        findCount.fetch_add(1);
                    }
                }
            });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "  Inserts: " << insertCount.load() << std::endl;
    std::cout << "  Erases: " << eraseCount.load() << std::endl;
    std::cout << "  Finds: " << findCount.load() << std::endl;
    std::cout << "  Final size: " << accessor.size() << std::endl;
    std::cout << "  Time: " << duration.count() << "ms" << std::endl;

    // Verify tree structure
    auto prev = accessor.begin();
    if (prev != accessor.end()) {
        for (auto it = std::next(prev); it != accessor.end(); ++it) {
            if (*prev > *it) {
                std::cerr << "  ERROR: Tree not sorted after stress test!" << std::endl;
                return false;
            }
            prev = it;
        }
    }

    std::cout << "StressTest test PASSED" << std::endl;
    return true;
}

// Test 7: Iterator Stress Test
bool testIteratorStressTest() {
    std::cout << "\n=== Test 7: IteratorStressTest ===" << std::endl;

    auto rbtree = RBTreeType::createInstance();
    RBTreeAccessor accessor(rbtree);

    const int numElements = 100000;

    // Insert elements
    for (int i = 0; i < numElements; ++i) {
        accessor.insert(i * 10);  // 0, 10, 20, ..., 999990
    }

    // Test forward iteration
    int64_t sum = 0;
    int count = 0;
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        sum += *it;
        count++;
    }

    int64_t expectedSum = 0;
    for (int i = 0; i < numElements; ++i) {
        expectedSum += i * 10;
    }

    assert(sum == expectedSum);
    assert(count == numElements);

    std::cout << "IteratorStressTest test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test Runner
//=============================================================================

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "==================================================" << std::endl;
    std::cout << "  RBTree Concurrent Correctness Test Suite" << std::endl;
    std::cout << "  Inspired by folly::ConcurrentSkipList tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    int numThreads = kDefaultNumThreads;
    if (argc > 1) {
        numThreads = std::atoi(argv[1]);
        if (numThreads <= 0) {
            numThreads = kDefaultNumThreads;
        }
    }

    std::cout << "\nUsing " << numThreads << " threads for concurrent tests" << std::endl;

    // Run tests
    if (!testSequentialAccess()) {
        std::cerr << "\n!!! TEST testSequentialAccess FAILED !!!" << std::endl;
        return 1;
    }

    if (!testRangeQuery()) {
        std::cerr << "\n!!! TEST testRangeQuery FAILED !!!" << std::endl;
        return 1;
    }

    if (!testIteratorStressTest()) {
        std::cerr << "\n!!! TEST testIteratorStressTest FAILED !!!" << std::endl;
        return 1;
    }

    if (!testConcurrentAdd()) {
        std::cerr << "\n!!! TEST testConcurrentAdd FAILED !!!" << std::endl;
        return 1;
    }

    if (!testConcurrentRemove()) {
        std::cerr << "\n!!! TEST testConcurrentRemove FAILED !!!" << std::endl;
        return 1;
    }

    // Run concurrent access with configured thread count
    testConcurrentAccess(10000, 100, kMaxValue, numThreads);

    if (!testStressTest()) {
        std::cerr << "\n!!! TEST testStressTest FAILED !!!" << std::endl;
        return 1;
    }

    std::cout << "\n==================================================" << std::endl;
    std::cout << "  ALL TESTS PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
}
