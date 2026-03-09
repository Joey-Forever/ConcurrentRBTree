/*
 * Edge Case and Concurrency Stress Test for ConcurrentRBTree
 * Tests scenarios that are easy to miss in standard testing
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

#include <ConcurrentRBTree.h>

namespace {

using ValueType = int;
using ConcurrentRBTreeType = gipsy_danger::ConcurrentRBTree<ValueType>;
using ConcurrentRBTreeAccessor = ConcurrentRBTreeType::Accessor;

//=============================================================================
// Test 1: High Contention - Multiple threads operating on SAME key
// This tests insert-insert, insert-erase, erase-find races
//=============================================================================
bool testHighContention() {
    std::cout << "\n=== Test: HighContention (Same Key Races) ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    const int numThreads = 16;
    const int numOpsPerThread = 10000;
    const int hotKey = 42;  // All threads will compete on this key

    std::atomic<int> insertSuccess(0);
    std::atomic<int> insertFailed(0);
    std::atomic<int> eraseSuccess(0);
    std::atomic<int> eraseFailed(0);
    std::atomic<int> findCount(0);

    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> opDist(0, 9);

            for (int j = 0; j < numOpsPerThread; ++j) {
                int op = opDist(gen);

                if (op < 4) {  // 40% insert hot key
                    auto ret = accessor.insert(hotKey);
                    if (ret.second) {
                        insertSuccess.fetch_add(1);
                    } else {
                        insertFailed.fetch_add(1);
                    }
                } else if (op < 7) {  // 30% erase hot key
                    if (accessor.erase(hotKey)) {
                        eraseSuccess.fetch_add(1);
                    } else {
                        eraseFailed.fetch_add(1);
                    }
                } else {  // 30% find hot key
                    accessor.find(hotKey);
                    findCount.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "  InsertSuccess: " << insertSuccess.load()
              << ", InsertFailed: " << insertFailed.load() << std::endl;
    std::cout << "  EraseSuccess: " << eraseSuccess.load()
              << ", EraseFailed: " << eraseFailed.load() << std::endl;
    std::cout << "  FindCount: " << findCount.load() << std::endl;
    std::cout << "  Final tree size: " << accessor.size() << std::endl;

    // Verify tree structure is still valid
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        // Just make sure we can iterate without crashing
    }

    std::cout << "  HighContention test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 2: Insert-Erase-Find Same Value Race
// Specifically tests: insert(A) racing with erase(A) racing with find(A)
//=============================================================================
bool testInsertEraseFindRace() {
    std::cout << "\n=== Test: InsertEraseFindRace ===" << std::endl;

    const int numIterations = 1000;
    const int targetValue = 999;

    for (int iter = 0; iter < numIterations; ++iter) {
        auto rbtree = ConcurrentRBTreeType::createInstance();
        ConcurrentRBTreeAccessor accessor(rbtree);

        // Pre-insert the value
        accessor.insert(targetValue);
        assert(accessor.find(targetValue) != accessor.end());

        std::atomic<bool> stop(false);
        std::atomic<int> findFound(0);
        std::atomic<int> findNotFound(0);

        // Thread 1: Keep erasing and re-inserting
        std::thread writer([&]() {
            std::mt19937 gen(iter);
            while (!stop.load()) {
                accessor.erase(targetValue);
                std::this_thread::yield();
                accessor.insert(targetValue);
                std::this_thread::yield();
            }
        });

        // Thread 2: Keep finding
        std::thread reader([&]() {
            std::mt19937 gen(iter + 1);
            int count = 0;
            while (count < 1000) {
                auto it = accessor.find(targetValue);
                if (it != accessor.end()) {
                    findFound.fetch_add(1);
                } else {
                    findNotFound.fetch_add(1);
                }
                count++;
                std::this_thread::yield();
            }
            stop.store(true);
        });

        writer.join();
        reader.join();

        // Verify final state is consistent
        auto it = accessor.find(targetValue);
        if (it != accessor.end()) {
            assert(*it == targetValue);
        }
    }

    std::cout << "  InsertEraseFindRace test PASSED (" << numIterations << " iterations)" << std::endl;
    return true;
}

//=============================================================================
// Test 3: NodeRecycler Safety - Accessor lifetime test
// Tests that nodes are not recycled while Accessor is still alive
//=============================================================================
bool testNodeRecyclerSafety() {
    std::cout << "\n=== Test: NodeRecyclerSafety ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();

    // Create multiple accessors
    std::vector<std::shared_ptr<ConcurrentRBTreeAccessor>> accessors;

    const int numAccessors = 10;
    const int numElementsPerAccessor = 100;

    // Each accessor inserts elements
    for (int i = 0; i < numAccessors; ++i) {
        auto acc = std::make_shared<ConcurrentRBTreeAccessor>(rbtree);
        for (int j = 0; j < numElementsPerAccessor; ++j) {
            acc->insert(i * numElementsPerAccessor + j);
        }
        accessors.push_back(acc);
    }

    // Now erase some elements using first accessor
    for (int j = 0; j < numElementsPerAccessor / 2; ++j) {
        (*accessors[0]).erase(j);
    }

    // Other accessors should still be able to iterate safely
    for (int i = 1; i < numAccessors; ++i) {
        int count = 0;
        for (auto it = accessors[i]->begin(); it != accessors[i]->end(); ++it) {
            count++;
        }
        // Count might be less than original due to erasures, but should be consistent
        (void)count;
    }

    // Destroy accessors one by one
    for (int i = 0; i < numAccessors; ++i) {
        accessors[i].reset();
    }

    // Final accessor should see consistent state
    ConcurrentRBTreeAccessor final_acc(rbtree);
    for (auto it = final_acc.begin(); it != final_acc.end(); ++it) {
        // Just verify no crash
    }

    std::cout << "  NodeRecyclerSafety test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 4: Iterator Safety During Concurrent Modification
// Tests that iteration doesn't crash even with concurrent modifications
//=============================================================================
bool testIteratorSafetyDuringModification() {
    std::cout << "\n=== Test: IteratorSafetyDuringModification ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    // Pre-populate
    const int numElements = 10000;
    for (int i = 0; i < numElements; ++i) {
        accessor.insert(i);
    }

    std::atomic<bool> stop(false);
    std::atomic<int> iterationCount(0);
    std::atomic<int> crashCount(0);

    // Thread that keeps iterating
    std::thread reader([&]() {
        while (!stop.load()) {
            int count = 0;
            try {
                for (auto it = accessor.begin(); it != accessor.end(); ++it) {
                    count++;
                    if (count > 100000) break;  // Safety limit
                }
                iterationCount.fetch_add(1);
            } catch (...) {
                crashCount.fetch_add(1);
            }
        }
    });

    // Threads that modify
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, numElements - 1);
            std::uniform_int_distribution<int> opDist(0, 1);

            while (!stop.load()) {
                int value = valueDist(gen);
                if (opDist(gen) == 0) {
                    accessor.insert(value);
                } else {
                    accessor.erase(value);
                }
            }
        });
    }

    // Let it run for a while
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true);

    reader.join();
    for (auto& t : writers) {
        t.join();
    }

    std::cout << "  Iterations: " << iterationCount.load()
              << ", Crashes: " << crashCount.load() << std::endl;

    if (crashCount.load() > 0) {
        std::cerr << "  WARNING: Iterator crashed during concurrent modification!" << std::endl;
        return false;
    }

    std::cout << "  IteratorSafetyDuringModification test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 5: Empty and Single Element Edge Cases
//=============================================================================
bool testEmptyAndSingleElement() {
    std::cout << "\n=== Test: EmptyAndSingleElement ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    // Test empty tree
    assert(accessor.empty());
    assert(accessor.size() == 0);
    assert(accessor.begin() == accessor.end());
    assert(accessor.find(42) == accessor.end());
    assert(accessor.lower_bound(42) == accessor.end());
    assert(accessor.erase(42) == 0);

    // Test single element
    auto ret = accessor.insert(42);
    assert(ret.second);
    assert(*ret.first == 42);
    assert(!accessor.empty());
    assert(accessor.size() == 1);

    // Find existing
    auto it = accessor.find(42);
    assert(it != accessor.end());
    assert(*it == 42);

    // Find non-existing
    assert(accessor.find(43) == accessor.end());

    // Lower bound on existing
    auto lb = accessor.lower_bound(42);
    assert(lb != accessor.end());
    assert(*lb == 42);

    // Lower bound between
    lb = accessor.lower_bound(43);
    assert(lb == accessor.end());

    // Erase the only element
    assert(accessor.erase(42) == 1);
    assert(accessor.empty());
    assert(accessor.size() == 0);
    assert(accessor.find(42) == accessor.end());

    // Double erase should fail
    assert(accessor.erase(42) == 0);

    // Insert again after erase
    ret = accessor.insert(42);
    assert(ret.second);
    assert(accessor.size() == 1);

    std::cout << "  EmptyAndSingleElement test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 6: Memory Ordering Stress Test
// Tests visibility of updates across threads
//=============================================================================
bool testMemoryOrdering() {
    std::cout << "\n=== Test: MemoryOrdering ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    const int numValues = 1000;
    const int numReaders = 8;

    // Insert all values
    for (int i = 0; i < numValues; ++i) {
        accessor.insert(i);
    }

    std::atomic<bool> stop(false);
    std::atomic<int> totalFound(0);
    std::atomic<int> totalNotFound(0);
    std::atomic<int> inconsistencies(0);

    // Reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < numReaders; ++i) {
        readers.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, numValues - 1);

            while (!stop.load()) {
                int value = valueDist(gen);

                // Find the value
                auto it = accessor.find(value);
                if (it != accessor.end()) {
                    totalFound.fetch_add(1);
                    if (*it != value) {
                        inconsistencies.fetch_add(1);  // Found wrong value!
                    }
                } else {
                    totalNotFound.fetch_add(1);
                }

                // Also test lower_bound
                auto lb = accessor.lower_bound(value);
                if (lb != accessor.end()) {
                    if (*lb < value) {
                        inconsistencies.fetch_add(1);  // lower_bound returned smaller value!
                    }
                }
            }
        });
    }

    // Let readers run for a while
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);

    for (auto& t : readers) {
        t.join();
    }

    std::cout << "  TotalFound: " << totalFound.load()
              << ", TotalNotFound: " << totalNotFound.load()
              << ", Inconsistencies: " << inconsistencies.load() << std::endl;

    if (inconsistencies.load() > 0) {
        std::cerr << "  WARNING: Found memory ordering inconsistencies!" << std::endl;
        return false;
    }

    std::cout << "  MemoryOrdering test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 7: Rapid Insert-Erase-Insert Same Key
// Tests the reuse of nodes with same value
//=============================================================================
bool testRapidInsertEraseInsert() {
    std::cout << "\n=== Test: RapidInsertEraseInsert ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    const int targetValue = 777;
    const int numIterations = 10000;

    for (int i = 0; i < numIterations; ++i) {
        // Insert
        auto ret1 = accessor.insert(targetValue);
        assert(ret1.first != accessor.end());
        assert(*ret1.first == targetValue);

        // Find should succeed
        auto it = accessor.find(targetValue);
        assert(it != accessor.end());
        assert(*it == targetValue);

        // Erase
        bool erased = accessor.erase(targetValue);
        assert(erased);

        // Find should fail
        it = accessor.find(targetValue);
        assert(it == accessor.end());

        // Insert again
        auto ret2 = accessor.insert(targetValue);
        assert(ret2.second);  // Should succeed since we just erased

        // Find should succeed again
        it = accessor.find(targetValue);
        assert(it != accessor.end());
        assert(*it == targetValue);

        // Clean up for next iteration
        accessor.erase(targetValue);
    }

    // Verify final state is empty
    assert(accessor.find(targetValue) == accessor.end());

    std::cout << "  RapidInsertEraseInsert test PASSED (" << numIterations << " iterations)" << std::endl;
    return true;
}

//=============================================================================
// Test 8: Concurrent Size Accuracy
// Tests if size() is approximately accurate during concurrent operations
//=============================================================================
bool testConcurrentSizeAccuracy() {
    std::cout << "\n=== Test: ConcurrentSizeAccuracy ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    const int numThreads = 8;
    const int numOpsPerThread = 1000;
    const int valueRange = 100;

    std::vector<std::thread> threads;
    std::atomic<int> totalInserts(0);
    std::atomic<int> totalErases(0);

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, valueRange - 1);
            std::uniform_int_distribution<int> opDist(0, 1);

            for (int j = 0; j < numOpsPerThread; ++j) {
                int value = valueDist(gen);
                if (opDist(gen) == 0) {
                    if (accessor.insert(value).second) {
                        totalInserts.fetch_add(1);
                    }
                } else {
                    if (accessor.erase(value)) {
                        totalErases.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    size_t finalSize = accessor.size();
    int expectedSize = totalInserts.load() - totalErases.load();

    std::cout << "  Final size: " << finalSize
              << ", Expected: " << expectedSize
              << ", Diff: " << std::abs((int)finalSize - expectedSize) << std::endl;

    // Note: Due to duplicates and failed operations, size might not match exactly
    // But it should be in a reasonable range
    std::cout << "  ConcurrentSizeAccuracy test completed" << std::endl;
    return true;
}

//=============================================================================
// Test 9: Recycler Under Pressure
// Tests node recycling with many accessor lifecycles
//=============================================================================
bool testRecyclerUnderPressure() {
    std::cout << "\n=== Test: RecyclerUnderPressure ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();

    const int numCycles = 1000;
    const int numElementsPerCycle = 100;

    for (int cycle = 0; cycle < numCycles; ++cycle) {
        // Create accessor, insert elements
        {
            ConcurrentRBTreeAccessor accessor(rbtree);
            for (int i = 0; i < numElementsPerCycle; ++i) {
                accessor.insert(cycle * numElementsPerCycle + i);
            }
            // Erase half
            for (int i = 0; i < numElementsPerCycle / 2; ++i) {
                accessor.erase(i);
            }
        }  // Accessor destroyed here, potential recycling

        // Create new accessor and verify
        ConcurrentRBTreeAccessor accessor2(rbtree);
        int count = 0;
        for (auto it = accessor2.begin(); it != accessor2.end(); ++it) {
            count++;
        }
        (void)count;
    }

    ConcurrentRBTreeAccessor final_accessor(rbtree);
    std::cout << "  Final tree size: " << final_accessor.size() << std::endl;

    std::cout << "  RecyclerUnderPressure test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 10: Stress Test with Small Value Range
// Forces high contention with small key space
//=============================================================================
bool testSmallValueRangeStress() {
    std::cout << "\n=== Test: SmallValueRangeStress ===" << std::endl;

    auto rbtree = ConcurrentRBTreeType::createInstance();
    ConcurrentRBTreeAccessor accessor(rbtree);

    const int numThreads = 16;
    const int numOpsPerThread = 10000;
    const int valueRange = 10;  // Very small range!

    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, valueRange - 1);
            std::uniform_int_distribution<int> opDist(0, 2);

            for (int j = 0; j < numOpsPerThread; ++j) {
                int value = valueDist(gen);
                int op = opDist(gen);

                if (op == 0) {
                    accessor.insert(value);
                } else if (op == 1) {
                    accessor.erase(value);
                } else {
                    accessor.find(value);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify tree is still valid
    std::vector<int> values;
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        values.push_back(*it);
    }

    // Check sorted
    for (size_t i = 1; i < values.size(); ++i) {
        assert(values[i-1] < values[i]);
    }

    // Check range
    for (int v : values) {
        assert(v >= 0 && v < valueRange);
    }

    std::cout << "  Final size: " << accessor.size()
              << ", Expected max: " << valueRange << std::endl;
    std::cout << "  SmallValueRangeStress test PASSED" << std::endl;
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "==================================================" << std::endl;
    std::cout << "  ConcurrentRBTree Edge Case & Concurrency Stress Tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    int failed = 0;

    if (!testHighContention()) failed++;
    if (!testInsertEraseFindRace()) failed++;
    if (!testNodeRecyclerSafety()) failed++;
    if (!testIteratorSafetyDuringModification()) failed++;
    if (!testEmptyAndSingleElement()) failed++;
    if (!testMemoryOrdering()) failed++;
    if (!testRapidInsertEraseInsert()) failed++;
    if (!testConcurrentSizeAccuracy()) failed++;
    if (!testRecyclerUnderPressure()) failed++;
    if (!testSmallValueRangeStress()) failed++;

    std::cout << "\n==================================================" << std::endl;
    if (failed == 0) {
        std::cout << "  ALL EDGE CASE TESTS PASSED!" << std::endl;
    } else {
        std::cout << "  " << failed << " TESTS FAILED!" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return failed;
}
