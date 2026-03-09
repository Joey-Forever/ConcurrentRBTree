/*
 * All threads can perform both read and write operations.
 * X-axis: Write probability (1/N, where N is the frequency parameter)
 * Y-axis: Read throughput and Write throughput (ops/s)
 */

#ifndef TEST_PERF_H
#define TEST_PERF_H

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
#include <functional>
#include <memory>
#include <cstdint>

template<typename T>
static void TestMultiReadFewWriteConcurrentPerf(std::function<std::unique_ptr<T>()> accesor_generator) {
  std::cout << "------------------------------------------------------------------------------------------------" << "\n";
  std::cout << "concurrent test --- all threads can read/write/erase (varying write probability)" << "\n";
  std::cout << "Write operations: 50%% insert (50%% existing, 50%% non-existing), 50%% erase (50%% existing, 50%% non-existing)" << "\n";

  // 测试参数：所有线程都可以执行读写操作
  const int TOTAL_THREAD_COUNT = 16;
  const size_t INIT_DATA_SIZE = 4000000;
  const size_t MAX_OPERATIONS_PER_THREAD = 100000;
  const size_t ITERATIONS = 3;  // 每种写概率执行5次

  // 不同的写概率 (每N次操作执行一次写)
  std::vector<int> write_frequencies = {2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 50, 100, 200, 500, 1000};

  std::cout << "total threads: " << TOTAL_THREAD_COUNT << " (all can read/write/erase)" << "\n";
  std::cout << "init data size: " << INIT_DATA_SIZE << "\n";
  std::cout << "max operations per thread: " << MAX_OPERATIONS_PER_THREAD << "\n";
  std::cout << "iterations per write probability: " << ITERATIONS << "\n";
  std::cout << "\n";
  std::cout << "Write Prob | Read Throughput | Insert Throughput | Erase Throughput | Read Latency | Insert Latency | Erase Latency" << "\n";
  std::cout << "           | (ops/s)         | (ops/s)          | (ops/s)          | (ns)        | (ns)          | (ns)" << "\n";
  std::cout << "------------------------------------------------------------------------------------------------------------" << "\n";

  for (int write_frequency : write_frequencies) {
    // 存储（不在计时内）
    struct ThreadOps {
      std::vector<int> read_values;
      std::vector<int> insert_exist_values;
      std::vector<int> insert_not_exist_values;
      std::vector<int> erase_exist_values;
      std::vector<int> erase_not_exist_values;
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(INT32_MIN, INT32_MAX);

    // 计算 TOTAL_OPERATIONS_PER_THREAD
    // 约束条件：erase_exist + insert_exist 的数量不能超过 INIT_DATA_SIZE
    // 每 write_frequency 次操作有 1 次写操作
    // 写操作中 1/2 是 erase，1/2 是 insert
    // erase 中 write_count_local % 4 == 0 时是 erase_exist (1/4)
    // insert 中 write_count_local % 4 < 3 时是 insert_exist (1/4)
    // 所以每 4 次写操作中：1 次 erase_exist + 1 次 insert_exist + 1 次 erase_not_exist + 1 次 insert_not_exist
    // erase_exist + insert_exist 比例 = 1/2 * 1/write_frequency
    // 所以：TOTAL_OPERATIONS_PER_THREAD * TOTAL_THREAD_COUNT / (2 * write_frequency) <= INIT_DATA_SIZE
    // 因此：TOTAL_OPERATIONS_PER_THREAD <= INIT_DATA_SIZE * 2 * write_frequency / TOTAL_THREAD_COUNT
    // 为了确保精确计算，TOTAL_OPERATIONS_PER_THREAD 需要是 write_frequency * 4 的倍数
    size_t TOTAL_OPERATIONS_PER_THREAD = std::min(
      static_cast<size_t>(INIT_DATA_SIZE * 2 * write_frequency / TOTAL_THREAD_COUNT),
      MAX_OPERATIONS_PER_THREAD
    );
    // 调整为 write_frequency * 4 的倍数，确保精确计算
    size_t group_size = write_frequency * 4;
    TOTAL_OPERATIONS_PER_THREAD = (TOTAL_OPERATIONS_PER_THREAD / group_size) * group_size;

    // 计算每个线程需要的各类操作数量
    // 每个线程有 TOTAL_OPERATIONS_PER_THREAD 次操作
    // 其中 1/write_frequency 是写操作，写操作中 1/2 是 erase，erase 中 1/2 是 erase_exist
    // 写操作中 1/2 是 insert，insert 中 1/2 是 insert_exist
    size_t erase_exist_per_thread = 0;
    size_t erase_not_exist_per_thread = 0;
    size_t insert_exist_per_thread = 0;
    size_t insert_not_exist_per_thread = 0;
    size_t write_count_local = 0;
    for (size_t op = 0; op < TOTAL_OPERATIONS_PER_THREAD; op++) {
      if ((op % static_cast<size_t>(write_frequency)) == 0) {
        // 写操作
        if ((write_count_local % 2) == 0) {
          // erase操作
          if ((write_count_local % 4) < 1) {
            erase_exist_per_thread++;
          } else {
            erase_not_exist_per_thread++;
          }
        } else {
          // insert操作
          if ((write_count_local % 4) < 3) {
            insert_exist_per_thread++;
          } else {
            insert_not_exist_per_thread++;
          }
        }
        write_count_local++;
      }
    }
    size_t total_erase_exist_needed = erase_exist_per_thread * TOTAL_THREAD_COUNT;
    size_t total_erase_not_exist_needed = erase_not_exist_per_thread * TOTAL_THREAD_COUNT;
    size_t total_insert_exist_needed = insert_exist_per_thread * TOTAL_THREAD_COUNT;
    size_t total_insert_not_exist_needed = insert_not_exist_per_thread * TOTAL_THREAD_COUNT;

    // 固定使用 INIT_DATA_SIZE
    size_t init_data_needed = INIT_DATA_SIZE;

    assert(init_data_needed >= total_erase_exist_needed + total_insert_exist_needed);

    // 生成初始化数据 (不在计时内)
    std::set<int> total_eles;
    std::vector<int> init_data;
    while (init_data.size() < init_data_needed) {
      int a = dis(gen);
      if (total_eles.find(a) != total_eles.end()) { continue; }
      total_eles.insert(a);
      init_data.push_back(a);
    }

    // 生成四个操作序列（确保所有序列中的数据唯一）

    // 1. 生成 erase_exist_values - 从 init_data 中选择（init_data本身就是随机生成的）
    std::vector<int> erase_exist_values(init_data.begin(), init_data.begin() + total_erase_exist_needed);

    // 2. 生成 erase_not_exist_values - 不在 init_data 中，且内部不重复
    std::vector<int> erase_not_exist_values;
    erase_not_exist_values.reserve(total_erase_not_exist_needed);
    while (erase_not_exist_values.size() < total_erase_not_exist_needed) {
      int erase_val = dis(gen);
      if (total_eles.find(erase_val) == total_eles.end()) {
        erase_not_exist_values.push_back(erase_val);
        total_eles.insert(erase_val);
      }
    }

    // 3. 生成 insert_exist_values - 从 init_data 中选择，跳过 erase_exist_values 部分
    std::vector<int> insert_exist_values(
      init_data.begin() + total_erase_exist_needed,
      init_data.begin() + total_erase_exist_needed + total_insert_exist_needed
    );

    // 4. 生成 insert_not_exist_values - 必须不能与任何其他序列重复
    std::vector<int> insert_not_exist_values;
    insert_not_exist_values.reserve(total_insert_not_exist_needed);
    while (insert_not_exist_values.size() < total_insert_not_exist_needed) {
      int insert_val = dis(gen);
      if (total_eles.find(insert_val) == total_eles.end()) {
        insert_not_exist_values.push_back(insert_val);
        total_eles.insert(insert_val);
      }
    }

    // 更新 value_dis 范围（用于读操作）
    std::uniform_int_distribution<> updated_value_dis(0, static_cast<int>(init_data.size()) - 1);

    // 5. 生成 read_values - 从 init_data 中随机选择
    size_t total_read_needed = TOTAL_OPERATIONS_PER_THREAD * TOTAL_THREAD_COUNT - total_erase_exist_needed - total_erase_not_exist_needed - total_insert_exist_needed - total_insert_not_exist_needed;
    std::vector<int> read_values;
    read_values.reserve(total_read_needed);
    for (size_t i = 0; i < total_read_needed; i++) {
      read_values.push_back(init_data[updated_value_dis(gen)]);
    }

    // 为每个线程分配操作序列 - 直接拷贝对应片段
    std::vector<ThreadOps> thread_ops_list(TOTAL_THREAD_COUNT);
    for (size_t tid = 0; tid < TOTAL_THREAD_COUNT; tid++) {
      size_t erase_exist_start = tid * erase_exist_per_thread;
      size_t erase_not_exist_start = tid * erase_not_exist_per_thread;
      size_t insert_exist_start = tid * insert_exist_per_thread;
      size_t insert_not_exist_start = tid * insert_not_exist_per_thread;
      size_t read_start = tid * (total_read_needed / TOTAL_THREAD_COUNT);

      thread_ops_list[tid].erase_exist_values.assign(
        erase_exist_values.begin() + erase_exist_start,
        erase_exist_values.begin() + erase_exist_start + erase_exist_per_thread
      );
      thread_ops_list[tid].erase_not_exist_values.assign(
        erase_not_exist_values.begin() + erase_not_exist_start,
        erase_not_exist_values.begin() + erase_not_exist_start + erase_not_exist_per_thread
      );
      thread_ops_list[tid].insert_exist_values.assign(
        insert_exist_values.begin() + insert_exist_start,
        insert_exist_values.begin() + insert_exist_start + insert_exist_per_thread
      );
      thread_ops_list[tid].insert_not_exist_values.assign(
        insert_not_exist_values.begin() + insert_not_exist_start,
        insert_not_exist_values.begin() + insert_not_exist_start + insert_not_exist_per_thread
      );
      size_t read_per_thread = (tid == TOTAL_THREAD_COUNT - 1)
        ? (total_read_needed - read_start)
        : (total_read_needed / TOTAL_THREAD_COUNT);
      thread_ops_list[tid].read_values.assign(
        read_values.begin() + read_start,
        read_values.begin() + read_start + read_per_thread
      );

      // 断言：读操作序列size必须 >= 读操作次数
      size_t read_ops_needed = TOTAL_OPERATIONS_PER_THREAD - erase_exist_per_thread - erase_not_exist_per_thread - insert_exist_per_thread - insert_not_exist_per_thread;
      assert(thread_ops_list[tid].read_values.size() >= read_ops_needed);
    }

    // 存储每次迭代的结果
    struct RunResult {
      double read_throughput;
      double insert_throughput;
      double erase_throughput;
      double read_latency;
      double insert_latency;
      double erase_latency;
    };
    std::vector<RunResult> results;

    for (size_t iter = 0; iter < ITERATIONS; iter++) {
      std::unique_ptr<T> accessor = accesor_generator();

      // 初始化 (不在计时内)
      for (int ele : init_data) {
        accessor->insert(ele);
      }

      std::atomic<bool> start_flag(false);
      std::atomic<bool> stop_flag(false);

      // std::atomic<long long> total_read_ops(0);
      // std::atomic<long long> total_insert_ops(0);
      // std::atomic<long long> total_erase_ops(0);
      // std::atomic<long long> total_read_latency_ns(0);
      // std::atomic<long long> total_insert_latency_ns(0);
      // std::atomic<long long> total_erase_latency_ns(0);

      auto worker_ope = [&accessor, &start_flag, &stop_flag,
                         &thread_ops_list,
                        //  &total_read_ops, &total_insert_ops, &total_erase_ops,
                        //  &total_read_latency_ns, &total_insert_latency_ns, &total_erase_latency_ns,
                         write_frequency, TOTAL_OPERATIONS_PER_THREAD](int tid) {
        const auto& ops = thread_ops_list[tid];
        size_t read_idx = 0;
        size_t insert_exist_idx = 0;
        size_t insert_not_exist_idx = 0;
        size_t erase_exist_idx = 0;
        size_t erase_not_exist_idx = 0;
        size_t write_count = 0;

        while (!start_flag.load()) { std::this_thread::yield(); }

        for (size_t op_count = 0; op_count < TOTAL_OPERATIONS_PER_THREAD; op_count++) {
          if ((op_count % static_cast<size_t>(write_frequency)) == 0) {
            // 写操作: 1/2 erase, 1/2 insert
            if ((write_count % 2) == 0) {
              // erase操作: 1/2 存在的, 1/2 不存在的
              int erase_value;
              if ((write_count % 4) < 1) {
                // erase存在的值
                erase_value = ops.erase_exist_values[erase_exist_idx++];
              } else {
                // erase不存在的值
                erase_value = ops.erase_not_exist_values[erase_not_exist_idx++];
              }
              // auto start = std::chrono::high_resolution_clock::now();
              accessor->erase(erase_value);
              // auto end = std::chrono::high_resolution_clock::now();
              // long long latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
              // total_erase_ops.fetch_add(1);
              // total_erase_latency_ns.fetch_add(latency);
            } else {
              // insert操作: 1/2 存在的, 1/2 不存在的
              int insert_value;
              if ((write_count % 4) < 3) {
                // insert存在的值
                insert_value = ops.insert_exist_values[insert_exist_idx++];
              } else {
                // insert不存在的值
                insert_value = ops.insert_not_exist_values[insert_not_exist_idx++];
              }
              // auto start = std::chrono::high_resolution_clock::now();
              accessor->insert(insert_value);
              // auto end = std::chrono::high_resolution_clock::now();
              // long long latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
              // total_insert_ops.fetch_add(1);
              // total_insert_latency_ns.fetch_add(latency);
            }
            write_count++;
          } else {
            // 读操作
            int read_value = ops.read_values[read_idx++];
            // auto start = std::chrono::high_resolution_clock::now();
            accessor->find(read_value);
            // auto end = std::chrono::high_resolution_clock::now();
            // long long latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            // total_read_ops.fetch_add(1);
            // total_read_latency_ns.fetch_add(latency);
          }
        }
      };

      // 启动所有线程
      std::vector<std::thread> threads;

      for (int i = 0; i < TOTAL_THREAD_COUNT; i++) {
        threads.emplace_back(worker_ope, i);
      }

      auto t1 = std::chrono::high_resolution_clock::now();

      start_flag.store(true);

      // 等待所有线程完成
      for (auto& t : threads) {
        t.join();
      }

      auto t2 = std::chrono::high_resolution_clock::now();
      long long total_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

      stop_flag.store(true);

      // 计算性能指标
      long long read_ops = total_read_needed;
      long long insert_ops = total_insert_exist_needed + total_insert_not_exist_needed;
      long long erase_ops = total_erase_exist_needed + total_erase_not_exist_needed;
      // long long read_latency_ns = total_read_latency_ns.load();
      // long long insert_latency_ns = total_insert_latency_ns.load();
      // long long erase_latency_ns = total_erase_latency_ns.load();
      long long read_latency_ns = 0;
      long long insert_latency_ns = 0;
      long long erase_latency_ns = 0;

      // 吞吐量计算：基于操作的总耗时
      double read_throughput = (total_time_ns > 0) ?
        (read_ops * 1000000000.0) / total_time_ns : 0.0;
      double insert_throughput = (total_time_ns > 0) ?
        (insert_ops * 1000000000.0) / total_time_ns : 0.0;
      double erase_throughput = (total_time_ns > 0) ?
        (erase_ops * 1000000000.0) / total_time_ns : 0.0;
      double avg_read_latency = (read_ops > 0) ? (read_latency_ns * 1.0) / read_ops : 0.0;
      double avg_insert_latency = (insert_ops > 0) ? (insert_latency_ns * 1.0) / insert_ops : 0.0;
      double avg_erase_latency = (erase_ops > 0) ? (erase_latency_ns * 1.0) / erase_ops : 0.0;

      RunResult result;
      result.read_throughput = read_throughput;
      result.insert_throughput = insert_throughput;
      result.erase_throughput = erase_throughput;
      result.read_latency = avg_read_latency;
      result.insert_latency = avg_insert_latency;
      result.erase_latency = avg_erase_latency;

      results.push_back(result);
    }

    // 去掉最大最小值后取平均
    auto compute_avg = [&results](auto RunResult::* field) -> double {
      std::vector<double> values;
      for (const auto& r : results) {
        values.push_back(r.*field);
      }
      std::sort(values.begin(), values.end());
      // 去掉最小和最大值
      double sum = 0;
      for (size_t i = 1; i < values.size() - 1; i++) {
        sum += values[i];
      }
      return sum / (values.size() - 2);
    };

    double avg_read_tp = compute_avg(&RunResult::read_throughput);
    double avg_insert_tp = compute_avg(&RunResult::insert_throughput);
    double avg_erase_tp = compute_avg(&RunResult::erase_throughput);
    double avg_read_lat = compute_avg(&RunResult::read_latency);
    double avg_insert_lat = compute_avg(&RunResult::insert_latency);
    double avg_erase_lat = compute_avg(&RunResult::erase_latency);

    // 输出结果
    std::cout << std::fixed << std::setprecision(6)
              << std::setw(10) << (1.0 / write_frequency) << " | "
              << std::setprecision(0) << std::setw(15) << avg_read_tp << " | "
              << std::setw(16) << avg_insert_tp << " | "
              << std::setw(16) << avg_erase_tp << " | "
              << std::setprecision(2) << std::setw(11) << avg_read_lat << " | "
              << std::setw(14) << avg_insert_lat << " | "
              << std::setw(12) << avg_erase_lat << "\n";
  }
}

#endif // TEST_PERF_H
