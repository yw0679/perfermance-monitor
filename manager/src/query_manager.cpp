/**
 * 文件归类：1
 * 说明：实现 MySQL 查询、结果解析和时间转换等数据库访问逻辑。
 */

#include "query_manager.h"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace monitor {

namespace {

std::chrono::system_clock::time_point ParseMysqlTime(const char* str) {
  std::tm tm = {};
  std::istringstream ss(str ? str : "");
  ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

const char kPerformanceSelectColumns[] =
    "server_name, timestamp, cpu_percent, usr_percent, system_percent, "
    "nice_percent, idle_percent, io_wait_percent, irq_percent, "
    "soft_irq_percent, load_avg_1, load_avg_3, load_avg_15, "
    "mem_used_percent, total, mem_free, avail, disk_util_percent, "
    "send_rate, rcv_rate, send_packets_rate, rcv_packets_rate, score, "
    "cpu_percent_rate, mem_used_percent_rate, disk_util_percent_rate, "
    "load_avg_1_rate, send_rate_rate, rcv_rate_rate";

PerformanceRecord ParsePerformanceRecordRow(MYSQL_ROW row) {
  PerformanceRecord rec;
  int i = 0;
  rec.server_name = row[i++] ? row[i - 1] : "";
  rec.timestamp =
      row[i] ? ParseMysqlTime(row[i]) : std::chrono::system_clock::now();
  i++;
  rec.cpu_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.usr_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.system_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.nice_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.idle_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.io_wait_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.irq_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.soft_irq_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.load_avg_1 = row[i] ? std::atof(row[i]) : 0; i++;
  rec.load_avg_3 = row[i] ? std::atof(row[i]) : 0; i++;
  rec.load_avg_15 = row[i] ? std::atof(row[i]) : 0; i++;
  rec.mem_used_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.mem_total = row[i] ? std::atof(row[i]) : 0; i++;
  rec.mem_free = row[i] ? std::atof(row[i]) : 0; i++;
  rec.mem_avail = row[i] ? std::atof(row[i]) : 0; i++;
  rec.disk_util_percent = row[i] ? std::atof(row[i]) : 0; i++;
  rec.send_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.rcv_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.send_packets_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.rcv_packets_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.score = row[i] ? std::atof(row[i]) : 0; i++;
  rec.cpu_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.mem_used_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.disk_util_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.load_avg_1_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.send_rate_rate = row[i] ? std::atof(row[i]) : 0; i++;
  rec.rcv_rate_rate = row[i] ? std::atof(row[i]) : 0;
  return rec;
}

}  // namespace

QueryManager::QueryManager() = default;

QueryManager::~QueryManager() { Close(); }

bool QueryManager::Init(const std::string& host, const std::string& user,
                        const std::string& password,
                        const std::string& database) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (initialized_) {
    return true;
  }

  conn_ = mysql_init(nullptr);
  if (!conn_) {
    std::cerr << "QueryManager: mysql_init failed" << std::endl;
    return false;
  }

  if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(),
                          database.c_str(), 0, nullptr, 0)) {
    std::cerr << "QueryManager: mysql_real_connect failed: "
              << mysql_error(conn_) << std::endl;
    mysql_close(conn_);
    conn_ = nullptr;
    return false;
  }

  mysql_set_character_set(conn_, "utf8mb4");
  initialized_ = true;
  std::cout << "QueryManager: MySQL connection initialized" << std::endl;
  return true;
}

void QueryManager::Close() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  initialized_ = false;
}

bool QueryManager::IsInitialized() {
  std::lock_guard<std::mutex> lock(mtx_);
  return initialized_;
}

bool QueryManager::ValidateTimeRange(const TimeRange& range) const {
  return range.start_time <= range.end_time;
}

std::string QueryManager::FormatTime(
    const std::chrono::system_clock::time_point& tp) const {
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm_time;
  localtime_r(&t, &tm_time);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_time);
  return std::string(buf);
}

std::chrono::system_clock::time_point QueryManager::ParseTime(
    const char* str) const {
  return ParseMysqlTime(str);
}

int QueryManager::GetTotalCount(const std::string& count_sql) {
  if (mysql_query(conn_, count_sql.c_str()) != 0) {
    std::cerr << "QueryManager: count query failed: " << mysql_error(conn_)
              << std::endl;
    return 0;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return 0;
  }

  int count = 0;
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row && row[0]) {
    count = std::atoi(row[0]);
  }
  mysql_free_result(result);
  return count;
}

std::vector<PerformanceRecord> QueryManager::QueryPerformance(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<PerformanceRecord> records;

  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }
  if (!ValidateTimeRange(time_range)) {
    std::cerr << "QueryManager: Invalid time range" << std::endl;
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  const std::string start_time = FormatTime(time_range.start_time);
  const std::string end_time = FormatTime(time_range.end_time);

  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_performance WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  const int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT " << kPerformanceSelectColumns
      << " FROM server_performance WHERE server_name='" << server_name
      << "' AND timestamp BETWEEN '" << start_time << "' AND '" << end_time
      << "' ORDER BY timestamp DESC LIMIT " << page_size << " OFFSET "
      << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: performance query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    records.push_back(ParsePerformanceRecordRow(row));
  }
  mysql_free_result(result);

  return records;
}

std::vector<NetDetailRecord> QueryManager::QueryNetDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<NetDetailRecord> records;

  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }
  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  const std::string start_time = FormatTime(time_range.start_time);
  const std::string end_time = FormatTime(time_range.end_time);

  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_net_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  const int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, net_name, timestamp, rcv_bytes_rate, "
         "snd_bytes_rate, rcv_packets_rate, snd_packets_rate "
         "FROM server_net_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: net detail query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    NetDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.net_name = row[i++] ? row[i - 1] : "";
    rec.timestamp =
        row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.rcv_bytes_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.snd_bytes_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.rcv_packets_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.snd_packets_rate = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);

  return records;
}

std::vector<DiskDetailRecord> QueryManager::QueryDiskDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<DiskDetailRecord> records;

  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }
  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  const std::string start_time = FormatTime(time_range.start_time);
  const std::string end_time = FormatTime(time_range.end_time);

  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_disk_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  const int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, disk_name, timestamp, read_bytes_per_sec, "
         "write_bytes_per_sec, read_iops, write_iops, avg_read_latency_ms, "
         "avg_write_latency_ms, util_percent "
         "FROM server_disk_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: disk detail query failed: "
              << mysql_error(conn_) << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    DiskDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.disk_name = row[i++] ? row[i - 1] : "";
    rec.timestamp =
        row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.read_bytes_per_sec = row[i] ? std::atof(row[i]) : 0; i++;
    rec.write_bytes_per_sec = row[i] ? std::atof(row[i]) : 0; i++;
    rec.read_iops = row[i] ? std::atof(row[i]) : 0; i++;
    rec.write_iops = row[i] ? std::atof(row[i]) : 0; i++;
    rec.avg_read_latency_ms = row[i] ? std::atof(row[i]) : 0; i++;
    rec.avg_write_latency_ms = row[i] ? std::atof(row[i]) : 0; i++;
    rec.util_percent = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);

  return records;
}

}  // namespace monitor
