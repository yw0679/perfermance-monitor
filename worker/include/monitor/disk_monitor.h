/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#pragma once

#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

class DiskMonitor : public MonitorInter {
 public:
  DiskMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
