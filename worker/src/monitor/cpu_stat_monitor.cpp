/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include "monitor/cpu_stat_monitor.h"

#include <fstream>
#include <sstream>
#include <string>

namespace monitor {

namespace {

struct ProcCpuStat {
  std::string cpu_name;
  uint64_t user = 0;
  uint64_t nice = 0;
  uint64_t system = 0;
  uint64_t idle = 0;
  uint64_t io_wait = 0;
  uint64_t irq = 0;
  uint64_t soft_irq = 0;
  uint64_t steal = 0;
  uint64_t guest = 0;
  uint64_t guest_nice = 0;
};

bool ParseCpuStatLine(const std::string& line, ProcCpuStat* stat) {
  if (!stat) {
    return false;
  }

  std::istringstream stream(line);
  if (!(stream >> stat->cpu_name)) {
    return false;
  }
  if (stat->cpu_name.rfind("cpu", 0) != 0) {
    return false;
  }
  if (!(stream >> stat->user >> stat->nice >> stat->system >> stat->idle >>
        stat->io_wait >> stat->irq >> stat->soft_irq >> stat->steal)) {
    return false;
  }
  if (!(stream >> stat->guest)) {
    stat->guest = 0;
    stream.clear();
  }
  if (!(stream >> stat->guest_nice)) {
    stat->guest_nice = 0;
  }
  return true;
}

uint64_t TotalTime(const ProcCpuStat& stat) {
  return stat.user + stat.nice + stat.system + stat.idle + stat.io_wait +
         stat.irq + stat.soft_irq + stat.steal;
}

uint64_t BusyTime(const ProcCpuStat& stat) {
  return stat.user + stat.nice + stat.system + stat.irq + stat.soft_irq +
         stat.steal;
}

float SafePercent(uint64_t current_value, uint64_t previous_value,
                  uint64_t total_delta) {
  if (total_delta == 0 || current_value < previous_value) {
    return 0.0f;
  }
  return static_cast<float>(
      static_cast<double>(current_value - previous_value) * 100.0 /
      static_cast<double>(total_delta));
}

}  // namespace

void CpuStatMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return;
  }

  std::ifstream stat_file("/proc/stat");
  if (!stat_file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(stat_file, line)) {
    ProcCpuStat current;
    if (!ParseCpuStatLine(line, &current)) {
      break;
    }

    auto* cpu_stat_msg = monitor_info->add_cpu_stat();
    cpu_stat_msg->set_cpu_name(current.cpu_name);

    auto cached_it = cpu_stat_map_.find(current.cpu_name);//返回当前cpu_name的迭代器
    if (cached_it != cpu_stat_map_.end()) {
      const CpuStat& previous = cached_it->second;
      ProcCpuStat previous_stat;
      previous_stat.cpu_name = previous.cpu_name;
      previous_stat.user = previous.user;
      previous_stat.nice = previous.nice;
      previous_stat.system = previous.system;
      previous_stat.idle = previous.idle;
      previous_stat.io_wait = previous.io_wait;
      previous_stat.irq = previous.irq;
      previous_stat.soft_irq = previous.soft_irq;
      previous_stat.steal = previous.steal;
      previous_stat.guest = previous.guest;
      previous_stat.guest_nice = previous.guest_nice;

      uint64_t current_total = TotalTime(current);
      uint64_t previous_total = TotalTime(previous_stat);
      uint64_t total_delta =
          current_total > previous_total ? current_total - previous_total : 0;

      uint64_t current_busy = BusyTime(current);
      uint64_t previous_busy = BusyTime(previous_stat);
      float cpu_percent = 0.0f;
      if (total_delta > 0 && current_busy >= previous_busy) {
        cpu_percent = static_cast<float>(
            static_cast<double>(current_busy - previous_busy) * 100.0 /
            static_cast<double>(total_delta));
      }

      cpu_stat_msg->set_cpu_percent(cpu_percent);
      cpu_stat_msg->set_usr_percent(
          SafePercent(current.user, previous.user, total_delta));
      cpu_stat_msg->set_system_percent(
          SafePercent(current.system, previous.system, total_delta));
      cpu_stat_msg->set_nice_percent(
          SafePercent(current.nice, previous.nice, total_delta));
      cpu_stat_msg->set_idle_percent(
          SafePercent(current.idle, previous.idle, total_delta));
      cpu_stat_msg->set_io_wait_percent(
          SafePercent(current.io_wait, previous.io_wait, total_delta));
      cpu_stat_msg->set_irq_percent(
          SafePercent(current.irq, previous.irq, total_delta));
      cpu_stat_msg->set_soft_irq_percent(
          SafePercent(current.soft_irq, previous.soft_irq, total_delta));
    }

//[]:从cpu_stat_map_表中取出键为current.cpu_name的值，并返回引用，也就是可以直接修改
    CpuStat& cached = cpu_stat_map_[current.cpu_name];
    cached.cpu_name = current.cpu_name;
    cached.user = current.user;
    cached.nice = current.nice;
    cached.system = current.system;
    cached.idle = current.idle;
    cached.io_wait = current.io_wait;
    cached.irq = current.irq;
    cached.soft_irq = current.soft_irq;
    cached.steal = current.steal;
    cached.guest = current.guest;
    cached.guest_nice = current.guest_nice;
  }
}

}  // namespace monitor
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
