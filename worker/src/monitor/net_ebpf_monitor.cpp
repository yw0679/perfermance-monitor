/**
 * 文件归类：eBPF 模块文件（可选能力，默认未启用）
 * 说明：仅在 ENABLE_EBPF 打开且依赖满足时参与构建或运行。
 */

#include "monitor/net_ebpf_monitor.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <net/if.h>
#include <string>

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

bool ShouldSkipInterface(const std::string& ifname) {
  return ifname == "lo" || ifname.rfind("docker", 0) == 0 ||
         ifname.rfind("veth", 0) == 0 || ifname.rfind("br-", 0) == 0 ||
         ifname.rfind("virbr", 0) == 0;
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
    if (ShouldSkipInterface(entry->d_name)) {
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

int CreateClsactQdisc(int ifindex) {
  char ifname[IF_NAMESIZE] = {};
  if (if_indextoname(ifindex, ifname) == nullptr) {
    return -1;
  }

  std::string command = "tc qdisc add dev ";
  command += ifname;
  command += " clsact >/dev/null 2>&1";
  return std::system(command.c_str());
}

void DeleteClsactQdisc(int ifindex) {
  char ifname[IF_NAMESIZE] = {};
  if (if_indextoname(ifindex, ifname) == nullptr) {
    return;
  }

  std::string command = "tc qdisc del dev ";
  command += ifname;
  command += " clsact >/dev/null 2>&1";
  std::system(command.c_str());
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
  skel_ = net_stats_bpf__open();
  if (!skel_) {
    std::cerr << "NetEbpfMonitor: failed to open BPF skeleton" << std::endl;
    return false;
  }

  const int err = net_stats_bpf__load(skel_);
  if (err != 0) {
    std::cerr << "NetEbpfMonitor: failed to load BPF program: "
              << std::strerror(-err) << std::endl;
    CleanupEbpf();
    return false;
  }

  map_fd_ = bpf_map__fd(skel_->maps.net_stats_map);
  if (map_fd_ < 0) {
    std::cerr << "NetEbpfMonitor: failed to get map fd" << std::endl;
    CleanupEbpf();
    return false;
  }

  const int ingress_fd = bpf_program__fd(skel_->progs.tc_ingress);
  const int egress_fd = bpf_program__fd(skel_->progs.tc_egress);
  for (uint32_t ifindex : GetAllIfIndexes()) {
    CreateClsactQdisc(static_cast<int>(ifindex));

    const bool ingress_ok =
        AttachTcProgram(static_cast<int>(ifindex), BPF_TC_INGRESS, ingress_fd,
                        "ingress");
    const bool egress_ok =
        AttachTcProgram(static_cast<int>(ifindex), BPF_TC_EGRESS, egress_fd,
                        "egress");
    if (!ingress_ok || !egress_ok) {
      DetachTcProgram(static_cast<int>(ifindex), BPF_TC_INGRESS);
      DetachTcProgram(static_cast<int>(ifindex), BPF_TC_EGRESS);
      DeleteClsactQdisc(static_cast<int>(ifindex));
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
    DeleteClsactQdisc(static_cast<int>(ifindex));
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
    if (ifname.empty() || ShouldSkipInterface(ifname)) {
      continue;
    }

    auto* net_info = monitor_info->add_net_info();
    net_info->set_name(ifname);
    net_info->set_err_in(0);
    net_info->set_err_out(0);
    net_info->set_drop_in(0);
    net_info->set_drop_out(0);

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
 * 文件归类：eBPF 模块文件（可选能力，默认未启用）
 * 说明：仅在 ENABLE_EBPF 打开且依赖满足时参与构建或运行。
 */
