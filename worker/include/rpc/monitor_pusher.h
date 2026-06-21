/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <iostream>
#include <chrono>

#include "monitor/metric_collector.h"
#include "utils/ring_buffer.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * 监控数据推送器
 * 
 * 每隔指定间隔（默认 10 秒）采集本机监控数据，
 * 并通过 gRPC 推送给管理者服务器。
 */
class MonitorPusher {
 public:
  /**
   * 构造函数
   * @param manager_address 管理者服务器地址（如 "192.168.1.100:50051"）
   * @param interval_seconds 推送间隔（秒），默认 10 秒
   */
  explicit MonitorPusher(const std::string& manager_address,
                         int interval_seconds = 10);
  ~MonitorPusher();

  // 启动一个线程执行PushLoop。
  void Start();

  // 停止推送
  void Stop();

  // 获取管理者地址
  const std::string& GetManagerAddress() const { return manager_address_; }

 private:
  void PushLoop();
  bool PushOnce();

  // 执行一次 gRPC SetMonitorInfo 调用，封装 deadline + 日志
  bool PushData(const monitor::proto::MonitorInfo& info);
  // 尝试补推环形缓冲中的积压数据
  void FlushBacklog();

  std::string manager_address_;
  int interval_seconds_;
  std::atomic<bool> running_; //保证原子性的bool
  std::unique_ptr<std::thread> thread_;
  std::unique_ptr<MetricCollector> collector_;
  std::unique_ptr<monitor::proto::GrpcManager::Stub> stub_;

  // 环形缓冲：manager 不可达时暂存 MonitorInfo，恢复后补推
  // 容量 120 = 2 分钟（每秒 1 条），旧数据被自然覆盖
  monitor::RingBuffer<monitor::proto::MonitorInfo> backlog_{120};
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
