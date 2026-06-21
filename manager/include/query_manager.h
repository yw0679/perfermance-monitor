/**
 * 文件归类：1
 * 说明：定义查询结果结构体以及 MySQL 查询管理器的对外接口。
 */

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <mysql/mysql.h>

namespace monitor {

enum class ServerStatus { ONLINE = 0, OFFLINE = 1 };

struct TimeRange {
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;
};

struct PerformanceRecord {
  std::string server_name;
  std::chrono::system_clock::time_point timestamp;

  float cpu_percent = 0;
  float usr_percent = 0;
  float system_percent = 0;
  float nice_percent = 0;
  float idle_percent = 0;
  float io_wait_percent = 0;
  float irq_percent = 0;
  float soft_irq_percent = 0;

  float load_avg_1 = 0;
  float load_avg_3 = 0;
  float load_avg_15 = 0;

  float mem_used_percent = 0;
  float mem_total = 0;
  float mem_free = 0;
  float mem_avail = 0;

  float disk_util_percent = 0;

  float send_rate = 0;
  float rcv_rate = 0;
  float send_packets_rate = 0;
  float rcv_packets_rate = 0;

  float score = 0;
  float cpu_percent_rate = 0;
  float mem_used_percent_rate = 0;
  float disk_util_percent_rate = 0;
  float load_avg_1_rate = 0;
  float send_rate_rate = 0;
  float rcv_rate_rate = 0;
};

struct ServerOverviewRecord {
  std::string server_name;
  float score = 0;
  std::chrono::system_clock::time_point last_update;
  ServerStatus status = ServerStatus::ONLINE;

  float cpu_percent = 0;
  float mem_used_percent = 0;
  float disk_util_percent = 0;
  float load_avg_1 = 0;
  float send_rate = 0;
  float rcv_rate = 0;
};

struct ClusterStats {
  int total_servers = 0;
  int online_servers = 0;
  int offline_servers = 0;
  float avg_score = 0;
  float max_score = 0;
  float min_score = 0;
  std::string best_server;
  std::string worst_server;
};

struct NetDetailRecord {
  std::string server_name;
  std::string net_name;
  std::chrono::system_clock::time_point timestamp;
  float rcv_bytes_rate = 0;
  float snd_bytes_rate = 0;
  float rcv_packets_rate = 0;
  float snd_packets_rate = 0;
};

struct DiskDetailRecord {
  std::string server_name;
  std::string disk_name;
  std::chrono::system_clock::time_point timestamp;
  float read_bytes_per_sec = 0;
  float write_bytes_per_sec = 0;
  float read_iops = 0;
  float write_iops = 0;
  float avg_read_latency_ms = 0;
  float avg_write_latency_ms = 0;
  float util_percent = 0;
};

class QueryManager {
 public:
  QueryManager();
  ~QueryManager();

  bool Init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database);
  void Close();
  bool IsInitialized();

  bool ValidateTimeRange(const TimeRange& range) const;

  std::vector<PerformanceRecord> QueryPerformance(const std::string& server_name,
                                                  const TimeRange& time_range,
                                                  int page, int page_size,
                                                  int* total_count);
  std::vector<NetDetailRecord> QueryNetDetail(const std::string& server_name,
                                              const TimeRange& time_range,
                                              int page, int page_size,
                                              int* total_count);
  std::vector<DiskDetailRecord> QueryDiskDetail(const std::string& server_name,
                                                const TimeRange& time_range,
                                                int page, int page_size,
                                                int* total_count);

 private:
  std::string FormatTime(const std::chrono::system_clock::time_point& tp) const;
  std::chrono::system_clock::time_point ParseTime(const char* str) const;

  MYSQL* conn_ = nullptr;
  std::mutex mtx_;
  bool initialized_ = false;
};

}  // namespace monitor
