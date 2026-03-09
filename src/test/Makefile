# ConcurrentRBTree Test Makefile

# Compiler settings
CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -g
INCLUDE_DIR = -I../include

# Google Test (for accuracy tests)
GTEST_DIR = /opt/homebrew
GTEST_INCLUDE = -I$(GTEST_DIR)/include
GTEST_LINK = -L$(GTEST_DIR)/lib -lgtest -lgtest_main

# Google Benchmark (for performance tests)
BENCHMARK_DIR = /opt/homebrew
BENCHMARK_INCLUDE = -I$(BENCHMARK_DIR)/include
BENCHMARK_LINK = -L$(BENCHMARK_DIR)/lib -lbenchmark

# Folly library (for skiplist comparison)
FOLLY_DIR = /opt/homebrew
FOLLY_INCLUDE = -I$(FOLLY_DIR)/include
FOLLY_LINK = -lfolly

# Directories
ACCURACY_TEST_DIR = accuracy_test
COMPARISION_TEST_DIR = comparision_test
BUILD_DIR = ../../build

# Accuracy test source files
ACCURACY_TEST_SOURCES = $(wildcard $(ACCURACY_TEST_DIR)/*.cpp)
ACCURACY_TEST_BINS = $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.cpp,%,$(ACCURACY_TEST_SOURCES))))

# Performance test binary
PERF_TEST_BIN = $(BUILD_DIR)/perf_test

.PHONY: all accuracy perf clean test run-perf

all: accuracy

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Accuracy tests (only need Google Test)
accuracy: $(ACCURACY_TEST_BINS)

$(ACCURACY_TEST_BINS): $(BUILD_DIR)/%: $(ACCURACY_TEST_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDE_DIR) $(GTEST_INCLUDE) $< -o $@ -pthread $(GTEST_LINK)

# Performance test (requires Google Benchmark and Folly)
perf: $(PERF_TEST_BIN)

$(PERF_TEST_BIN): $(COMPARISION_TEST_DIR)/test_perf.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDE_DIR) $(BENCHMARK_INCLUDE) $(FOLLY_INCLUDE) $^ -o $@ -pthread $(BENCHMARK_LINK) $(FOLLY_LINK)

# Run all accuracy tests
test: accuracy
	@for test in $(ACCURACY_TEST_BINS); do \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done

# Run performance test
run-perf: perf
	$(PERF_TEST_BIN) --benchmark_format=json > ../../results.json

clean:
	rm -rf $(BUILD_DIR)
