/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#pragma once

#include <string>

#include "monitor_info.pb.h"

namespace monitor {
class MonitorInter {
 public:
  MonitorInter() {}
  virtual ~MonitorInter() {}
  virtual void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) = 0;
  virtual void Stop() = 0;
};
}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
