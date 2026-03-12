/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include "monitor/cpu_load_monitor.h"

#include <cstdio>

namespace monitor {

namespace {

bool ReadLoadFromProc(float* load1, float* load3, float* load15) {
  FILE* fp = fopen("/proc/loadavg", "r");
  if (!fp) {
    return false;
  }

  int ret = fscanf(fp, "%f %f %f", load1, load3, load15);
  fclose(fp);
  return ret == 3;
}

}  // namespace

void CpuLoadMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return;
  }

  float load1 = 0;
  float load3 = 0;
  float load15 = 0;
  if (!ReadLoadFromProc(&load1, &load3, &load15)) {
    return;
  }

  auto* cpu_load_msg = monitor_info->mutable_cpu_load();
  cpu_load_msg->set_load_avg_1(load1);
  cpu_load_msg->set_load_avg_3(load3);
  cpu_load_msg->set_load_avg_15(load15);
}

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
