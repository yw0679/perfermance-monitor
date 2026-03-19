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

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

namespace {
constexpr double kScheduleWeightSmoothing = 0.3;
}  // namespace

#ifdef ENABLE_MYSQL
namespace {
const char* MYSQL_HOST = "127.0.0.1";
const char* MYSQL_USER = "monitor";
const char* MYSQL_PASS = "monitor123";
const char* MYSQL_DB = "monitor_db";

}  // namespace
#endif

// 用于网络速率计算的采样数据
struct NetSample {
  double last_in_bytes = 0;
  double last_out_bytes = 0;
  std::chrono::system_clock::time_point last_time;
};
static std::map<std::string, NetSample> net_samples;

// 用于变化率计算的性能采样数据
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

HostManager::HostManager() : running_(false) {
}

HostManager::~HostManager() {
  Stop();
}

void HostManager::Start() {
  running_ = true;
  thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
}

void HostManager::Stop() {
  running_ = false;
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void HostManager::ProcessLoop() {
  while (running_) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
    
    auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = host_scores_.begin(); it != host_scores_.end();) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          now - it->second.timestamp).count();
      if (age > 60) {
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

  // 网络速率计算
  double net_in_rate = 0, net_out_rate = 0;
  if (info.net_info_size() > 0) {
    net_in_rate = info.net_info(0).rcv_rate();
    net_out_rate = info.net_info(0).send_rate();
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

  // 写入所有表
  bool mysql_write_ok = WriteToMysql(
      host_name, host_score, cpu_percent_rate, load_avg_1_rate,
      mem_used_percent_rate, disk_util_percent_rate, net_in_rate_rate,
      net_out_rate_rate);

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
  std::cout << "  MySQL write: " << (mysql_write_ok ? "success" : "skipped/failed")
            << std::endl;
  std::cout << "====================================================\n" << std::endl;
}

std::unordered_map<std::string, HostScore> HostManager::GetAllHostScores() {
  std::lock_guard<std::mutex> lock(mtx_);
  return host_scores_;
}

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

double HostManager::CalcSchedulingWeight(double score) const {
  return score > 0 ? score : 0;
}

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

bool HostManager::WriteToMysql(
    const std::string& host_name, const HostScore& host_score,
    float cpu_percent_rate, float load_avg_1_rate,
    float mem_used_percent_rate, float disk_util_percent_rate,
    float net_in_rate_rate, float net_out_rate_rate) {
#ifdef ENABLE_MYSQL
  MYSQL* conn = mysql_init(NULL);
  if (!conn) {
    std::cerr << "mysql_init failed\n";
    return false;
  }
  if (!mysql_real_connect(conn, MYSQL_HOST, MYSQL_USER, MYSQL_PASS, MYSQL_DB, 0,
                          NULL, 0)) {
    std::cerr << "mysql_real_connect failed: " << mysql_error(conn) << "\n";
    mysql_close(conn);
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
    float total = 0, avail = 0, send_rate = 0, rcv_rate = 0;
    float cpu_percent = 0, usr_percent = 0, system_percent = 0;
    float load_avg_1 = 0, load_avg_3 = 0, load_avg_15 = 0, mem_used_percent = 0;
    float disk_util_percent = 0;

    if (info.has_mem_info()) {
      total = info.mem_info().total();
      avail = info.mem_info().avail();
      mem_used_percent = info.mem_info().used_percent();
    }
    if (info.net_info_size() > 0) {
      send_rate = info.net_info(0).send_rate();
      rcv_rate = info.net_info(0).rcv_rate();
    }
    if (info.cpu_stat_size() > 0) {
      const auto& cpu = info.cpu_stat(0);
      cpu_percent = cpu.cpu_percent();
      usr_percent = cpu.usr_percent();
      system_percent = cpu.system_percent();
    }
    if (info.has_cpu_load()) {
      load_avg_1 = info.cpu_load().load_avg_1();
      load_avg_3 = info.cpu_load().load_avg_3();
      load_avg_15 = info.cpu_load().load_avg_15();
    }
    // 获取磁盘利用率最大值（用于评分）
    for (int i = 0; i < info.disk_info_size(); ++i) {
      float util = info.disk_info(i).util_percent();
      if (util > disk_util_percent) disk_util_percent = util;
    }

    std::ostringstream oss;
    oss << "INSERT INTO server_performance "
        << "(server_name, cpu_percent, usr_percent, system_percent, "
        << "load_avg_1, load_avg_3, load_avg_15, "
        << "mem_used_percent, total, avail, "
        << "disk_util_percent, send_rate, rcv_rate, score, "
        << "cpu_percent_rate, load_avg_1_rate, mem_used_percent_rate, "
        << "disk_util_percent_rate, send_rate_rate, rcv_rate_rate, timestamp) VALUES ('"
        << host_name << "',"
        << cpu_percent << "," << usr_percent << "," << system_percent << ","
        << load_avg_1 << "," << load_avg_3 << "," << load_avg_15 << ","
        << mem_used_percent << "," << total << "," << avail << ","
        << disk_util_percent << "," << send_rate << "," << rcv_rate << "," << host_score.score << ","
        << cpu_percent_rate << "," << load_avg_1_rate << ","
        << mem_used_percent_rate << "," << disk_util_percent_rate << ","
        << net_in_rate_rate << "," << net_out_rate_rate
        << ",'" << time_buf << "')";
    mysql_query(conn, oss.str().c_str());
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
    mysql_query(conn, oss.str().c_str());
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
    mysql_query(conn, oss.str().c_str());
  }

  mysql_close(conn);
  return true;
#else
  (void)host_name;
  (void)host_score;
  (void)cpu_percent_rate;
  (void)load_avg_1_rate;
  (void)mem_used_percent_rate;
  (void)disk_util_percent_rate;
  (void)net_in_rate_rate;
  (void)net_out_rate_rate;
  return false;
#endif
}

}  // namespace monitor
