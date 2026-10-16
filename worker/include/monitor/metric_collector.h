/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：采集所有指标并填充到 MonitorInfo
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
  // 采样序号，作为 manager/MySQL 侧的幂等键。
  // 构造时用 epoch 毫秒播种，保证 worker 重启后依然单调、不会撞上历史数据。
  uint64_t collect_seq_ = 0;
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
