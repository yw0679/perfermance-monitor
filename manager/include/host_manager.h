/**
 * 文件归类：1
 * 说明：定义主机监控数据管理、评分计算和落库相关接口。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "monitor_info.pb.h"

namespace monitor {

struct HostScore {
  monitor::proto::MonitorInfo info;
  double score;
  std::chrono::system_clock::time_point timestamp;
};

struct ScheduleState {
  double base_weight = 0;
  double current_weight = 0;
};

// 管理多个远程主机的监控数据（推送模式）
class HostManager {
 public:
  HostManager();
  ~HostManager();

  // 启动后台处理线程
  void Start();
  void Stop();

  // 接收工作者推送的数据（由 gRPC 服务调用），并且写进mysql，输出到终端。
  void OnDataReceived(const monitor::proto::MonitorInfo& info);

  // 预留接口：当前版本没有外部调用点。
  // host_scores_ 本身仍会在 OnDataReceived/ProcessLoop 中被维护。
  std::unordered_map<std::string, HostScore> GetAllHostScores();

  // 预留接口：当前版本没有外部调用点。
  // 对应的调度状态 schedule_states_ 仍会在 OnDataReceived 中持续更新。
  std::string GetBestHost();

 private:
  void ProcessLoop();
  double CalcScore(const monitor::proto::MonitorInfo& info);
  double CalcSchedulingWeight(double score) const;
  // 预留给 GetBestHost() 的兜底选择逻辑；当前版本没有实际走到这里。
  std::string SelectHighestScoreHostLocked() const;
  bool WriteToMysql(const std::string& host_name, const HostScore& host_score,
                    float cpu_percent_rate, float load_avg_1_rate,
                    float mem_used_percent_rate, float disk_util_percent_rate,
                    float net_in_rate_rate, float net_out_rate_rate);

  // 当前在用：保存每台主机最新一次上报后的内存态快照和评分。
  std::unordered_map<std::string, HostScore> host_scores_;
  // 当前在用：保存平滑加权轮询所需的调度状态，虽然对外调度接口暂未接入。
  std::unordered_map<std::string, ScheduleState> schedule_states_;
  std::mutex mtx_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> thread_;
};

}  // namespace monitor
