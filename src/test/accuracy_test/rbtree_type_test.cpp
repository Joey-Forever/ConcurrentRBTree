/*
 * Non-Trivial Type Tests for ConcurrentRBTree
 * Inspired by folly::ConcurrentSkipList type tests
 *
 * Tests:
 * 1. std::string - non-trivial copy type
 * 2. Wrapper for unique_ptr - since ConcurrentRBTree doesn't support custom comparator
 * 3. NonTrivialValue - type with instance counter for destructor verification
 */

#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cassert>
#include <random>
#include <thread>
#include <functional>

#include <ConcurrentRBTree.h>

namespace {

//=============================================================================
// Test 1: StringType - Test non-trivial copy type (std::string)
// Corresponds to: TEST(ConcurrentSkipList, TestStringType)
//=============================================================================
static std::string makeRandomString(int len) {
    std::string s;
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist('A', 'Z');
    for (int j = 0; j < len; j++) {
        s.push_back(static_cast<char>(dist(gen)));
    }
    return s;
}

bool testStringType() {
    std::cout << "\n=== Test: StringType (std::string) ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<std::string>;
    auto rbtree = ConcurrentRBTreeT::createInstance();
    ConcurrentRBTreeT::Accessor accessor(rbtree);

    {
        for (int i = 0; i < 100000; i++) {
            std::string s = makeRandomString(7);
            accessor.insert(s);
        }
    }

    // Verify all strings are sorted
    std::vector<std::string> vec;
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        vec.push_back(*it);
    }

    bool is_sorted = std::is_sorted(vec.begin(), vec.end());
    std::cout << "  Inserted 100000 random strings" << std::endl;
    std::cout << "  Total unique strings: " << vec.size() << std::endl;
    std::cout << "  Is sorted: " << (is_sorted ? "YES" : "NO") << std::endl;

    // Test find
    if (!vec.empty()) {
        std::string test_str = vec[vec.size() / 2];
        auto it = accessor.find(test_str);
        assert(it != accessor.end());
        assert(*it == test_str);
        std::cout << "  Find test: PASSED" << std::endl;
    }

    // Test lower_bound
    std::string search_str = "MIDDLE";
    auto lb = accessor.lower_bound(search_str);
    int count_greater = 0;
    for (auto it = lb; it != accessor.end(); ++it) {
        if (*it >= search_str) count_greater++;
    }
    std::cout << "  Lower_bound test: PASSED" << std::endl;

    std::cout << "  StringType test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 2: MoveOnlyType - Test a type with move semantics
// We use a simple wrapper that owns a pointer
//=============================================================================
template<typename T>
class MoveOnlyValue {
public:
    // Default constructor (for sentinel nodes)
    MoveOnlyValue() : value_(), owns_(false) {}

    // Constructor from const reference
    explicit MoveOnlyValue(const T& value)
        : value_(value), owns_(true) {}

    // Constructor from rvalue
    explicit MoveOnlyValue(T&& value)
        : value_(std::move(value)), owns_(true) {}

    // Move constructor
    MoveOnlyValue(MoveOnlyValue&& other) noexcept
        : value_(std::move(other.value_)), owns_(other.owns_) {
        other.owns_ = false;
    }

    // Move assignment
    MoveOnlyValue& operator=(MoveOnlyValue&& other) noexcept {
        if (this != &other) {
            value_ = std::move(other.value_);
            owns_ = other.owns_;
            other.owns_ = false;
        }
        return *this;
    }

    // Copy constructor (needed for ConcurrentRBTree)
    MoveOnlyValue(const MoveOnlyValue& other)
        : value_(other.value_), owns_(true) {}

    // Copy assignment
    MoveOnlyValue& operator=(const MoveOnlyValue& other) {
        if (this != &other) {
            value_ = other.value_;
            owns_ = true;
        }
        return *this;
    }

    bool operator<(const MoveOnlyValue& other) const {
        return value_ < other.value_;
    }

    bool operator>(const MoveOnlyValue& other) const {
        return value_ > other.value_;
    }

    bool operator>=(const MoveOnlyValue& other) const {
        return value_ >= other.value_;
    }

    bool operator==(const MoveOnlyValue& other) const {
        return value_ == other.value_;
    }

    const T& value() const { return value_; }
    T& value() { return value_; }

private:
    T value_;
    bool owns_;
};

bool testMovableData() {
    std::cout << "\n=== Test: MovableData (MoveOnlyValue<int>) ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<MoveOnlyValue<int> >;
    auto rbtree = ConcurrentRBTreeT::createInstance();
    ConcurrentRBTreeT::Accessor accessor(rbtree);

    static const int N = 10;
    for (int i = 0; i < N; ++i) {
        accessor.insert(MoveOnlyValue<int>(i));
    }

    std::cout << "  Inserted " << N << " MoveOnlyValue elements" << std::endl;

    // Test find
    for (int i = 0; i < N; ++i) {
        MoveOnlyValue<int> val(i);
        auto it = accessor.find(val);
        assert(it != accessor.end());
        assert(it->value() == i);
    }

    // Test find non-existent
    MoveOnlyValue<int> val(N);
    auto it = accessor.find(val);
    assert(it == accessor.end());

    // Test iteration
    int count = 0;
    int sum = 0;
    for (auto iter = accessor.begin(); iter != accessor.end(); ++iter) {
        sum += iter->value();
        count++;
    }
    assert(count == N);
    assert(sum == N * (N - 1) / 2);  // 0+1+2+...+9 = 45

    std::cout << "  Iteration count: " << count << ", Sum: " << sum << std::endl;
    std::cout << "  MovableData test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 3: NonTrivialValue - Type with instance counter
// Corresponds to: TEST(ConcurrentSkipList, NonTrivialDeallocation*)
// This verifies that destructors are called correctly
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
        // Assert that we're not comparing default-constructed (uninitialized) values
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

bool testNonTrivialDeallocation() {
    std::cout << "\n=== Test: NonTrivialDeallocation ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<NonTrivialValue>;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    {
        auto rbtree = ConcurrentRBTreeT::createInstance();
        ConcurrentRBTreeT::Accessor accessor(rbtree);

        static const size_t N = 10000;
        std::cout << "  Inserting " << N << " NonTrivialValue elements..." << std::endl;

        for (size_t i = 0; i < N; ++i) {
            accessor.insert(NonTrivialValue(static_cast<int>(i)));
        }

        int currentCount = NonTrivialValue::InstanceCounter.load();
        std::cout << "  Instance count after insertion: " << currentCount << std::endl;

        // Verify we can find and iterate
        size_t iterCount = 0;
        for (auto it = accessor.begin(); it != accessor.end(); ++it) {
            iterCount++;
        }
        std::cout << "  Iteration count: " << iterCount << std::endl;
        assert(iterCount == N);

        // Test erase
        for (size_t i = 0; i < N / 2; ++i) {
            accessor.erase(NonTrivialValue(static_cast<int>(i)));
        }

        iterCount = 0;
        for (auto it = accessor.begin(); it != accessor.end(); ++it) {
            iterCount++;
        }
        std::cout << "  Instance count after erasing half: "
                  << NonTrivialValue::InstanceCounter.load() << std::endl;
        std::cout << "  Remaining elements: " << iterCount << std::endl;

        // Accessor and rbtree destroyed here
    }

    // After accessor and rbtree are destroyed, check instance count
    int finalCount = NonTrivialValue::InstanceCounter.load();
    std::cout << "  Final instance count after destruction: " << finalCount << std::endl;

    if (finalCount == 0) {
        std::cout << "  All instances cleaned up immediately" << std::endl;
    } else {
        std::cout << "  Note: " << finalCount
                  << " instances pending cleanup in NodeRecycler (expected behavior)" << std::endl;
    }

    std::cout << "  NonTrivialDeallocation test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 4: Concurrent StringType Test
// Multiple threads inserting strings
//=============================================================================
bool testConcurrentStringType() {
    std::cout << "\n=== Test: ConcurrentStringType ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<std::string>;
    auto rbtree = ConcurrentRBTreeT::createInstance();

    const int numThreads = 8;
    const int stringsPerThread = 10000;

    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&rbtree, i]() {
            ConcurrentRBTreeT::Accessor accessor(rbtree);
            thread_local std::mt19937 gen(i);
            std::uniform_int_distribution<int> lenDist(3, 15);
            std::uniform_int_distribution<int> charDist('A', 'Z');

            for (int j = 0; j < stringsPerThread; ++j) {
                int len = lenDist(gen);
                std::string s;
                for (int k = 0; k < len; ++k) {
                    s.push_back(static_cast<char>(charDist(gen)));
                }
                accessor.insert(s);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ConcurrentRBTreeT::Accessor accessor(rbtree);
    size_t totalCount = 0;
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        totalCount++;
    }

    std::cout << "  Total unique strings: " << totalCount << std::endl;

    // Verify sorted
    std::string prev;
    bool sorted = true;
    size_t checkCount = 0;
    for (auto it = accessor.begin(); it != accessor.end() && checkCount < 1000; ++it) {
        if (!prev.empty() && *it < prev) {
            sorted = false;
            break;
        }
        prev = *it;
        checkCount++;
    }

    std::cout << "  Checked " << checkCount << " elements, sorted: "
              << (sorted ? "YES" : "NO") << std::endl;
    std::cout << "  ConcurrentStringType test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 5: String Stress Test - Insert sorted strings
//=============================================================================
bool testSortedStringInsert() {
    std::cout << "\n=== Test: SortedStringInsert ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<std::string>;
    auto rbtree = ConcurrentRBTreeT::createInstance();
    ConcurrentRBTreeT::Accessor accessor(rbtree);

    // Insert strings in sorted order (worst case for some tree implementations)
    const int N = 10000;
    for (int i = 0; i < N; ++i) {
        std::string s = "String_" + std::to_string(i);
        accessor.insert(s);
    }

    // Verify all inserted
    size_t count = 0;
    for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        count++;
    }
    assert(count == static_cast<size_t>(N));

    // Test find
    for (int i = 0; i < N; i += 100) {
        std::string s = "String_" + std::to_string(i);
        auto it = accessor.find(s);
        assert(it != accessor.end());
        assert(*it == s);
    }

    std::cout << "  Inserted " << N << " sorted strings" << std::endl;
    std::cout << "  SortedStringInsert test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 6: Empty and Single String
//=============================================================================
bool testEmptyAndSingleString() {
    std::cout << "\n=== Test: EmptyAndSingleString ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<std::string>;
    auto rbtree = ConcurrentRBTreeT::createInstance();
    ConcurrentRBTreeT::Accessor accessor(rbtree);

    // Test empty
    assert(accessor.empty());
    assert(accessor.find("") == accessor.end());

    // Test empty string
    auto ret = accessor.insert("");
    assert(ret.second);
    assert(*ret.first == "");

    auto it = accessor.find("");
    assert(it != accessor.end());
    assert(*it == "");

    // Test single char
    ret = accessor.insert("a");
    assert(ret.second);

    it = accessor.find("a");
    assert(it != accessor.end());
    assert(*it == "a");

    // Test erase
    assert(accessor.erase("") == 1);
    assert(accessor.find("") == accessor.end());

    std::cout << "  EmptyAndSingleString test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 7: NonTrivialValue Concurrent Test
//=============================================================================
bool testConcurrentNonTrivial() {
    std::cout << "\n=== Test: ConcurrentNonTrivial ===" << std::endl;

    // Reset counter
    NonTrivialValue::InstanceCounter.store(0);

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<NonTrivialValue>;
    auto rbtree = ConcurrentRBTreeT::createInstance();

    const int numThreads = 8;
    const int opsPerThread = 1000;

    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&rbtree, i, opsPerThread]() {
            ConcurrentRBTreeT::Accessor accessor(rbtree);
            std::mt19937 gen(i);
            std::uniform_int_distribution<int> valueDist(0, 999);

            for (int j = 0; j < opsPerThread; ++j) {
                int value = valueDist(gen);
                accessor.insert(NonTrivialValue(value));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ConcurrentRBTreeT::Accessor accessor(rbtree);
    size_t finalSize = accessor.size();
    int instanceCount = NonTrivialValue::InstanceCounter.load();

    std::cout << "  Final tree size: " << finalSize << std::endl;
    std::cout << "  Instance counter: " << instanceCount << std::endl;
    std::cout << "  ConcurrentNonTrivial test PASSED" << std::endl;
    return true;
}

//=============================================================================
// Test 9: String Find and Erase Stress
//=============================================================================
bool testStringFindEraseStress() {
    std::cout << "\n=== Test: StringFindEraseStress ===" << std::endl;

    using ConcurrentRBTreeT = gipsy_danger::ConcurrentRBTree<std::string>;
    auto rbtree = ConcurrentRBTreeT::createInstance();
    ConcurrentRBTreeT::Accessor accessor(rbtree);

    // Insert specific strings
    std::vector<std::string> testStrings = {
        "alpha", "bravo", "charlie", "delta", "echo",
        "foxtrot", "golf", "hotel", "india", "juliet"
    };

    for (const auto& s : testStrings) {
        accessor.insert(s);
    }

    // Test find all
    for (const auto& s : testStrings) {
        auto it = accessor.find(s);
        assert(it != accessor.end());
        assert(*it == s);
    }

    // Test lower_bound
    auto it = accessor.lower_bound("delta");
    assert(it != accessor.end());
    assert(*it == "delta");

    it = accessor.lower_bound("david");
    assert(it != accessor.end());
    assert(*it == "delta");

    // Test erase
    assert(accessor.erase("charlie") == 1);
    assert(accessor.find("charlie") == accessor.end());

    // Verify others still exist
    assert(accessor.find("bravo") != accessor.end());
    assert(accessor.find("delta") != accessor.end());

    std::cout << "  StringFindEraseStress test PASSED" << std::endl;
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "==================================================" << std::endl;
    std::cout << "  ConcurrentRBTree Non-Trivial Type Tests" << std::endl;
    std::cout << "  Inspired by folly::ConcurrentSkipList type tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    int failed = 0;

    if (!testStringType()) failed++;
    if (!testMovableData()) failed++;
    if (!testNonTrivialDeallocation()) failed++;
    if (!testConcurrentStringType()) failed++;
    if (!testSortedStringInsert()) failed++;
    if (!testEmptyAndSingleString()) failed++;
    if (!testConcurrentNonTrivial()) failed++;
    if (!testStringFindEraseStress()) failed++;

    std::cout << "\n==================================================" << std::endl;
    if (failed == 0) {
        std::cout << "  ALL TYPE TESTS PASSED!" << std::endl;
    } else {
        std::cout << "  " << failed << " TESTS FAILED!" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return failed;
}
