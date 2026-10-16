/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */

#include "monitor/net_ebpf_monitor.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <net/if.h>
#include <string>
#include <sys/stat.h>

#include "monitor_info.pb.h"
#include "monitor/net_stats.skel.h"

extern "C" {
struct net_stats {
  uint64_t rcv_bytes;
  uint64_t rcv_packets;
  uint64_t snd_bytes;
  uint64_t snd_packets;
};

// 与 net_stats.bpf.c 里的 struct proc_net_stats 布局保持一致。
// PERCPU map 的每个元素按 CPU 连续排布，所以这里也必须是纯 POD。
struct proc_net_stats {
  uint64_t snd_bytes;
  uint64_t rcv_bytes;
  char comm[16];
};
}

namespace monitor {

namespace {

constexpr __u32 kTcHandle = 1;
constexpr __u32 kTcPriority = 1;
// 每次上报只带流量最大的前 N 个进程，避免上报报文和落库行数随进程数增长
constexpr size_t kTopProcessCount = 10;

// 两张 PERCPU map 的 value 大小（当前都是 32 字节），取最大值给共用读缓冲定尺寸。
// 内核给每个 key 分配 nr_cpus 份副本，用户态一次 lookup 会把它们全部拷出来。
constexpr size_t kPerCpuValueSize =
    std::max(sizeof(struct net_stats), sizeof(struct proc_net_stats));

bool IsPhysicalInterface(const std::string& ifname) {
  if (ifname == "lo") {
    return false;
  }

  // 物理网卡（以及带真实底层设备的 virtio/pci 网卡）在 sysfs 下会有 device 链接。
  const std::string device_path = "/sys/class/net/" + ifname + "/device";
  struct stat st = {};
  return lstat(device_path.c_str(), &st) == 0;
}

std::vector<uint32_t> GetAllIfIndexes() {
  std::vector<uint32_t> indexes;
  DIR* dir = opendir("/sys/class/net");
  if (!dir) {
    return indexes;
  }

  dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (!IsPhysicalInterface(entry->d_name)) {
      continue;
    }

    unsigned int ifindex = if_nametoindex(entry->d_name);
    if (ifindex > 0) {
      indexes.push_back(ifindex);
    }
  }

  closedir(dir);
  return indexes;
}

void DestroyClsactQdisc(int ifindex) {
  bpf_tc_hook hook = {};
  hook.sz = sizeof(hook);
  hook.ifindex = ifindex;
  hook.attach_point =
      static_cast<bpf_tc_attach_point>(BPF_TC_INGRESS | BPF_TC_EGRESS);

  const int err = bpf_tc_hook_destroy(&hook);
  if (err != 0 && err != -ENOENT && err != -EINVAL) {
    char ifname[IF_NAMESIZE] = {};
    if (if_indextoname(ifindex, ifname) == nullptr) {
      std::strncpy(ifname, "unknown", sizeof(ifname) - 1);
    }
    std::cerr << "Destroy clsact qdisc failed on " << ifname
              << ": " << std::strerror(-err) << std::endl;
  }
}

bpf_tc_hook MakeHook(int ifindex, bpf_tc_attach_point attach_point) {
  bpf_tc_hook hook = {};
  hook.sz = sizeof(hook);
  hook.ifindex = ifindex;
  hook.attach_point = attach_point;
  return hook;
}

bpf_tc_opts MakeAttachOpts(int prog_fd) {
  bpf_tc_opts opts = {};
  opts.sz = sizeof(opts);
  opts.prog_fd = prog_fd;
  opts.handle = kTcHandle;
  opts.priority = kTcPriority;
  return opts;
}

bpf_tc_opts MakeDetachOpts() {
  bpf_tc_opts opts = {};
  opts.sz = sizeof(opts);
  opts.handle = kTcHandle;
  opts.priority = kTcPriority;
  return opts;
}

//把prog_fd这个ebpf程序fd挂到ifindex网卡的attach_point上
bool AttachTcProgram(int ifindex, bpf_tc_attach_point attach_point, int prog_fd,
                     const char* direction) {
  bpf_tc_hook hook = MakeHook(ifindex, attach_point);
  int err = bpf_tc_hook_create(&hook);
  if (err != 0 && err != -EEXIST) {
    char ifname[IF_NAMESIZE] = {};
    if (if_indextoname(ifindex, ifname) == nullptr) {
      std::strncpy(ifname, "unknown", sizeof(ifname) - 1);
    }
    std::cerr << "Create TC hook failed on " << ifname
              << ": " << std::strerror(-err) << std::endl;
    return false;
  }

  //清空钩子处可能遗留的ebpf程序
  bpf_tc_opts detach_opts = MakeDetachOpts();
  bpf_tc_detach(&hook, &detach_opts);

  bpf_tc_opts attach_opts = MakeAttachOpts(prog_fd);
  err = bpf_tc_attach(&hook, &attach_opts);
  if (err != 0) {
    char ifname[IF_NAMESIZE] = {};
    if (if_indextoname(ifindex, ifname) == nullptr) {
      std::strncpy(ifname, "unknown", sizeof(ifname) - 1);
    }
    std::cerr << "Attach TC " << direction << " failed on " << ifname
              << ": " << std::strerror(-err) << std::endl;
    return false;
  }

  return true;
}

void DetachTcProgram(int ifindex, bpf_tc_attach_point attach_point) {
  bpf_tc_hook hook = MakeHook(ifindex, attach_point);
  bpf_tc_opts opts = MakeDetachOpts();
  bpf_tc_detach(&hook, &opts);
}

}  // namespace

NetEbpfMonitor::NetEbpfMonitor() {
  loaded_ = InitEbpf();
}

NetEbpfMonitor::~NetEbpfMonitor() {
  CleanupEbpf();
}

bool NetEbpfMonitor::InitEbpf() {
  //打开自己写好的bpf的skeleton，在内存里准备好
  skel_ = net_stats_bpf__open();
  if (!skel_) {
    std::cerr << "NetEbpfMonitor: failed to open BPF skeleton" << std::endl;
    return false;
  }
  //把bpf程序加载进内核
  const int err = net_stats_bpf__load(skel_);
  if (err != 0) {
    std::cerr << "NetEbpfMonitor: failed to load BPF program: "
              << std::strerror(-err) << std::endl;
    CleanupEbpf();
    return false;
  }
  //取出map_fd，用户态靠这个fd读数据
  map_fd_ = bpf_map__fd(skel_->maps.net_stats_map);
  if (map_fd_ < 0) {
    std::cerr << "NetEbpfMonitor: failed to get map fd" << std::endl;
    CleanupEbpf();
    return false;
  }

  proc_map_fd_ = bpf_map__fd(skel_->maps.proc_net_map);
  if (proc_map_fd_ < 0) {
    std::cerr << "NetEbpfMonitor: failed to get process map fd" << std::endl;
    CleanupEbpf();
    return false;
  }

  // PERCPU map 一次 lookup 会把所有 CPU 的副本都拷出来，
  // 这里按 possible CPU 数把读取缓冲准备好，之后每次采样复用同一块内存。
  num_possible_cpus_ = libbpf_num_possible_cpus();
  if (num_possible_cpus_ <= 0) {
    std::cerr << "NetEbpfMonitor: failed to get possible cpu count" << std::endl;
    CleanupEbpf();
    return false;
  }
  percpu_buf_.resize(static_cast<size_t>(num_possible_cpus_) * kPerCpuValueSize);

  const int ingress_fd = bpf_program__fd(skel_->progs.tc_ingress);
  const int egress_fd = bpf_program__fd(skel_->progs.tc_egress);

  // 当前实现会复用同一个已加载的 ingress/egress 程序挂到多块物理网卡上。
  // 每块网卡的数据通过 skb->ifindex 作为 map key 进行区分。
  for (uint32_t ifindex : GetAllIfIndexes()) {
    const bool ingress_ok =
        AttachTcProgram(static_cast<int>(ifindex), BPF_TC_INGRESS, ingress_fd,
                        "ingress");
    const bool egress_ok =
        AttachTcProgram(static_cast<int>(ifindex), BPF_TC_EGRESS, egress_fd,
                        "egress");
    if (!ingress_ok || !egress_ok) {
      DetachTcProgram(static_cast<int>(ifindex), BPF_TC_INGRESS);
      DetachTcProgram(static_cast<int>(ifindex), BPF_TC_EGRESS);
      DestroyClsactQdisc(static_cast<int>(ifindex));
      continue;
    }

    attached_ifindexes_.push_back(ifindex);
  }

  if (attached_ifindexes_.empty()) {
    std::cerr << "NetEbpfMonitor: no usable interface found for TC hook"
              << std::endl;
    CleanupEbpf();
    return false;
  }

  std::cout << "NetEbpfMonitor: attached eBPF TC hooks to "
            << attached_ifindexes_.size() << " interfaces" << std::endl;

  // 进程级归因是增量能力，挂载失败时降级为"只有网卡维度统计"，不影响 worker 启动。
  if (!AttachProcessProbes()) {
    std::cerr << "NetEbpfMonitor: process-level attribution disabled, "
              << "falling back to interface-level stats only" << std::endl;
  }

  return true;
}

bool NetEbpfMonitor::AttachProcessProbes() {
  // SEC("kprobe/tcp_sendmsg") / SEC("kretprobe/tcp_recvmsg")，
  // bpf_program__attach 会按 section 名自动选择 kprobe 还是 kretprobe。
  send_probe_link_ = bpf_program__attach(skel_->progs.trace_tcp_sendmsg);
  long err = libbpf_get_error(send_probe_link_);
  if (err != 0) {
    std::cerr << "NetEbpfMonitor: failed to attach tcp_sendmsg kprobe: "
              << std::strerror(static_cast<int>(-err)) << std::endl;
    send_probe_link_ = nullptr;
    return false;
  }

  recv_probe_link_ = bpf_program__attach(skel_->progs.trace_tcp_recvmsg);
  err = libbpf_get_error(recv_probe_link_);
  if (err != 0) {
    std::cerr << "NetEbpfMonitor: failed to attach tcp_recvmsg kretprobe: "
              << std::strerror(static_cast<int>(-err)) << std::endl;
    bpf_link__destroy(send_probe_link_);
    send_probe_link_ = nullptr;
    recv_probe_link_ = nullptr;
    return false;
  }

  std::cout << "NetEbpfMonitor: attached process-level probes on "
            << "kprobe/tcp_sendmsg + kretprobe/tcp_recvmsg" << std::endl;
  return true;
}

void NetEbpfMonitor::CleanupEbpf() {
  // kprobe 的 link 需要显式销毁，否则进程退出前 probe 一直挂在被监控主机上
  if (send_probe_link_) {
    bpf_link__destroy(send_probe_link_);
    send_probe_link_ = nullptr;
  }
  if (recv_probe_link_) {
    bpf_link__destroy(recv_probe_link_);
    recv_probe_link_ = nullptr;
  }

  for (uint32_t ifindex : attached_ifindexes_) {
    DetachTcProgram(static_cast<int>(ifindex), BPF_TC_INGRESS);
    DetachTcProgram(static_cast<int>(ifindex), BPF_TC_EGRESS);
    DestroyClsactQdisc(static_cast<int>(ifindex));
  }
  attached_ifindexes_.clear();

  if (skel_) {
    net_stats_bpf__destroy(skel_);
    skel_ = nullptr;
  }

  map_fd_ = -1;
  proc_map_fd_ = -1;
  num_possible_cpus_ = 0;
  percpu_buf_.clear();
  loaded_ = false;
  cache_.clear();
  ifname_cache_.clear();
  proc_cache_.clear();
}

// 读取 PERCPU map 的一个 key。
//
// 内核按 round_up(value_size, 8) 的步长把各 CPU 的副本连续排布，而这两个 value
// 结构体里都含 u64，sizeof 必然是 8 的倍数，所以 sizeof 就是正确的步长。
// 这一步只负责把原始副本取出来，具体的字段归并由调用方处理，因为网卡和进程
// 两张 map 的字段语义不同（进程那张还要顺带取 comm）。
const char* NetEbpfMonitor::LookupPerCpuValue(int fd, const void* key) {
  if (fd < 0 || percpu_buf_.empty()) {
    return nullptr;
  }

  if (bpf_map_lookup_elem(fd, key, percpu_buf_.data()) != 0) {
    return nullptr;
  }

  return percpu_buf_.data();
}

std::string NetEbpfMonitor::GetIfName(uint32_t ifindex) {
  auto it = ifname_cache_.find(ifindex);
  if (it != ifname_cache_.end()) {
    return it->second;
  }

  char ifname[IF_NAMESIZE] = {};
  if (if_indextoname(ifindex, ifname) == nullptr) {
    return "";
  }

  std::string name(ifname);
  ifname_cache_[ifindex] = name;
  return name;
}

void NetEbpfMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info || !loaded_ || map_fd_ < 0) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  uint32_t current_key = 0;
  uint32_t next_key = 0;
  bool has_current_key = false;

  while (bpf_map_get_next_key(map_fd_, has_current_key ? &current_key : nullptr,
                              &next_key) == 0) {
    has_current_key = true;
    current_key = next_key;

    // PERCPU map：每个 key 有 nr_cpus 份副本，逐副本累加才是该网卡的真实累计量
    const char* raw = LookupPerCpuValue(map_fd_, &next_key);
    if (raw == nullptr) {
      continue;
    }
    const auto* slots = reinterpret_cast<const net_stats*>(raw);

    net_stats stats = {};
    for (int cpu = 0; cpu < num_possible_cpus_; ++cpu) {
      stats.rcv_bytes += slots[cpu].rcv_bytes;
      stats.rcv_packets += slots[cpu].rcv_packets;
      stats.snd_bytes += slots[cpu].snd_bytes;
      stats.snd_packets += slots[cpu].snd_packets;
    }

    std::string ifname = GetIfName(next_key);
    if (ifname.empty() || !IsPhysicalInterface(ifname)) {
      continue;
    }

    auto* net_info = monitor_info->add_net_info();
    net_info->set_name(ifname);
    //计算速率
    auto cache_it = cache_.find(next_key);
    if (cache_it != cache_.end()) {
      const auto elapsed =
          std::chrono::duration<double>(now - cache_it->second.timestamp).count();
      if (elapsed > 0) {
        const auto& last = cache_it->second;
        const uint64_t rcv_bytes_delta =
            stats.rcv_bytes >= last.rcv_bytes ? stats.rcv_bytes - last.rcv_bytes
                                              : stats.rcv_bytes;
        const uint64_t snd_bytes_delta =
            stats.snd_bytes >= last.snd_bytes ? stats.snd_bytes - last.snd_bytes
                                              : stats.snd_bytes;
        const uint64_t rcv_packets_delta =
            stats.rcv_packets >= last.rcv_packets
                ? stats.rcv_packets - last.rcv_packets
                : stats.rcv_packets;
        const uint64_t snd_packets_delta =
            stats.snd_packets >= last.snd_packets
                ? stats.snd_packets - last.snd_packets
                : stats.snd_packets;

        net_info->set_rcv_rate(
            static_cast<float>(rcv_bytes_delta / 1024.0 / elapsed));
        net_info->set_send_rate(
            static_cast<float>(snd_bytes_delta / 1024.0 / elapsed));
        net_info->set_rcv_packets_rate(
            static_cast<float>(rcv_packets_delta / elapsed));
        net_info->set_send_packets_rate(
            static_cast<float>(snd_packets_delta / elapsed));
      }
    }

    cache_[next_key] = NetStatCache{stats.rcv_bytes, stats.rcv_packets,
                                    stats.snd_bytes, stats.snd_packets, now};
  }

  CollectProcessStats(monitor_info);
}

// 读取进程级归因 map，算出每个进程的实时速率，取 Top N 写进上报报文。
//
// 这里做三件 /proc 做不到的事：
// 1. 跨 CPU 归并 PERCPU map：map 里每个 key 有 nr_cpus 份副本，
//    必须逐副本累加才是该进程的真实累计量；
// 2. 回收已退出进程的 map 条目：否则 map 会随主机运行时间单调增长；
// 3. 按速率排序取 Top N，把"这些流量是谁产生的"变成可上报的结构化字段。
void NetEbpfMonitor::CollectProcessStats(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info || proc_map_fd_ < 0 || num_possible_cpus_ <= 0) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  std::vector<ProcRate> all_rates;
  std::vector<uint32_t> dead_pids;
  double total_rate = 0;

  uint32_t current_key = 0;
  uint32_t next_key = 0;
  bool has_current_key = false;

  while (bpf_map_get_next_key(proc_map_fd_,
                              has_current_key ? &current_key : nullptr,
                              &next_key) == 0) {
    has_current_key = true;
    current_key = next_key;

    const char* raw = LookupPerCpuValue(proc_map_fd_, &next_key);
    if (raw == nullptr) {
      continue;
    }
    const auto* slots = reinterpret_cast<const struct proc_net_stats*>(raw);
    uint64_t snd_bytes = 0;
    uint64_t rcv_bytes = 0;
    std::string name;
    for (int cpu = 0; cpu < num_possible_cpus_; ++cpu) {
      snd_bytes += slots[cpu].snd_bytes;
      rcv_bytes += slots[cpu].rcv_bytes;
      // comm 只在条目创建时写过，取第一个非空的副本即可
      if (name.empty() && slots[cpu].comm[0] != '\0') {
        char comm[sizeof(slots[cpu].comm) + 1] = {};
        std::memcpy(comm, slots[cpu].comm, sizeof(slots[cpu].comm));
        name = comm;
      }
    }

    // 进程已经退出：先记下来，等遍历结束再删。
    // 不能在遍历过程中删当前 key，否则 bpf_map_get_next_key 找不到该 key 会
    // 回退到 map 的第一个元素，导致重复遍历甚至死循环。
    if (kill(static_cast<pid_t>(next_key), 0) != 0 && errno == ESRCH) {
      dead_pids.push_back(next_key);
      proc_cache_.erase(next_key);
      continue;
    }

    if (name.empty()) {
      name = "unknown";
    }

    auto cache_it = proc_cache_.find(next_key);
    if (cache_it == proc_cache_.end()) {
      // 首次看到该进程：本次没有可算的差值，只记录基线
      proc_cache_[next_key] = ProcStatCache{snd_bytes, rcv_bytes, now};
      continue;
    }

    const auto& last = cache_it->second;
    const double elapsed =
        std::chrono::duration<double>(now - last.timestamp).count();
    if (elapsed <= 0) {
      continue;
    }

    // 进程重启后 PID 被复用、或计数器被重置时，当前值会小于上次值，
    // 这时按"从零重新开始"处理，而不是算出一个负的差值。
    const uint64_t snd_delta =
        snd_bytes >= last.snd_bytes ? snd_bytes - last.snd_bytes : snd_bytes;
    const uint64_t rcv_delta =
        rcv_bytes >= last.rcv_bytes ? rcv_bytes - last.rcv_bytes : rcv_bytes;

    ProcRate rate;
    rate.pid = next_key;
    rate.name = name;
    rate.snd_rate = static_cast<double>(snd_delta) / 1024.0 / elapsed;
    rate.rcv_rate = static_cast<double>(rcv_delta) / 1024.0 / elapsed;
    total_rate += rate.snd_rate + rate.rcv_rate;
    all_rates.push_back(std::move(rate));

    cache_it->second = ProcStatCache{snd_bytes, rcv_bytes, now};
  }

  for (uint32_t pid : dead_pids) {
    bpf_map_delete_elem(proc_map_fd_, &pid);
  }

  if (all_rates.empty()) {
    return;
  }

  std::sort(all_rates.begin(), all_rates.end(),
            [](const ProcRate& lhs, const ProcRate& rhs) {
              return lhs.snd_rate + lhs.rcv_rate > rhs.snd_rate + rhs.rcv_rate;
            });

  const size_t count = std::min(all_rates.size(), kTopProcessCount);
  for (size_t i = 0; i < count; ++i) {
    const auto& rate = all_rates[i];
    const double total = rate.snd_rate + rate.rcv_rate;
    auto* proc_info = monitor_info->add_process_net_info();
    proc_info->set_pid(rate.pid);
    proc_info->set_name(rate.name);
    proc_info->set_send_rate(static_cast<float>(rate.snd_rate));
    proc_info->set_rcv_rate(static_cast<float>(rate.rcv_rate));
    proc_info->set_total_rate(static_cast<float>(total));
    proc_info->set_share_percent(
        total_rate > 0 ? static_cast<float>(total / total_rate * 100.0) : 0.0f);
  }
}

void NetEbpfMonitor::Stop() {
  CleanupEbpf();
}

}  // namespace monitor
/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */
