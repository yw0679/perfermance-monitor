/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：读取每个磁盘的累计统计值，然后和上一次采样做差，计算读写吞吐、IOPS、平均延迟和磁盘利用率。
 */

#pragma once

#include <map>
#include <string>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

class DiskMonitor : public MonitorInter {
 public:
  DiskMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  struct DiskSample {
    uint64_t reads;
    uint64_t writes;
    uint64_t sectors_read;
    uint64_t sectors_written;
    uint64_t read_time_ms;
    uint64_t write_time_ms;
    uint64_t io_in_progress;
    uint64_t io_time_ms;
    uint64_t weighted_io_time_ms;
  };

  std::map<std::string, DiskSample> last_samples_;
  std::map<std::string, double> last_time_;
};

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
