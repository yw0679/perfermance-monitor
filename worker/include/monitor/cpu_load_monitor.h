/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#pragma once

#include <string>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
class CpuLoadMonitor : public MonitorInter {
 public:
  CpuLoadMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  float load_avg_1_;
  float load_avg_3_;
  float load_avg_15_;
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
