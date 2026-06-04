/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：CPU 统计监控器。
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
class CpuStatMonitor : public MonitorInter {
  struct CpuStat {
    std::string cpu_name;
    uint64_t user = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t nice = 0;
    uint64_t io_wait = 0;
    uint64_t irq = 0;
    uint64_t soft_irq = 0;
    uint64_t steal = 0;
    uint64_t guest = 0;
    uint64_t guest_nice = 0;
 };

 public:
  // 初始化 CPU 状态监控器。
  CpuStatMonitor() {}

  // 采集一轮 CPU 状态并写入统一监控消息。
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;

  // 停止监控器并释放相关资源。
  void Stop() override {}

 private:
  std::unordered_map<std::string, struct CpuStat> cpu_stat_map_;
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
