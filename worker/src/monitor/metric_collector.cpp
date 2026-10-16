/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include "monitor/metric_collector.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>

#include "monitor/cpu_load_monitor.h"
#include "monitor/cpu_stat_monitor.h"
#include "monitor/disk_monitor.h"
#include "monitor/host_info_monitor.h"
#include "monitor/mem_monitor.h"
#include "monitor/net_ebpf_monitor.h"

namespace monitor {

MetricCollector::MetricCollector() {
  // 获取主机名
  char hostname[256];
  //gethostname：linux中获取本机名字的函数
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    hostname_ = hostname;
  } else {
    hostname_ = "unknown";
  }

  // sequence 用 epoch 毫秒播种，而不是从 0 开始自增。
  //
  // 原因：manager / MySQL 侧是把 (server_name, sequence) 当作幂等键来去重的
  // （INSERT IGNORE + 唯一键）。如果每个 worker 进程都从 1 开始编号，那么 worker
  // 重启之后新进程的 sequence 会撞上上一轮已经写过的号，导致重启后的一段时间内
  // 采样被静默丢弃 —— 直到新 sequence 超过上一轮的最大值为止。
  //
  // 用 epoch 毫秒播种后 sequence 跨重启依然单调递增，撞不上历史数据；
  // 同一进程内继续用 ++ 保证严格递增（采集间隔 1s，远大于 1ms，不会自增追上时钟）。
  //
  // 已知边界：如果系统时钟被向后调整（例如 NTP step），新进程的播种值可能低于
  // 上一轮的最大值，此时仍会出现一小段丢弃窗口。要彻底消除得让 manager 记录
  // worker 的启动标识并把它纳入幂等键，本项目的目标是低成本规避这一常见场景。
  collect_seq_ = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  // 初始化所有监控器
  monitors_.push_back(std::make_unique<CpuLoadMonitor>());
  monitors_.push_back(std::make_unique<CpuStatMonitor>());
  monitors_.push_back(std::make_unique<MemMonitor>());
  monitors_.push_back(std::make_unique<NetEbpfMonitor>());
  monitors_.push_back(std::make_unique<DiskMonitor>());
  monitors_.push_back(std::make_unique<HostInfoMonitor>());
}

MetricCollector::~MetricCollector() {
  for (auto& monitor : monitors_) {
    monitor->Stop();
  }
}

void MetricCollector::CollectAll(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return;
  }

  monitor_info->set_name(hostname_);
  monitor_info->set_sequence(++collect_seq_);

  for (auto& monitor : monitors_) {
    monitor->UpdateOnce(monitor_info);
  }
}

}  // namespace monitor
