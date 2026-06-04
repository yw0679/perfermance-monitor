/**
 * 文件归类：1
 * 说明：定义主机监控数据管理、评分计算和落库相关接口。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

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

// 表示一次完整的 MySQL 持久化任务。
// 该结构把 OnDataReceived 中已经计算好的评分、变化率和原始监控快照整体搬运到后台写线程，
// 这样 gRPC 接收线程只需要把任务压入队列即可返回，不再同步等待数据库。
struct MysqlWriteTask {
  std::string host_name;
  HostScore host_score;
  float cpu_percent_rate = 0;
  float load_avg_1_rate = 0;
  float mem_used_percent_rate = 0;
  float disk_util_percent_rate = 0;
  float net_in_rate_rate = 0;
  float net_out_rate_rate = 0;
};

// 管理多个远程主机的监控数据（推送模式）
class HostManager {
 public:
  // 构造函数负责初始化运行标记，但不会主动启动线程。
  // 这样 main 可以先完成 gRPC 回调绑定，再显式调用 Start() 开始后台工作。
  HostManager();
  // 析构函数负责兜底关闭后台线程，避免对象销毁时仍有清理线程或写线程访问成员数据。
  ~HostManager();

  // 启动后台线程。
  // 当前会启动两类后台任务：
  // 1. 过期主机清理线程，定期删除长时间未上报的内存态快照；
  // 2. MySQL 写线程，异步消费写队列并复用数据库连接执行落库。
  void Start();
  // 停止后台线程并等待退出。
  // 该函数会先将 running_ 置为 false，再唤醒正在等待写队列的线程，最后 join 所有后台线程。
  void Stop();

  // 接收工作者推送的数据（由 gRPC 服务调用）。
  // 该函数负责完成标识归一化、评分计算、变化率计算和内存态更新，然后把写库任务压入异步队列。
  void OnDataReceived(const monitor::proto::MonitorInfo& info);

  // 返回每台主机最近一次上报对应的内存态快照和评分。
  // 返回值是一个拷贝，便于调用方在无锁条件下遍历当前快照。
  std::unordered_map<std::string, HostScore> GetAllHostScores();

  // 基于平滑加权轮询从当前在线主机中选出一个最优节点。
  // 每次调用会推进调度状态，保证并发请求下流量被按权重比例分散到整个集群，
  // 避免惊群效应把所有任务压到同一台机器上。
  std::string GetBestHost();

 private:
  // 周期性清理长期未上报主机的后台循环。
  // 该线程只处理内存态淘汰，不参与数据库写入。
  void ProcessLoop();
  // MySQL 写线程的后台循环。
  // 该函数会等待写队列中出现任务，复用长连接执行落库，并在连接异常时尝试重连。
  void MysqlWriteLoop();
  // 将一条已计算完毕的监控样本压入写队列，并通知后台写线程消费。
  void EnqueueMysqlWrite(MysqlWriteTask task);
  double CalcScore(const monitor::proto::MonitorInfo& info);
  double CalcSchedulingWeight(double score) const;
  // 预留给 GetBestHost() 的兜底选择逻辑；当前版本没有实际走到这里。
  std::string SelectHighestScoreHostLocked() const;
  // 使用已经建立好的 MySQL 连接执行一次完整落库。
  // 返回 true 表示三类表写入都成功，返回 false 表示任意一条 SQL 执行失败。
  bool WriteToMysql(
#ifdef ENABLE_MYSQL
      MYSQL* conn,
#endif
      const std::string& host_name, const HostScore& host_score,
      float cpu_percent_rate, float load_avg_1_rate,
      float mem_used_percent_rate, float disk_util_percent_rate,
      float net_in_rate_rate, float net_out_rate_rate);

  // 当前在用：保存每台主机最新一次上报后的内存态快照和评分。
  std::unordered_map<std::string, HostScore> host_scores_;
  // 当前在用：保存平滑加权轮询所需的调度状态。
  std::unordered_map<std::string, ScheduleState> schedule_states_;
  std::mutex mtx_;
  // 保护 MySQL 写队列及其条件变量配套状态。
  std::mutex mysql_queue_mtx_;
  std::condition_variable mysql_queue_cv_;
  std::deque<MysqlWriteTask> mysql_write_queue_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> cleanup_thread_;
  std::unique_ptr<std::thread> mysql_thread_;
};

}  // namespace monitor
