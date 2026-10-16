/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "monitor/monitor_inter.h"

struct net_stats_bpf;
struct bpf_link;

namespace monitor {

/**
 * 基于 eBPF 的网络流量监控器
 *
 * 使用两类 hook，分别覆盖两个 /proc 覆盖不到的维度：
 * - TC ingress/egress：网卡维度的收发流量，复用同一个已加载程序挂到多块物理网卡上；
 * - kprobe/kretprobe：socket 层维度的进程级流量归因，回答"这些流量是谁产生的"。
 */
class NetEbpfMonitor : public MonitorInter {
 public:
  NetEbpfMonitor();
  ~NetEbpfMonitor() override;

  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override;

  // 检查 eBPF 是否成功加载
  bool IsLoaded() const { return loaded_; }

 private:
  // 初始化 eBPF 程序:打开skeleton，把ebpf加载进内核，取出map的fd，把tc程序挂上去。
  bool InitEbpf();

  // 把 kprobe/kretprobe 挂到 tcp_sendmsg / tcp_recvmsg 上，做进程级流量归因。
  // 返回 false 表示挂载失败，此时 TC 部分的统计仍然可用。
  bool AttachProcessProbes();

  // 清理 eBPF 资源
  void CleanupEbpf();

  // 根据 ifindex 获取网卡名称
  std::string GetIfName(uint32_t ifindex);

  // 读 PERCPU map 的一个 key：内核会把所有 CPU 的副本连续拷进共享读缓冲。
  // 返回缓冲首地址（调用方负责按 CPU 归并），失败返回 nullptr。
  const char* LookupPerCpuValue(int fd, const void* key);

  // 读取进程级 map，计算速率后按总流量排序，把 Top N 写进 MonitorInfo。
  void CollectProcessStats(monitor::proto::MonitorInfo* monitor_info);

  // 上一次采集的数据，用于计算速率
  struct NetStatCache {
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    std::chrono::steady_clock::time_point timestamp;
  };

  // 进程级累计计数器的上一次采样值，用于算速率
  struct ProcStatCache {
    uint64_t snd_bytes;
    uint64_t rcv_bytes;
    std::chrono::steady_clock::time_point timestamp;
  };

  // 单次采样里一个进程的实时速率
  struct ProcRate {
    uint32_t pid;
    std::string name;
    double snd_rate;  // kB/s
    double rcv_rate;  // kB/s
  };

  std::unordered_map<uint32_t, NetStatCache> cache_;  // key: ifindex
  std::unordered_map<uint32_t, std::string> ifname_cache_;  // ifindex -> name
  std::vector<uint32_t> attached_ifindexes_;  // 已附加 TC hook 的网卡

  std::unordered_map<uint32_t, ProcStatCache> proc_cache_;  // key: tgid
  // 两张 PERCPU map 共用的读缓冲，尺寸 num_possible_cpus_ * kPerCpuValueSize。
  // 两个 map 的采样是顺序进行的，所以可以复用同一块内存。
  std::vector<char> percpu_buf_;
  int num_possible_cpus_ = 0;

  struct net_stats_bpf* skel_ = nullptr;
  struct bpf_link* send_probe_link_ = nullptr;
  struct bpf_link* recv_probe_link_ = nullptr;
  int map_fd_ = -1;
  int proc_map_fd_ = -1;
  bool loaded_ = false;
};

}  // namespace monitor
/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */
