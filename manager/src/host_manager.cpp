/**
 * 文件归类：1
 * 说明：实现主机数据处理、评分计算、过期清理和 MySQL 写入逻辑。
 */

#include "host_manager.h"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#include <mysql/mysql.h>

namespace monitor {

namespace {
constexpr double kScheduleWeightSmoothing = 0.3;
constexpr auto kHostCleanupInterval = std::chrono::seconds(60);
constexpr auto kHostExpireWindow = std::chrono::seconds(60);
}  // namespace

namespace {
const char* MYSQL_HOST = "127.0.0.1";
const char* MYSQL_USER = "monitor";
const char* MYSQL_PASS = "monitor123";
const char* MYSQL_DB = "monitor_db";

}  // namespace

// 用于变化率计算的性能采样数据。
// 这些样本只保存在 manager 内存里，用于比较当前值和上一次值，从而生成变化率字段。
struct PerfSample {
  float cpu_percent = 0, usr_percent = 0, system_percent = 0;
  float nice_percent = 0, idle_percent = 0, io_wait_percent = 0;
  float irq_percent = 0, soft_irq_percent = 0;
  float steal_percent = 0, guest_percent = 0, guest_nice_percent = 0;
  float load_avg_1 = 0, load_avg_3 = 0, load_avg_15 = 0;
  float mem_used_percent = 0, mem_total = 0, mem_free = 0, mem_avail = 0;
  float disk_util_percent = 0;
  float net_in_rate = 0, net_out_rate = 0;
  float score = 0;
};
static std::map<std::string, PerfSample> last_perf_samples;

// 构造函数只初始化运行标记。
// 后台线程的真实启动时机由 Start() 控制，这样可以让对象创建和线程启动解耦。
HostManager::HostManager() : running_(false) {}

// 析构时统一走 Stop()，确保后台线程在对象释放前全部退出。
HostManager::~HostManager() { Stop(); }

// 启动 HostManager 依赖的后台线程。
// 为了避免重复启动，该函数会先检查 running_ 状态；启动后分别创建清理线程和 MySQL 写线程。
void HostManager::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }

  cleanup_thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
  mysql_thread_ = std::make_unique<std::thread>(&HostManager::MysqlWriteLoop, this);
}

// 停止后台线程并等待其结束。
// MySQL 写线程可能阻塞在条件变量上，因此这里除了设置 running_ 为 false 外，还需要显式通知它退出。
void HostManager::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  mysql_queue_cv_.notify_all();

  if (cleanup_thread_ && cleanup_thread_->joinable()) {
    cleanup_thread_->join();
  }
  if (mysql_thread_ && mysql_thread_->joinable()) {
    mysql_thread_->join();
  }

  cleanup_thread_.reset();
  mysql_thread_.reset();
}

// 定期清理过期主机快照。
// 该循环每隔固定时间检查 host_scores_，如果某台主机超过过期窗口没有上报，就把它从内存态和调度态中删除。
void HostManager::ProcessLoop() {
  while (running_) {
    std::this_thread::sleep_for(kHostCleanupInterval);
    if (!running_) {
      break;
    }

    auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = host_scores_.begin(); it != host_scores_.end();) {
      const auto age = now - it->second.timestamp;
      if (age > kHostExpireWindow) {
        const std::string host_name = it->first;
        std::cout << "Removing stale host: " << host_name << std::endl;
        schedule_states_.erase(host_name);
        it = host_scores_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// MySQL 写线程主循环。
// 该线程会长期持有一个可复用连接；如果连接断开或尚未建立，会在消费下一条任务前尝试重连。
// 退出条件是 running_ 已关闭且队列已被消费完毕，从而保证 Stop() 前已入队的数据尽量完成刷盘。
void HostManager::MysqlWriteLoop() {
  MYSQL* conn = nullptr;

  while (true) {
    MysqlWriteTask task;
    {
      std::unique_lock<std::mutex> lock(mysql_queue_mtx_);
      mysql_queue_cv_.wait(lock, [this] {
        return !running_ || !mysql_write_queue_.empty();
      });

      if (!running_ && mysql_write_queue_.empty()) {
        break;
      }

      task = std::move(mysql_write_queue_.front());
      mysql_write_queue_.pop_front();
    }

    if (!conn) {
      conn = mysql_init(nullptr);
      if (!conn) {
        std::cerr << "mysql_init failed in MysqlWriteLoop" << std::endl;
        continue;
      }

      if (!mysql_real_connect(conn, MYSQL_HOST, MYSQL_USER, MYSQL_PASS, MYSQL_DB,
                              0, nullptr, 0)) {
        std::cerr << "mysql_real_connect failed in MysqlWriteLoop: "
                  << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = nullptr;
        continue;
      }

      mysql_set_character_set(conn, "utf8mb4");
    }

    const bool write_ok =
        WriteToMysql(conn, task.host_name, task.host_score, task.cpu_percent_rate,
                     task.load_avg_1_rate, task.mem_used_percent_rate,
                     task.disk_util_percent_rate, task.net_in_rate_rate,
                     task.net_out_rate_rate);
    if (!write_ok) {
      std::cerr << "MySQL async write failed for host: " << task.host_name
                << std::endl;
      mysql_close(conn);
      conn = nullptr;
    }
  }

  if (conn) {
    mysql_close(conn);
  }
}

// 将已计算完成的写库任务压入队列。
// 这里使用移动语义减少 HostScore/MonitorInfo 拷贝成本，并在入队后立刻通知后台写线程。
void HostManager::EnqueueMysqlWrite(MysqlWriteTask task) {
  {
    std::lock_guard<std::mutex> lock(mysql_queue_mtx_);
    mysql_write_queue_.push_back(std::move(task));
  }
  mysql_queue_cv_.notify_one();
}

// 监控数据主处理入口。
// 该函数仍然负责完成评分、变化率计算和内存态刷新，但数据库写入已改为异步入队，
// 因此 gRPC 请求线程不再为 MySQL 连接建立和 SQL 执行阻塞。
void HostManager::OnDataReceived(const monitor::proto::MonitorInfo& info) {
  // 构建服务器唯一标识: hostname_ip
  std::string host_name;
  if (info.has_host_info()) {
    const auto& host_info = info.host_info();
    std::string hostname = host_info.hostname();
    std::string ip = host_info.ip_address();
    
    if (!hostname.empty() && !ip.empty()) {
      host_name = hostname + "_" + ip;  // 格式: hostname_192.168.1.100
    } else if (!hostname.empty()) {
      host_name = hostname;
    } else if (!ip.empty()) {
      host_name = ip;
    }
  }
  
  // 兼容旧版本：如果 host_info 为空，使用 name 字段
  if (host_name.empty()) {
    host_name = info.name();
  }
  
  if (host_name.empty()) {
    std::cerr << "Received data with empty server identifier" << std::endl;
    return;
  }

  double score = CalcScore(info);
  auto now = std::chrono::system_clock::now();

  // 网络速率计算：汇总所有网卡，避免只看第一块网卡。
  double net_in_rate = 0, net_out_rate = 0;
  for (int i = 0; i < info.net_info_size(); ++i) {
    net_in_rate += info.net_info(i).rcv_rate();
    net_out_rate += info.net_info(i).send_rate();
  }

  // 当前采样
  PerfSample curr;
  if (info.cpu_stat_size() > 0) {
    const auto& cpu = info.cpu_stat(0);
    curr.cpu_percent = cpu.cpu_percent();
    curr.usr_percent = cpu.usr_percent();
    curr.system_percent = cpu.system_percent();
    curr.nice_percent = cpu.nice_percent();
    curr.idle_percent = cpu.idle_percent();
    curr.io_wait_percent = cpu.io_wait_percent();
    curr.irq_percent = cpu.irq_percent();
    curr.soft_irq_percent = cpu.soft_irq_percent();
  }
  if (info.has_cpu_load()) {
    curr.load_avg_1 = info.cpu_load().load_avg_1();
    curr.load_avg_3 = info.cpu_load().load_avg_3();
    curr.load_avg_15 = info.cpu_load().load_avg_15();
  }
  if (info.has_mem_info()) {
    curr.mem_used_percent = info.mem_info().used_percent();
    curr.mem_total = info.mem_info().total();
    curr.mem_free = info.mem_info().free();
    curr.mem_avail = info.mem_info().avail();
  }
  for (int i = 0; i < info.disk_info_size(); ++i) {
    curr.disk_util_percent = std::max(
        curr.disk_util_percent,
        static_cast<float>(info.disk_info(i).util_percent()));
  }
  curr.net_in_rate = net_in_rate;
  curr.net_out_rate = net_out_rate;
  curr.score = score;

  // 变化率计算
  PerfSample last = last_perf_samples[host_name];
  auto rate = [](float now_val, float last_val) -> float {
    if (last_val == 0) return 0;
    return (now_val - last_val) / last_val;
  };

  float cpu_percent_rate = rate(curr.cpu_percent, last.cpu_percent);
  float load_avg_1_rate = rate(curr.load_avg_1, last.load_avg_1);
  float mem_used_percent_rate = rate(curr.mem_used_percent, last.mem_used_percent);
  float disk_util_percent_rate =
      rate(curr.disk_util_percent, last.disk_util_percent);
  float net_in_rate_rate = rate(curr.net_in_rate, last.net_in_rate);
  float net_out_rate_rate = rate(curr.net_out_rate, last.net_out_rate);

  last_perf_samples[host_name] = curr;
  HostScore host_score{info, score, now};

  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_scores_[host_name] = host_score;

    auto& schedule_state = schedule_states_[host_name];
    const double new_weight = CalcSchedulingWeight(score);
    if (new_weight <= 0) {
      schedule_state.base_weight = 0;
      schedule_state.current_weight = 0;
    } else if (schedule_state.base_weight <= 0) {
      schedule_state.base_weight = new_weight;
    } else {
      schedule_state.base_weight =
          schedule_state.base_weight * (1.0 - kScheduleWeightSmoothing) +
          new_weight * kScheduleWeightSmoothing;
    }
  }

  auto all_host_scores = GetAllHostScores();
  std::string best_host = GetBestHost();

  EnqueueMysqlWrite(MysqlWriteTask{
      host_name,
      host_score,
      cpu_percent_rate,
      load_avg_1_rate,
      mem_used_percent_rate,
      disk_util_percent_rate,
      net_in_rate_rate,
      net_out_rate_rate,
  });

  std::cout << "\n================== Received Data ==================" << std::endl;
  std::cout << "Server: " << host_name << ", Score: " << score << std::endl;
  
  // CPU 详细信息
  std::cout << "\n--- CPU ---" << std::endl;
  std::cout << "  Usage: " << curr.cpu_percent << "%, "
            << "User: " << curr.usr_percent << "%, "
            << "System: " << curr.system_percent << "%" << std::endl;
  std::cout << "  Nice: " << curr.nice_percent << "%, "
            << "Idle: " << curr.idle_percent << "%, "
            << "IOWait: " << curr.io_wait_percent << "%" << std::endl;
  std::cout << "  IRQ: " << curr.irq_percent << "%, "
            << "SoftIRQ: " << curr.soft_irq_percent << "%" << std::endl;
  std::cout << "  Load: " << curr.load_avg_1 << "/" << curr.load_avg_3 << "/" << curr.load_avg_15 << std::endl;
  
  // 内存详细信息
  std::cout << "\n--- Memory ---" << std::endl;
  std::cout << "  Used: " << curr.mem_used_percent << "%, "
            << "Total: " << curr.mem_total << " GB" << std::endl;
  std::cout << "  Free: " << curr.mem_free << " GB, "
            << "Avail: " << curr.mem_avail << " GB" << std::endl;
  
  // 网络详细信息
  std::cout << "\n--- Network ---" << std::endl;
  std::cout << "  In: " << net_in_rate << " KB/s, "
            << "Out: " << net_out_rate << " KB/s" << std::endl;
  for (int i = 0; i < info.net_info_size(); ++i) {
    const auto& net = info.net_info(i);
    std::cout << "  [" << net.name() << "] Recv: " << net.rcv_rate() << " KB/s, "
              << "Send: " << net.send_rate() << " KB/s" << std::endl;
  }
  
  // 磁盘详细信息
  std::cout << "\n--- Disk ---" << std::endl;
  for (int i = 0; i < info.disk_info_size(); ++i) {
    const auto& disk = info.disk_info(i);
    std::cout << "  [" << disk.name() << "] "
              << "Read: " << disk.read_bytes_per_sec() / 1024.0 << " KB/s, "
              << "Write: " << disk.write_bytes_per_sec() / 1024.0 << " KB/s, "
              << "Util: " << disk.util_percent() << "%" << std::endl;
  }
  if (info.disk_info_size() == 0) {
    std::cout << "  No disk data" << std::endl;
  }
  
  // 变化率信息
  std::cout << "\n--- Change Rates ---" << std::endl;
  std::cout << "  CPU: " << cpu_percent_rate * 100 << "%, "
            << "Mem: " << mem_used_percent_rate * 100 << "%, "
            << "Load: " << load_avg_1_rate * 100 << "%" << std::endl;
  std::cout << "  Disk: " << disk_util_percent_rate * 100 << "%, "
            << "NetIn: " << net_in_rate_rate * 100 << "%, "
            << "NetOut: " << net_out_rate_rate * 100 << "%" << std::endl;
  
  std::cout << "\n--- Database ---" << std::endl;
  std::cout << "  MySQL write: queued for async flush" << std::endl;
  std::cout << "\n--- Host Scores ---" << std::endl;
  std::cout << "  Best host: "
            << (best_host.empty() ? "<none>" : best_host) << std::endl;
  if (all_host_scores.empty()) {
    std::cout << "  No host scores" << std::endl;
  } else {
    for (const auto& [name, score_info] : all_host_scores) {
      std::cout << "  " << name << " => " << score_info.score << std::endl;
    }
  }
  std::cout << "====================================================\n" << std::endl;
}

// 预留接口：当前版本没有外部调用点。
// 这里返回一份当前快照副本，便于查询层在不长期持锁的情况下读取主机状态。
std::unordered_map<std::string, HostScore> HostManager::GetAllHostScores() {
  std::lock_guard<std::mutex> lock(mtx_);
  return host_scores_;
}

// 预留接口：当前版本没有外部调用点。
// 该函数按照平滑加权轮询逻辑从当前可见主机里选出一个最优节点。
std::string HostManager::GetBestHost() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (host_scores_.empty()) {
    return "";
  }

  double total_weight = 0;
  double best_current_weight = -std::numeric_limits<double>::infinity();
  std::string best_host;

  // 基于实时评分做平滑加权轮询：分高的节点拿更多请求，但不会持续独占。
  for (const auto& [host, data] : host_scores_) {
    auto& state = schedule_states_[host];
    if (state.base_weight <= 0) {
      state.base_weight = CalcSchedulingWeight(data.score);
    }

    if (state.base_weight <= 0) {
      continue;
    }

    state.current_weight += state.base_weight;
    total_weight += state.base_weight;

    if (state.current_weight > best_current_weight) {
      best_current_weight = state.current_weight;
      best_host = host;
    }
  }

  if (best_host.empty() || total_weight <= 0) {
    return SelectHighestScoreHostLocked();
  }

  schedule_states_[best_host].current_weight -= total_weight;
  return best_host;
}

// 根据实时评分计算调度权重。
// 当前策略非常直接：评分越高权重越大；如果评分无效或小于等于 0，则不给调度权重。
double HostManager::CalcSchedulingWeight(double score) const {
  return score > 0 ? score : 0;
}

// 预留给 GetBestHost() 的内部辅助逻辑。
// 当平滑加权轮询无法选出结果时，该函数退化为简单地返回当前评分最高的主机。
std::string HostManager::SelectHighestScoreHostLocked() const {
  std::string best_host;
  double best_score = -1;

  for (const auto& [host, data] : host_scores_) {
    if (data.score > best_score) {
      best_score = data.score;
      best_host = host;
    }
  }

  return best_host;
}

// 根据当前监控样本计算主机综合评分。
// 评分模型综合考虑 CPU、内存、负载、磁盘利用率和网络带宽，并将结果归一化到 0-100。
double HostManager::CalcScore(const monitor::proto::MonitorInfo& info) {
  // ============================================================
  // 性能评分模型 - 针对学校选课/查成绩系统高并发场景优化
  // ============================================================
  // 权重配置：
  // - CPU 使用率: 35%
  // - 内存使用率: 30%
  // - CPU 负载:   15%
  // - 磁盘 IO:    15%
  // - 网络带宽:    5% (收发各 2.5%)
  // ============================================================
  
  const double cpu_weight = 0.35;
  const double mem_weight = 0.30;
  const double load_weight = 0.15;
  const double disk_weight = 0.15;
  const double net_weight = 0.05;

  const double load_coefficient = 1.5;  // I/O 密集型场景系数
  const double max_bandwidth = 125000000.0;  // 1Gbps

  double cpu_percent = 0, load_avg_1 = 0, mem_percent = 0;
  double net_recv_rate = 0, net_send_rate = 0, disk_util = 0;
  int cpu_cores = 1;

  if (info.cpu_stat_size() > 0) {
    cpu_percent = info.cpu_stat(0).cpu_percent();
    cpu_cores = info.cpu_stat_size() - 1;
    if (cpu_cores < 1) cpu_cores = 1;
  }
  if (info.has_cpu_load()) {
    load_avg_1 = info.cpu_load().load_avg_1();
  }
  if (info.has_mem_info()) {
    mem_percent = info.mem_info().used_percent();
  }
  if (info.net_info_size() > 0) {
    net_recv_rate = info.net_info(0).rcv_rate();
    net_send_rate = info.net_info(0).send_rate();
  }
  if (info.disk_info_size() > 0) {
    for (int i = 0; i < info.disk_info_size(); ++i) {
      double util = info.disk_info(i).util_percent();
      if (util > disk_util) disk_util = util;
    }
  }

  // 反向归一化
  auto clamp = [](double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
  
  double cpu_score = clamp(1.0 - cpu_percent / 100.0);
  double mem_score = clamp(1.0 - mem_percent / 100.0);
  double load_score = clamp(1.0 - load_avg_1 / (cpu_cores * load_coefficient));
  double disk_score = clamp(1.0 - disk_util / 100.0);
  double net_recv_score = clamp(1.0 - net_recv_rate / max_bandwidth);
  double net_send_score = clamp(1.0 - net_send_rate / max_bandwidth);
  double net_score = (net_recv_score + net_send_score) / 2.0;

  double score = cpu_score * cpu_weight + mem_score * mem_weight +
                 load_score * load_weight + disk_score * disk_weight +
                 net_score * net_weight;

  score *= 100.0;
  return score < 0 ? 0 : (score > 100 ? 100 : score);
}

// 使用已建立的 MySQL 连接执行一次完整落库。
// 该函数由后台写线程调用，不再负责连接创建和销毁，只专注于把一条监控样本写入三类表。
bool HostManager::WriteToMysql(
    MYSQL* conn, const std::string& host_name, const HostScore& host_score,
    float cpu_percent_rate, float load_avg_1_rate,
    float mem_used_percent_rate, float disk_util_percent_rate,
    float net_in_rate_rate, float net_out_rate_rate) {
  if (!conn) {
    return false;
  }

  // 时间戳
  std::time_t t = std::chrono::system_clock::to_time_t(host_score.timestamp);
  std::tm tm_time;
  localtime_r(&t, &tm_time);
  char time_buf[32];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_time);

  const auto& info = host_score.info;

  // ========== 1. 写入主表 server_performance ==========
  {
    float total = 0, free = 0, avail = 0;
    float send_rate = 0, rcv_rate = 0;
    float send_packets_rate = 0, rcv_packets_rate = 0;
    float cpu_percent = 0, usr_percent = 0, system_percent = 0;
    float nice_percent = 0, idle_percent = 0, io_wait_percent = 0;
    float irq_percent = 0, soft_irq_percent = 0;
    float load_avg_1 = 0, load_avg_3 = 0, load_avg_15 = 0, mem_used_percent = 0;
    float disk_util_percent = 0;

    if (info.has_mem_info()) {
      total = info.mem_info().total();
      free = info.mem_info().free();
      avail = info.mem_info().avail();
      mem_used_percent = info.mem_info().used_percent();
    }
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      send_rate += net.send_rate();
      rcv_rate += net.rcv_rate();
      send_packets_rate += net.send_packets_rate();
      rcv_packets_rate += net.rcv_packets_rate();
    }
    if (info.cpu_stat_size() > 0) {
      const auto& cpu = info.cpu_stat(0);
      cpu_percent = cpu.cpu_percent();
      usr_percent = cpu.usr_percent();
      system_percent = cpu.system_percent();
      nice_percent = cpu.nice_percent();
      idle_percent = cpu.idle_percent();
      io_wait_percent = cpu.io_wait_percent();
      irq_percent = cpu.irq_percent();
      soft_irq_percent = cpu.soft_irq_percent();
    }
    if (info.has_cpu_load()) {
      load_avg_1 = info.cpu_load().load_avg_1();
      load_avg_3 = info.cpu_load().load_avg_3();
      load_avg_15 = info.cpu_load().load_avg_15();
    }
    for (int i = 0; i < info.disk_info_size(); ++i) {
      const auto util = static_cast<float>(info.disk_info(i).util_percent());
      if (util > disk_util_percent) disk_util_percent = util;
    }

    std::ostringstream oss;
    oss << "INSERT INTO server_performance "
        << "(server_name, cpu_percent, usr_percent, system_percent, "
        << "nice_percent, idle_percent, io_wait_percent, irq_percent, soft_irq_percent, "
        << "load_avg_1, load_avg_3, load_avg_15, "
        << "mem_used_percent, total, mem_free, avail, "
        << "disk_util_percent, send_rate, rcv_rate, send_packets_rate, rcv_packets_rate, score, "
        << "cpu_percent_rate, load_avg_1_rate, mem_used_percent_rate, "
        << "disk_util_percent_rate, send_rate_rate, rcv_rate_rate, timestamp) VALUES ('"
        << host_name << "',"
        << cpu_percent << "," << usr_percent << "," << system_percent << ","
        << nice_percent << "," << idle_percent << "," << io_wait_percent << ","
        << irq_percent << "," << soft_irq_percent << ","
        << load_avg_1 << "," << load_avg_3 << "," << load_avg_15 << ","
        << mem_used_percent << "," << total << "," << free << "," << avail << ","
        << disk_util_percent << "," << send_rate << "," << rcv_rate << ","
        << send_packets_rate << "," << rcv_packets_rate << "," << host_score.score << ","
        << cpu_percent_rate << "," << load_avg_1_rate << ","
        << mem_used_percent_rate << "," << disk_util_percent_rate << ","
        << net_in_rate_rate << "," << net_out_rate_rate
        << ",'" << time_buf << "')";
    if (mysql_query(conn, oss.str().c_str()) != 0) {
      std::cerr << "server_performance insert failed: " << mysql_error(conn)
                << std::endl;
      return false;
    }
  }

  // ========== 2. 写入网络详细表 server_net_detail ==========
  for (int i = 0; i < info.net_info_size(); ++i) {
    const auto& net = info.net_info(i);
    std::ostringstream oss;
    oss << "INSERT INTO server_net_detail "
        << "(server_name, net_name, rcv_bytes_rate, rcv_packets_rate, "
        << "snd_bytes_rate, snd_packets_rate, "
        << "timestamp) VALUES ('"
        << host_name << "','" << net.name() << "',"
        << net.rcv_rate() << "," << net.rcv_packets_rate() << ","
        << net.send_rate() << "," << net.send_packets_rate()
        << ",'" << time_buf << "')";
    if (mysql_query(conn, oss.str().c_str()) != 0) {
      std::cerr << "server_net_detail insert failed: " << mysql_error(conn)
                << std::endl;
      return false;
    }
  }

  // ========== 3. 写入磁盘详细表 server_disk_detail ==========
  for (int i = 0; i < info.disk_info_size(); ++i) {
    const auto& disk = info.disk_info(i);
    std::ostringstream oss;
    oss << "INSERT INTO server_disk_detail "
        << "(server_name, disk_name, read_bytes_per_sec, write_bytes_per_sec, "
        << "read_iops, write_iops, avg_read_latency_ms, avg_write_latency_ms, util_percent, "
        << "timestamp) VALUES ('"
        << host_name << "','" << disk.name() << "',"
        << disk.read_bytes_per_sec() << "," << disk.write_bytes_per_sec() << ","
        << disk.read_iops() << "," << disk.write_iops() << ","
        << disk.avg_read_latency_ms() << "," << disk.avg_write_latency_ms() << ","
        << disk.util_percent()
        << ",'" << time_buf << "')";
    if (mysql_query(conn, oss.str().c_str()) != 0) {
      std::cerr << "server_disk_detail insert failed: " << mysql_error(conn)
                << std::endl;
      return false;
    }
  }

  return true;
}

}  // namespace monitor
