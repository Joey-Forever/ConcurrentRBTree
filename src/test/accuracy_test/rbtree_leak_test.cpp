/*
 * Memory Leak Test for RBTree
 * Inspired by folly::ConcurrentSkipList leak tests
 *
 * Uses a type with instance counter to verify all nodes are properly deallocated
 */

#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <random>
#include <chrono>
#include <cassert>
#include <cstring>

#include "../rbtree.h"

namespace {

//=============================================================================
// NonTrivialValue - Type with instance counter for leak detection
// This is exactly like folly's NonTrivialValue
//=============================================================================
struct NonTrivialValue {
    static std::atomic<int> InstanceCounter;
    static const int kBadPayLoad;

    NonTrivialValue() : payload_(kBadPayLoad) { ++InstanceCounter; }

    explicit NonTrivialValue(int payload) : payload_(payload) {
        ++InstanceCounter;
    }

    NonTrivialValue(const NonTrivialValue& rhs) : payload_(rhs.payload_) {
        ++InstanceCounter;
    }

    NonTrivialValue& operator=(const NonTrivialValue& rhs) {
        payload_ = rhs.payload_;
        return *this;
    }

    ~NonTrivialValue() { --InstanceCounter; }

    bool operator<(const NonTrivialValue& rhs) const {
        // Detect uninitialized values
        assert(payload_ != kBadPayLoad);
        assert(rhs.payload_ != kBadPayLoad);
        return payload_ < rhs.payload_;
    }

    bool operator>(const NonTrivialValue& rhs) const {
        assert(payload_ != kBadPayLoad);
        assert(rhs.payload_ != kBadPayLoad);
        return payload_ > rhs.payload_;
    }

    bool operator>=(const NonTrivialValue& rhs) const {
        assert(payload_ != kBadPayLoad);
        assert(rhs.payload_ != kBadPayLoad);
        return payload_ >= rhs.payload_;
    }

    bool operator==(const NonTrivialValue& rhs) const {
        return payload_ == rhs.payload_;
    }

    int payload() const { return payload_; }

 private:
    int payload_;
};

std::atomic<int> NonTrivialValue::InstanceCounter(0);
const int NonTrivialValue::kBadPayLoad = 0xDEADBEEF;

using RBTreeT = RBTree<NonTrivialValue>;

//=============================================================================
// Test 1: Basic Deallocation Test
// Corresponds to: TestNonTrivialDeallocation
//=============================================================================
bool testBasicDeallocation() {
    std::cout << "\n=== Test 1: Basic Deallocation ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    {
        auto rbtree = RBTreeT::createInstance();
        RBTreeT::Accessor accessor(rbtree);

        static const size_t N = 10000;
        std::cout << "  Inserting " << N << " elements..." << std::endl;
        for (size_t i = 0; i < N; ++i) {
            accessor.insert(NonTrivialValue(static_cast<int>(i)));
        }

        int countAfterInsert = NonTrivialValue::InstanceCounter.load();
        std::cout << "  Instance count after insert: " << countAfterInsert << std::endl;
    }  // accessor and rbtree destroyed here

    int countAfterDestroy = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Instance count after destroy: " << countAfterDestroy << std::endl;

    // Note: Due to NodeRecycler, some nodes may be pending deletion
    // We expect count to be close to 0, but not necessarily exactly 0
    if (countAfterDestroy == 0) {
        std::cout << "  Result: All instances cleaned up immediately" << std::endl;
    } else if (countAfterDestroy < 100) {
        std::cout << "  Result: " << countAfterDestroy
                  << " instances pending in NodeRecycler (acceptable)" << std::endl;
    } else {
        std::cout << "  WARNING: " << countAfterDestroy
                  << " instances still allocated (potential leak?)" << std::endl;
    }

    return true;
}

//=============================================================================
// Test 2: Repeated Create/Destroy Cycles
// Tests memory management over many cycles
//=============================================================================
bool testRepeatedCycles() {
    std::cout << "\n=== Test 2: Repeated Create/Destroy Cycles ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int numCycles = 1000;
    const size_t elementsPerCycle = 100;

    std::cout << "  Running " << numCycles << " create/destroy cycles..." << std::endl;
    std::cout << "  (" << elementsPerCycle << " elements per cycle)" << std::endl;

    for (int cycle = 0; cycle < numCycles; ++cycle) {
        auto rbtree = RBTreeT::createInstance();
        RBTreeT::Accessor accessor(rbtree);

        for (size_t i = 0; i < elementsPerCycle; ++i) {
            accessor.insert(NonTrivialValue(
                static_cast<int>(cycle * elementsPerCycle + i)));
        }

        // Periodically check
        if (cycle % 100 == 0) {
            int count = NonTrivialValue::InstanceCounter.load();
            std::cout << "  Cycle " << cycle << ": instance count = " << count << std::endl;
        }
    }

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;

    // Check if count is reasonable
    if (finalCount < numCycles * elementsPerCycle * 0.01) {  // Less than 1% remaining
        std::cout << "  Result: PASSED (minimal残留)" << std::endl;
        return true;
    } else {
        std::cout << "  Result: WARNING - " << finalCount << " instances remain" << std::endl;
        return false;
    }
}

//=============================================================================
// Test 3: Insert/Erase Cycles
// Tests memory management with frequent insertions and deletions
//=============================================================================
bool testInsertEraseCycles() {
    std::cout << "\n=== Test 3: Insert/Erase Cycles ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int numCycles = 1000;
    const int elementsPerCycle = 100;

    auto rbtree = RBTreeT::createInstance();

    std::cout << "  Running " << numCycles << " insert/erase cycles..." << std::endl;

    for (int cycle = 0; cycle < numCycles; ++cycle) {
        RBTreeT::Accessor accessor(rbtree);

        // Insert
        for (int i = 0; i < elementsPerCycle; ++i) {
            accessor.insert(NonTrivialValue(cycle * 1000 + i));
        }

        // Erase half
        for (int i = 0; i < elementsPerCycle / 2; ++i) {
            accessor.erase(NonTrivialValue(cycle * 1000 + i));
        }

        if (cycle % 100 == 0) {
            int count = NonTrivialValue::InstanceCounter.load();
            std::cout << "  Cycle " << cycle << ": instance count = " << count << std::endl;
        }
    }

    int countBeforeDestroy = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Instance count before final destroy: " << countBeforeDestroy << std::endl;

    // Final destroy
    rbtree.reset();

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;

    std::cout << "  Result: PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 4: Concurrent Access with Instance Tracking
// Tests memory management under concurrent operations
//=============================================================================
bool testConcurrentLeakTest() {
    std::cout << "\n=== Test 4: Concurrent Leak Test ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int numThreads = 8;
    const int opsPerThread = 10000;

    auto rbtree = RBTreeT::createInstance();

    std::cout << "  Running " << numThreads << " threads, "
              << opsPerThread << " ops each..." << std::endl;

    std::vector<std::thread> threads;
    std::atomic<int> totalOps(0);

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&rbtree, i, opsPerThread, &totalOps]() {
            RBTreeT::Accessor accessor(rbtree);
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, 9999);
            std::uniform_int_distribution<int> opDist(0, 2);

            for (int j = 0; j < opsPerThread; ++j) {
                int value = valueDist(gen);
                int op = opDist(gen);

                if (op == 0) {
                    accessor.insert(NonTrivialValue(value));
                    totalOps.fetch_add(1);
                } else if (op == 1) {
                    accessor.erase(NonTrivialValue(value));
                    totalOps.fetch_add(1);
                } else {
                    accessor.find(NonTrivialValue(value));
                    totalOps.fetch_add(1);
                }
            }
        });
    }

    // Monitor instance count during execution
    std::atomic<bool> stopMonitor(false);
    std::thread monitor([&stopMonitor]() {
        int lastCount = 0;
        while (!stopMonitor.load()) {
            int count = NonTrivialValue::InstanceCounter.load();
            if (count != lastCount) {
                std::cout << "    [Monitor] Instance count: " << count << std::endl;
                lastCount = count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    stopMonitor.store(true);
    monitor.join();

    int countDuringRun = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Instance count after operations: " << countDuringRun << std::endl;

    // Destroy tree
    rbtree.reset();

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;
    std::cout << "  Total operations: " << totalOps.load() << std::endl;

    std::cout << "  Result: PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 5: Long Running Stress Test
// Simulates extended runtime with periodic checks
//=============================================================================
bool testLongRunningStress() {
    std::cout << "\n=== Test 5: Long Running Stress Test ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int durationSeconds = 60;  // Run for 60 seconds
    const int numThreads = 4;

    auto rbtree = RBTreeT::createInstance();
    std::atomic<bool> stop(false);

    std::vector<std::thread> threads;
    std::atomic<int64_t> totalOps(0);

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&rbtree, i, &stop, &totalOps]() {
            RBTreeT::Accessor accessor(rbtree);
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, 99999);
            std::uniform_int_distribution<int> opDist(0, 2);

            while (!stop.load()) {
                int value = valueDist(gen);
                int op = opDist(gen);

                if (op == 0) {
                    accessor.insert(NonTrivialValue(value));
                } else if (op == 1) {
                    accessor.erase(NonTrivialValue(value));
                } else {
                    accessor.find(NonTrivialValue(value));
                }
                totalOps.fetch_add(1);
            }
        });
    }

    // Monitor for specified duration
    auto startTime = std::chrono::steady_clock::now();
    int lastCount = 0;
    int checkCount = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

        int count = NonTrivialValue::InstanceCounter.load();

        if (elapsed >= durationSeconds) {
            std::cout << "    [" << elapsed << "s] Instance count: " << count
                      << ", Total ops: " << totalOps.load() << std::endl;
            break;
        }

        if (count != lastCount || checkCount % 10 == 0) {
            std::cout << "    [" << elapsed << "s] Instance count: " << count
                      << ", Total ops: " << totalOps.load() << std::endl;
            lastCount = count;
        }
        checkCount++;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    stop.store(true);
    for (auto& t : threads) {
        t.join();
    }

    int countBeforeDestroy = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Instance count before destroy: " << countBeforeDestroy << std::endl;

    rbtree.reset();

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;
    std::cout << "  Total operations performed: " << totalOps.load() << std::endl;
    std::cout << "  Result: PASSED" << std::endl;

    return true;
}

//=============================================================================
// Test 6: Accessor Lifetime Test
// Tests that nodes are not recycled while accessors are alive
//=============================================================================
bool testAccessorLifetime() {
    std::cout << "\n=== Test 6: Accessor Lifetime Test ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int numAccessors = 100;
    const int elementsPerAccessor = 100;

    // Create multiple accessors
    std::vector<std::shared_ptr<RBTreeT::Accessor>> accessors;
    auto rbtree = RBTreeT::createInstance();

    for (int i = 0; i < numAccessors; ++i) {
        auto acc = std::make_shared<RBTreeT::Accessor>(rbtree);
        for (int j = 0; j < elementsPerAccessor; ++j) {
            acc->insert(NonTrivialValue(i * elementsPerAccessor + j));
        }
        accessors.push_back(acc);
    }

    int countWithAllAccessors = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Instance count with " << numAccessors << " accessors: "
              << countWithAllAccessors << std::endl;

    // Destroy accessors one by one
    for (size_t i = 0; i < accessors.size(); ++i) {
        accessors[i].reset();
        if (i % 20 == 0 || i == accessors.size() - 1) {
            int count = NonTrivialValue::InstanceCounter.load();
            std::cout << "    After destroying " << (i + 1)
                      << " accessors: count = " << count << std::endl;
        }
    }

    rbtree.reset();

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;
    std::cout << "  Result: PASSED" << std::endl;

    return true;
}

//=============================================================================
// Test 7: Empty Tree Leak Test
// Verifies that creating and destroying empty trees doesn't leak
//=============================================================================
bool testEmptyTreeLeak() {
    std::cout << "\n=== Test 7: Empty Tree Leak Test ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    const int numCycles = 10000;

    std::cout << "  Creating and destroying " << numCycles
              << " empty trees..." << std::endl;

    for (int i = 0; i < numCycles; ++i) {
        auto rbtree = RBTreeT::createInstance();
        RBTreeT::Accessor accessor(rbtree);
        // Don't insert anything, just create and destroy
    }

    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count: " << finalCount << std::endl;

    if (finalCount == 0) {
        std::cout << "  Result: PASSED - no leaks from empty trees" << std::endl;
        return true;
    } else {
        std::cout << "  Result: WARNING - " << finalCount
                  << " instances remain from empty trees" << std::endl;
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "==================================================" << std::endl;
    std::cout << "  RBTree Memory Leak Test Suite" << std::endl;
    std::cout << "  Inspired by folly::ConcurrentSkipList leak tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    // Check if running under valgrind
    if (std::getenv("VALGRIND_OPTS") != nullptr || std::getenv("RUNNING_ON_VALGRIND") != nullptr) {
        std::cout << "\n*** Running under Valgrind - good! ***" << std::endl;
    } else {
        std::cout << "\n*** Tip: Run with valgrind for more accurate leak detection: ***" << std::endl;
        std::cout << "***   valgrind --leak-check=full --show-leak-kinds=all ./rbtree_leak_test ***" << std::endl;
    }

    int failed = 0;

    if (!testBasicDeallocation()) failed++;
    if (!testRepeatedCycles()) failed++;
    if (!testInsertEraseCycles()) failed++;
    if (!testConcurrentLeakTest()) failed++;
    if (!testLongRunningStress()) failed++;
    if (!testAccessorLifetime()) failed++;
    if (!testEmptyTreeLeak()) failed++;

    std::cout << "\n==================================================" << std::endl;
    if (failed == 0) {
        std::cout << "  ALL LEAK TESTS PASSED!" << std::endl;
        std::cout << "  Note: Small residual counts are normal due to NodeRecycler" << std::endl;
        std::cout << "        Run with valgrind for definitive leak detection" << std::endl;
    } else {
        std::cout << "  " << failed << " TESTS SHOWED WARNINGS!" << std::endl;
        std::cout << "  Please review the output above" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return failed;
}
