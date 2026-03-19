/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "monitor/monitor_inter.h"

struct net_stats_bpf;

namespace monitor {

/**
 * 基于 eBPF 的网络流量监控器
 * 
 * 使用 eBPF TC Hook 挂载到内核网络路径，
 * 实时统计每个网卡的收发流量。
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
  // 初始化 eBPF 程序
  bool InitEbpf();
  
  // 清理 eBPF 资源
  void CleanupEbpf();
  
  // 根据 ifindex 获取网卡名称
  std::string GetIfName(uint32_t ifindex);

  // 上一次采集的数据，用于计算速率
  struct NetStatCache {
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    std::chrono::steady_clock::time_point timestamp;
  };

  std::unordered_map<uint32_t, NetStatCache> cache_;  // key: ifindex
  std::unordered_map<uint32_t, std::string> ifname_cache_;  // ifindex -> name
  std::vector<uint32_t> attached_ifindexes_;  // 已附加 TC hook 的网卡
  
  struct net_stats_bpf* skel_ = nullptr;
  int map_fd_ = -1;
  bool loaded_ = false;
};

}  // namespace monitor
/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */
