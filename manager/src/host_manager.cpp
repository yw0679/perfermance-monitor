/**
 * 文件归类：1
 * 说明：实现主机数据处理、评分计算、过期清理和 MySQL 写入逻辑。
 *
 * MySQL 写入优化要点：
 *   - prepared statement 参数化查询，杜绝 SQL 注入
 *   - START TRANSACTION / COMMIT 包裹 3 表 INSERT，保证同一采样快照的原子性
 *   - 后台写线程复用预编译语句和 bind buffer，避免每次重新分配
 */

#include "host_manager.h"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <cstring>
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

// ============================================================
// Prepared Statement 上下文 — 连接存续期间复用 bind buffer
// ============================================================

struct StmtPerfCtx {
  MYSQL_STMT* stmt = nullptr;
  static constexpr int kNumParams = 29;

  char  srv_name[256] = {};
  unsigned long srv_name_len = 0;
  float fvals[27] = {};
  char  ts_buf[32] = {};
  unsigned long ts_len = 0;

  MYSQL_BIND bind[kNumParams] = {};

  bool Prepare(MYSQL* conn) {
    stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    const char* sql =
        "INSERT INTO server_performance "
        "(server_name, cpu_percent, usr_percent, system_percent, "
        "nice_percent, idle_percent, io_wait_percent, irq_percent, soft_irq_percent, "
        "load_avg_1, load_avg_3, load_avg_15, "
        "mem_used_percent, total, mem_free, avail, "
        "disk_util_percent, send_rate, rcv_rate, send_packets_rate, rcv_packets_rate, score, "
        "cpu_percent_rate, load_avg_1_rate, mem_used_percent_rate, "
        "disk_util_percent_rate, send_rate_rate, rcv_rate_rate, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
        "        ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
        "        ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
      std::cerr << "perf stmt prepare: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }

    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type   = MYSQL_TYPE_STRING;
    bind[0].buffer        = srv_name;
    bind[0].buffer_length = sizeof(srv_name);
    bind[0].length        = &srv_name_len;

    for (int i = 0; i < 27; ++i) {
      bind[1 + i].buffer_type = MYSQL_TYPE_FLOAT;
      bind[1 + i].buffer      = &fvals[i];
    }

    bind[28].buffer_type   = MYSQL_TYPE_STRING;
    bind[28].buffer        = ts_buf;
    bind[28].buffer_length = sizeof(ts_buf);
    bind[28].length        = &ts_len;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
      std::cerr << "perf stmt bind: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }
    return true;
  }

  bool Execute() { return mysql_stmt_execute(stmt) == 0; }

  void Close() {
    if (stmt) { mysql_stmt_close(stmt); stmt = nullptr; }
  }
};

struct StmtNetCtx {
  MYSQL_STMT* stmt = nullptr;
  static constexpr int kNumParams = 7;

  char  srv_name[256] = {};
  unsigned long srv_name_len = 0;
  char  net_name[64] = {};
  unsigned long net_name_len = 0;
  float fvals[4] = {};
  char  ts_buf[32] = {};
  unsigned long ts_len = 0;

  MYSQL_BIND bind[kNumParams] = {};

  bool Prepare(MYSQL* conn) {
    stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    const char* sql =
        "INSERT INTO server_net_detail "
        "(server_name, net_name, rcv_bytes_rate, rcv_packets_rate, "
        "snd_bytes_rate, snd_packets_rate, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
      std::cerr << "net stmt prepare: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }

    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type   = MYSQL_TYPE_STRING;
    bind[0].buffer        = srv_name;
    bind[0].buffer_length = sizeof(srv_name);
    bind[0].length        = &srv_name_len;

    bind[1].buffer_type   = MYSQL_TYPE_STRING;
    bind[1].buffer        = net_name;
    bind[1].buffer_length = sizeof(net_name);
    bind[1].length        = &net_name_len;

    for (int i = 0; i < 4; ++i) {
      bind[2 + i].buffer_type = MYSQL_TYPE_FLOAT;
      bind[2 + i].buffer      = &fvals[i];
    }

    bind[6].buffer_type   = MYSQL_TYPE_STRING;
    bind[6].buffer        = ts_buf;
    bind[6].buffer_length = sizeof(ts_buf);
    bind[6].length        = &ts_len;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
      std::cerr << "net stmt bind: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }
    return true;
  }

  bool Execute() { return mysql_stmt_execute(stmt) == 0; }

  void Close() {
    if (stmt) { mysql_stmt_close(stmt); stmt = nullptr; }
  }
};

struct StmtDiskCtx {
  MYSQL_STMT* stmt = nullptr;
  static constexpr int kNumParams = 10;

  char  srv_name[256] = {};
  unsigned long srv_name_len = 0;
  char  disk_name[64] = {};
  unsigned long disk_name_len = 0;
  float fvals[7] = {};
  char  ts_buf[32] = {};
  unsigned long ts_len = 0;

  MYSQL_BIND bind[kNumParams] = {};

  bool Prepare(MYSQL* conn) {
    stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    const char* sql =
        "INSERT INTO server_disk_detail "
        "(server_name, disk_name, read_bytes_per_sec, write_bytes_per_sec, "
        "read_iops, write_iops, avg_read_latency_ms, avg_write_latency_ms, util_percent, "
        "timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
      std::cerr << "disk stmt prepare: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }

    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type   = MYSQL_TYPE_STRING;
    bind[0].buffer        = srv_name;
    bind[0].buffer_length = sizeof(srv_name);
    bind[0].length        = &srv_name_len;

    bind[1].buffer_type   = MYSQL_TYPE_STRING;
    bind[1].buffer        = disk_name;
    bind[1].buffer_length = sizeof(disk_name);
    bind[1].length        = &disk_name_len;

    for (int i = 0; i < 7; ++i) {
      bind[2 + i].buffer_type = MYSQL_TYPE_FLOAT;
      bind[2 + i].buffer      = &fvals[i];
    }

    bind[9].buffer_type   = MYSQL_TYPE_STRING;
    bind[9].buffer        = ts_buf;
    bind[9].buffer_length = sizeof(ts_buf);
    bind[9].length        = &ts_len;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
      std::cerr << "disk stmt bind: " << mysql_stmt_error(stmt) << std::endl;
      return false;
    }
    return true;
  }

  bool Execute() { return mysql_stmt_execute(stmt) == 0; }

  void Close() {
    if (stmt) { mysql_stmt_close(stmt); stmt = nullptr; }
  }
};

static void FormatTimestamp(
    const std::chrono::system_clock::time_point& tp,
    char* buf, size_t buf_size, unsigned long* out_len) {
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm_time;
  localtime_r(&t, &tm_time);
  int n = std::strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &tm_time);
  *out_len = static_cast<unsigned long>(n);
}

// 构造函数只初始化运行标记。
HostManager::HostManager() : running_(false) {}

HostManager::~HostManager() { Stop(); }

void HostManager::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }

  cleanup_thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
  mysql_thread_ = std::make_unique<std::thread>(&HostManager::MysqlWriteLoop, this);
}

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
// 连接存续期间复用 3 个 prepared statement；连接断开后全部重建。
void HostManager::MysqlWriteLoop() {
  MYSQL* conn = nullptr;
  StmtPerfCtx stmt_perf;
  StmtNetCtx  stmt_net;
  StmtDiskCtx stmt_disk;

  auto connect_and_prepare = [&]() -> bool {
    if (conn) {
      stmt_perf.Close();
      stmt_net.Close();
      stmt_disk.Close();
      mysql_close(conn);
      conn = nullptr;
    }

    conn = mysql_init(nullptr);
    if (!conn) {
      std::cerr << "mysql_init failed in MysqlWriteLoop" << std::endl;
      return false;
    }

    if (!mysql_real_connect(conn, MYSQL_HOST, MYSQL_USER, MYSQL_PASS,
                            MYSQL_DB, 0, nullptr, 0)) {
      std::cerr << "mysql_real_connect failed: " << mysql_error(conn)
                << std::endl;
      mysql_close(conn);
      conn = nullptr;
      return false;
    }

    mysql_set_character_set(conn, "utf8mb4");

    if (!stmt_perf.Prepare(conn) ||
        !stmt_net.Prepare(conn) ||
        !stmt_disk.Prepare(conn)) {
      stmt_perf.Close();
      stmt_net.Close();
      stmt_disk.Close();
      mysql_close(conn);
      conn = nullptr;
      return false;
    }

    return true;
  };

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
      if (!connect_and_prepare()) {
        continue;
      }
    }

    const bool write_ok = WriteToMysql(
        &stmt_perf, &stmt_net, &stmt_disk,
        task.host_name, task.host_score,
        task.cpu_percent_rate, task.load_avg_1_rate,
        task.mem_used_percent_rate, task.disk_util_percent_rate,
        task.net_in_rate_rate, task.net_out_rate_rate);

    if (!write_ok) {
      std::cerr << "MySQL write failed for host: " << task.host_name
                << ", reconnecting..." << std::endl;
      stmt_perf.Close();
      stmt_net.Close();
      stmt_disk.Close();
      mysql_close(conn);
      conn = nullptr;
    }
  }

  if (conn) {
    stmt_perf.Close();
    stmt_net.Close();
    stmt_disk.Close();
    mysql_close(conn);
  }
}

void HostManager::EnqueueMysqlWrite(MysqlWriteTask task) {
  {
    std::lock_guard<std::mutex> lock(mysql_queue_mtx_);
    mysql_write_queue_.push_back(std::move(task));
  }
  mysql_queue_cv_.notify_one();
}

void HostManager::OnDataReceived(const monitor::proto::MonitorInfo& info) {
  std::string host_name;
  if (info.has_host_info()) {
    const auto& host_info = info.host_info();
    std::string hostname = host_info.hostname();
    std::string ip = host_info.ip_address();

    if (!hostname.empty() && !ip.empty()) {
      host_name = hostname + "_" + ip;
    } else if (!hostname.empty()) {
      host_name = hostname;
    } else if (!ip.empty()) {
      host_name = ip;
    }
  }

  if (host_name.empty()) {
    host_name = info.name();
  }

  if (host_name.empty()) {
    std::cerr << "Received data with empty server identifier" << std::endl;
    return;
  }

  double score = CalcScore(info);
  auto now = std::chrono::system_clock::now();

  double net_in_rate = 0, net_out_rate = 0;
  for (int i = 0; i < info.net_info_size(); ++i) {
    net_in_rate += info.net_info(i).rcv_rate();
    net_out_rate += info.net_info(i).send_rate();
  }

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

  std::cout << "\n--- Memory ---" << std::endl;
  std::cout << "  Used: " << curr.mem_used_percent << "%, "
            << "Total: " << curr.mem_total << " GB" << std::endl;
  std::cout << "  Free: " << curr.mem_free << " GB, "
            << "Avail: " << curr.mem_avail << " GB" << std::endl;

  std::cout << "\n--- Network ---" << std::endl;
  std::cout << "  In: " << net_in_rate << " KB/s, "
            << "Out: " << net_out_rate << " KB/s" << std::endl;
  for (int i = 0; i < info.net_info_size(); ++i) {
    const auto& net = info.net_info(i);
    std::cout << "  [" << net.name() << "] Recv: " << net.rcv_rate() << " KB/s, "
              << "Send: " << net.send_rate() << " KB/s" << std::endl;
  }

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
  const double cpu_weight = 0.35;
  const double mem_weight = 0.30;
  const double load_weight = 0.15;
  const double disk_weight = 0.15;
  const double net_weight = 0.05;

  const double load_coefficient = 1.5;
  const double max_bandwidth = 125000000.0;

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

// 使用预编译语句 + 事务完成一次完整落库。
// 同一采样快照的 3 表数据要么全部写入，要么全部回滚。
bool HostManager::WriteToMysql(
    StmtPerfCtx* stmt_perf, StmtNetCtx* stmt_net, StmtDiskCtx* stmt_disk,
    const std::string& host_name, const HostScore& host_score,
    float cpu_percent_rate, float load_avg_1_rate,
    float mem_used_percent_rate, float disk_util_percent_rate,
    float net_in_rate_rate, float net_out_rate_rate) {

  if (!stmt_perf || !stmt_perf->stmt ||
      !stmt_net  || !stmt_net->stmt ||
      !stmt_disk || !stmt_disk->stmt) {
    return false;
  }

  const auto& info = host_score.info;

  // 时间戳格式化（3 个 stmt 共用）
  char time_buf[32];
  unsigned long time_len;
  FormatTimestamp(host_score.timestamp, time_buf, sizeof(time_buf), &time_len);

  // ========== 填充 server_name（3 个 stmt 共用） ==========
  auto set_server_name = [&](char* buf, unsigned long* len) {
    std::memcpy(buf, host_name.c_str(),
                std::min(host_name.size(), (size_t)255));
    *len = static_cast<unsigned long>(
        std::min(host_name.size(), (size_t)255));
  };
  set_server_name(stmt_perf->srv_name, &stmt_perf->srv_name_len);
  set_server_name(stmt_net->srv_name, &stmt_net->srv_name_len);
  set_server_name(stmt_disk->srv_name, &stmt_disk->srv_name_len);

  // 时间戳（3 个 stmt 共用）
  std::memcpy(stmt_perf->ts_buf, time_buf, time_len);
  stmt_perf->ts_len = time_len;
  std::memcpy(stmt_net->ts_buf, time_buf, time_len);
  stmt_net->ts_len = time_len;
  std::memcpy(stmt_disk->ts_buf, time_buf, time_len);
  stmt_disk->ts_len = time_len;

  // ========== 提取公共字段 ==========
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

  // ========== 填充 server_performance 的 fvals ==========
  // fvals 索引对齐 VALUES 子句中的 27 个 FLOAT 列:
  //   0:cpu_percent 1:usr_percent 2:system_percent 3:nice_percent
  //   4:idle_percent 5:io_wait_percent 6:irq_percent 7:soft_irq_percent
  //   8:load_avg_1 9:load_avg_3 10:load_avg_15
  //   11:mem_used_percent 12:total 13:mem_free 14:avail
  //   15:disk_util_percent 16:send_rate 17:rcv_rate
  //   18:send_packets_rate 19:rcv_packets_rate 20:score
  //   21:cpu_percent_rate 22:load_avg_1_rate 23:mem_used_percent_rate
  //   24:disk_util_percent_rate 25:send_rate_rate 26:rcv_rate_rate
  float* p = stmt_perf->fvals;
  p[0]  = cpu_percent;        p[1]  = usr_percent;
  p[2]  = system_percent;     p[3]  = nice_percent;
  p[4]  = idle_percent;       p[5]  = io_wait_percent;
  p[6]  = irq_percent;        p[7]  = soft_irq_percent;
  p[8]  = load_avg_1;         p[9]  = load_avg_3;
  p[10] = load_avg_15;        p[11] = mem_used_percent;
  p[12] = total;              p[13] = free;
  p[14] = avail;              p[15] = disk_util_percent;
  p[16] = send_rate;          p[17] = rcv_rate;
  p[18] = send_packets_rate;  p[19] = rcv_packets_rate;
  p[20] = static_cast<float>(host_score.score);
  p[21] = cpu_percent_rate;   p[22] = load_avg_1_rate;
  p[23] = mem_used_percent_rate; p[24] = disk_util_percent_rate;
  p[25] = net_in_rate_rate;   p[26] = net_out_rate_rate;

  // ========== 事务开始 ==========
  // 使用 mysql_query 发送事务控制语句（与 prepared statement 共用同一连接）
  if (mysql_query(stmt_perf->stmt->mysql, "START TRANSACTION") != 0) {
    std::cerr << "START TRANSACTION failed: "
              << mysql_error(stmt_perf->stmt->mysql) << std::endl;
    return false;
  }

  // ========== 1. 写入主表 ==========
  if (!stmt_perf->Execute()) {
    std::cerr << "perf insert: " << mysql_stmt_error(stmt_perf->stmt) << std::endl;
    mysql_query(stmt_perf->stmt->mysql, "ROLLBACK");
    return false;
  }

  // ========== 2. 写入网络明细 ==========
  for (int i = 0; i < info.net_info_size(); ++i) {
    const auto& net = info.net_info(i);
    const auto& name = net.name();
    std::memcpy(stmt_net->net_name, name.c_str(),
                std::min(name.size(), (size_t)63));
    stmt_net->net_name_len = static_cast<unsigned long>(
        std::min(name.size(), (size_t)63));

    stmt_net->fvals[0] = net.rcv_rate();
    stmt_net->fvals[1] = net.rcv_packets_rate();
    stmt_net->fvals[2] = net.send_rate();
    stmt_net->fvals[3] = net.send_packets_rate();

    if (!stmt_net->Execute()) {
      std::cerr << "net insert: " << mysql_stmt_error(stmt_net->stmt) << std::endl;
      mysql_query(stmt_net->stmt->mysql, "ROLLBACK");
      return false;
    }
  }

  // ========== 3. 写入磁盘明细 ==========
  for (int i = 0; i < info.disk_info_size(); ++i) {
    const auto& disk = info.disk_info(i);
    const auto& name = disk.name();
    std::memcpy(stmt_disk->disk_name, name.c_str(),
                std::min(name.size(), (size_t)63));
    stmt_disk->disk_name_len = static_cast<unsigned long>(
        std::min(name.size(), (size_t)63));

    stmt_disk->fvals[0] = disk.read_bytes_per_sec();
    stmt_disk->fvals[1] = disk.write_bytes_per_sec();
    stmt_disk->fvals[2] = disk.read_iops();
    stmt_disk->fvals[3] = disk.write_iops();
    stmt_disk->fvals[4] = disk.avg_read_latency_ms();
    stmt_disk->fvals[5] = disk.avg_write_latency_ms();
    stmt_disk->fvals[6] = disk.util_percent();

    if (!stmt_disk->Execute()) {
      std::cerr << "disk insert: " << mysql_stmt_error(stmt_disk->stmt) << std::endl;
      mysql_query(stmt_disk->stmt->mysql, "ROLLBACK");
      return false;
    }
  }

  // ========== 事务提交 ==========
  if (mysql_query(stmt_perf->stmt->mysql, "COMMIT") != 0) {
    std::cerr << "COMMIT failed: "
              << mysql_error(stmt_perf->stmt->mysql) << std::endl;
    mysql_query(stmt_perf->stmt->mysql, "ROLLBACK");
    return false;
  }

  return true;
}

}  // namespace monitor
