/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */

#include "monitor/net_ebpf_monitor.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <cerrno>
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
}

namespace monitor {

namespace {

constexpr __u32 kTcHandle = 1;
constexpr __u32 kTcPriority = 1;

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
  return true;
}

void NetEbpfMonitor::CleanupEbpf() {
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
  loaded_ = false;
  cache_.clear();
  ifname_cache_.clear();
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
  net_stats stats = {};

  while (bpf_map_get_next_key(map_fd_, has_current_key ? &current_key : nullptr,
                              &next_key) == 0) {
    has_current_key = true;
    current_key = next_key;

    if (bpf_map_lookup_elem(map_fd_, &next_key, &stats) != 0) {
      continue;
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
}

void NetEbpfMonitor::Stop() {
  CleanupEbpf();
}

}  // namespace monitor
/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */
