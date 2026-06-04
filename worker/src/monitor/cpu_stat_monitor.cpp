#include "monitor/cpu_stat_monitor.h"
#include "shared/cpu_stat_shared.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string_view>
#include <string>
#include <vector>

namespace monitor {

namespace {

constexpr char kCpuStatMonitorDevicePath[] = "/dev/cpu_stat_monitor";
constexpr int kSharedRegionReadRetryTimes = 5;
constexpr useconds_t kSharedRegionRetrySleepUs = 1000;

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

// 返回共享内存区域按系统页大小对齐后的映射长度。
size_t GetCpuStatMonitorMapSize() {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return sizeof(cpu_stat_shared_region);
  }

  const size_t raw_size = sizeof(cpu_stat_shared_region);
  const size_t aligned_size =
      ((raw_size + static_cast<size_t>(page_size) - 1) /
       static_cast<size_t>(page_size)) *
      static_cast<size_t>(page_size);
  return aligned_size;
}

// 将共享内存中的单个 CPU 原始数据转换为用户态统一快照结构。
ProcCpuStat BuildSnapshotFromSharedRaw(const cpu_stat_shared_raw& raw,
                                       std::string_view cpu_name) {
  ProcCpuStat stat;
  stat.cpu_name = std::string(cpu_name);
  stat.user = raw.user;
  stat.nice = raw.nice;
  stat.system = raw.system;
  stat.idle = raw.idle;
  stat.io_wait = raw.iowait;
  stat.irq = raw.irq;
  stat.soft_irq = raw.softirq;
  stat.steal = raw.steal;
  stat.guest = raw.guest;
  stat.guest_nice = raw.guest_nice;
  return stat;
}

// 将一个 CPU 的累计值累加到总 CPU 快照中。
void AccumulateCpuStat(const ProcCpuStat& current, ProcCpuStat* total) {
  if (!total) {
    return;
  }

  total->user += current.user;
  total->nice += current.nice;
  total->system += current.system;
  total->idle += current.idle;
  total->io_wait += current.io_wait;
  total->irq += current.irq;
  total->soft_irq += current.soft_irq;
  total->steal += current.steal;
  total->guest += current.guest;
  total->guest_nice += current.guest_nice;
}

// 计算一个 CPU 快照中的总运行时间。
uint64_t TotalTime(const ProcCpuStat& stat) {
  return stat.user + stat.nice + stat.system + stat.idle + stat.io_wait +
         stat.irq + stat.soft_irq + stat.steal;
}

// 计算一个 CPU 快照中的忙碌时间。
uint64_t BusyTime(const ProcCpuStat& stat) {
  return stat.user + stat.nice + stat.system + stat.irq + stat.soft_irq +
         stat.steal;
}

// 按总时间差计算某一项状态的占比，避免除零和回退值异常。
float SafePercent(uint64_t current_value, uint64_t previous_value,
                  uint64_t total_delta) {
  if (total_delta == 0 || current_value < previous_value) {
    return 0.0f;
  }
  return static_cast<float>(
      static_cast<double>(current_value - previous_value) * 100.0 /
      static_cast<double>(total_delta));
}

// 尝试打开内核模块导出的 CPU 状态设备。
bool OpenCpuStatMonitorDevice(int* device_fd) {
  if (!device_fd) {
    return false;
  }

  *device_fd = open(kCpuStatMonitorDevicePath, O_RDONLY);
  return *device_fd >= 0;
}

// 尝试将 CPU 状态设备映射到当前进程地址空间。
bool MapCpuStatMonitorDevice(int device_fd, void** mapped_region) {
  if (!mapped_region || device_fd < 0) {
    return false;
  }

  void* region =
      mmap(nullptr, GetCpuStatMonitorMapSize(), PROT_READ, MAP_SHARED, device_fd,
           0);
  if (region == MAP_FAILED) {
    *mapped_region = nullptr;
    return false;
  }

  *mapped_region = region;
  return true;
}

// 校验共享内存区域头部，确保版本和 CPU 数量在预期范围内。
bool IsSharedRegionValid(const cpu_stat_shared_region* region) {
  if (!region) {
    return false;
  }
  if (region->version != CPU_STAT_SHARED_VERSION) {
    return false;
  }
  if (region->cpu_count == 0 || region->cpu_count > CPU_STAT_MAX_CPUS) {
    return false;
  }
  return true;
}

// 将本地稳定快照转换为用户态快照列表，并补一个总 CPU 行。
bool BuildCpuStatsFromSnapshot(const cpu_stat_shared_region& snapshot,
                               std::vector<ProcCpuStat>* stats) {
  if (!IsSharedRegionValid(&snapshot) || !stats) {
    return false;
  }

  stats->clear();
  stats->reserve(snapshot.cpu_count + 1);

  ProcCpuStat total_stat;
  total_stat.cpu_name = "cpu";

  for (uint32_t cpu = 0; cpu < snapshot.cpu_count; ++cpu) {
    ProcCpuStat current =
        BuildSnapshotFromSharedRaw(snapshot.cpus[cpu], "cpu" + std::to_string(cpu));
    AccumulateCpuStat(current, &total_stat);
    stats->push_back(current);
  }

  stats->insert(stats->begin(), total_stat);
  return true;
}

// 尝试读取一份 seq 稳定的共享内存快照，避免读到内核更新中的半成品数据。
bool CopyMappedCpuStats(const cpu_stat_shared_region* region,
                        std::vector<ProcCpuStat>* stats) {
  if (!region || !stats) {
    return false;
  }

  for (int attempt = 0; attempt < kSharedRegionReadRetryTimes; ++attempt) {
    const uint32_t seq_before = region->seq;
    if ((seq_before & 1U) != 0U) {
      usleep(kSharedRegionRetrySleepUs);
      continue;
    }

    cpu_stat_shared_region snapshot;
    std::memcpy(&snapshot, region, sizeof(snapshot));

    const uint32_t seq_after = region->seq;
    if (seq_before != seq_after || (seq_after & 1U) != 0U) {
      usleep(kSharedRegionRetrySleepUs);
      continue;
    }

    if (snapshot.seq != seq_before) {
      usleep(kSharedRegionRetrySleepUs);
      continue;
    }

    return BuildCpuStatsFromSnapshot(snapshot, stats);
  }

  return false;
}

// 从内核模块共享内存中读取 CPU 原始累计值快照。
bool ReadMappedCpuStats(std::vector<ProcCpuStat>* stats) {
  if (!stats) {
    return false;
  }

  int device_fd = -1;
  if (!OpenCpuStatMonitorDevice(&device_fd)) {
    return false;
  }

  void* mapped_region = nullptr;
  if (!MapCpuStatMonitorDevice(device_fd, &mapped_region)) {
    close(device_fd);
    return false;
  }

  const auto* shared_region =
      static_cast<const cpu_stat_shared_region*>(mapped_region);
  const bool copied = CopyMappedCpuStats(shared_region, stats);

  munmap(mapped_region, GetCpuStatMonitorMapSize());
  close(device_fd);
  return copied;
}

// 根据当前快照和上一轮快照，计算 CPU 及各细分状态的百分比。
void FillCpuPercentages(const ProcCpuStat& current, const ProcCpuStat& previous,
                        monitor::proto::CpuStat* cpu_stat_msg) {
  if (!cpu_stat_msg) {
    return;
  }

  uint64_t current_total = TotalTime(current);
  uint64_t previous_total = TotalTime(previous);
  uint64_t total_delta =
      current_total > previous_total ? current_total - previous_total : 0;

  uint64_t current_busy = BusyTime(current);
  uint64_t previous_busy = BusyTime(previous);
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

}  // namespace

// 采集一轮 CPU 状态，并将结果填充到 MonitorInfo 中。
void CpuStatMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return;
  }

  std::vector<ProcCpuStat> current_stats;
  if (!ReadMappedCpuStats(&current_stats)) {
    return;
  }

  for (const ProcCpuStat& current : current_stats) {
    auto* cpu_stat_msg = monitor_info->add_cpu_stat();
    cpu_stat_msg->set_cpu_name(current.cpu_name);

    auto cached_it = cpu_stat_map_.find(current.cpu_name);
    if (cached_it != cpu_stat_map_.end()) {
      ProcCpuStat previous;
      previous.cpu_name = cached_it->second.cpu_name;
      previous.user = cached_it->second.user;
      previous.nice = cached_it->second.nice;
      previous.system = cached_it->second.system;
      previous.idle = cached_it->second.idle;
      previous.io_wait = cached_it->second.io_wait;
      previous.irq = cached_it->second.irq;
      previous.soft_irq = cached_it->second.soft_irq;
      previous.steal = cached_it->second.steal;
      previous.guest = cached_it->second.guest;
      previous.guest_nice = cached_it->second.guest_nice;
      FillCpuPercentages(current, previous, cpu_stat_msg);
    }

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
