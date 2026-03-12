/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

class MetricCollector {
 public:
  MetricCollector();
  ~MetricCollector();

  // 采集所有指标并填充到 MonitorInfo
  void CollectAll(monitor::proto::MonitorInfo* monitor_info);

 private:
  std::vector<std::unique_ptr<MonitorInter>> monitors_;
  std::string hostname_;
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
