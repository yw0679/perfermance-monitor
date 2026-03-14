/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include "monitor/metric_collector.h"

#include <unistd.h>

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
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    hostname_ = hostname;
  } else {
    hostname_ = "unknown";
  }

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

  // 设置主机名
  monitor_info->set_name(hostname_);

  // 调用每个监控器的 UpdateOnce 方法
  for (auto& monitor : monitors_) {
    monitor->UpdateOnce(monitor_info);
  }
}

}  // namespace monitor
