/**
 * 文件归类：1
 * 说明：实现查询 gRPC 服务，当前态查询走内存，历史查询走 QueryManager/MySQL。
 */

#include "rpc/query_service.h"

#include <algorithm>
#include <limits>

#include "host_manager.h"

namespace monitor {

namespace {

constexpr auto kHostOnlineWindow = std::chrono::seconds(60);

void FillClusterStats(const ClusterStats& stats,
                      ::monitor::proto::ClusterStats* proto_stats) {
  proto_stats->set_total_servers(stats.total_servers);
  proto_stats->set_online_servers(stats.online_servers);
  proto_stats->set_offline_servers(stats.offline_servers);
  proto_stats->set_avg_score(stats.avg_score);
  proto_stats->set_max_score(stats.max_score);
  proto_stats->set_min_score(stats.min_score);
  proto_stats->set_best_server(stats.best_server);
  proto_stats->set_worst_server(stats.worst_server);
}

template <typename SetTimestampFn>
void FillServerOverview(const ServerOverviewRecord& rec,
                        ::monitor::proto::ServerOverview* proto_rec,
                        SetTimestampFn&& set_ts) {
  proto_rec->set_server_name(rec.server_name);
  proto_rec->set_score(rec.score);
  set_ts(proto_rec->mutable_last_update(), rec.last_update);
  proto_rec->set_status(rec.status == ServerStatus::ONLINE
                            ? ::monitor::proto::ONLINE
                            : ::monitor::proto::OFFLINE);
  proto_rec->set_cpu_percent(rec.cpu_percent);
  proto_rec->set_mem_used_percent(rec.mem_used_percent);
  proto_rec->set_disk_util_percent(rec.disk_util_percent);
  proto_rec->set_load_avg_1(rec.load_avg_1);
  proto_rec->set_send_rate(rec.send_rate);
  proto_rec->set_rcv_rate(rec.rcv_rate);
}

template <typename SetTimestampFn>
void FillPerformanceRecord(const PerformanceRecord& rec,
                           ::monitor::proto::PerformanceRecord* proto_rec,
                           SetTimestampFn&& set_ts) {
  proto_rec->set_server_name(rec.server_name);
  set_ts(proto_rec->mutable_timestamp(), rec.timestamp);
  proto_rec->set_cpu_percent(rec.cpu_percent);
  proto_rec->set_usr_percent(rec.usr_percent);
  proto_rec->set_system_percent(rec.system_percent);
  proto_rec->set_nice_percent(rec.nice_percent);
  proto_rec->set_idle_percent(rec.idle_percent);
  proto_rec->set_io_wait_percent(rec.io_wait_percent);
  proto_rec->set_irq_percent(rec.irq_percent);
  proto_rec->set_soft_irq_percent(rec.soft_irq_percent);
  proto_rec->set_load_avg_1(rec.load_avg_1);
  proto_rec->set_load_avg_3(rec.load_avg_3);
  proto_rec->set_load_avg_15(rec.load_avg_15);
  proto_rec->set_mem_used_percent(rec.mem_used_percent);
  proto_rec->set_mem_total(rec.mem_total);
  proto_rec->set_mem_free(rec.mem_free);
  proto_rec->set_mem_avail(rec.mem_avail);
  proto_rec->set_disk_util_percent(rec.disk_util_percent);
  proto_rec->set_send_rate(rec.send_rate);
  proto_rec->set_rcv_rate(rec.rcv_rate);
  proto_rec->set_send_packets_rate(rec.send_packets_rate);
  proto_rec->set_rcv_packets_rate(rec.rcv_packets_rate);
  proto_rec->set_score(rec.score);
  proto_rec->set_cpu_percent_rate(rec.cpu_percent_rate);
  proto_rec->set_load_avg_1_rate(rec.load_avg_1_rate);
  proto_rec->set_mem_used_percent_rate(rec.mem_used_percent_rate);
  proto_rec->set_disk_util_percent_rate(rec.disk_util_percent_rate);
  proto_rec->set_send_rate_rate(rec.send_rate_rate);
  proto_rec->set_rcv_rate_rate(rec.rcv_rate_rate);
}

float GetMaxDiskUtilPercent(const monitor::proto::MonitorInfo& info) {
  float disk_util_percent = 0;
  for (int i = 0; i < info.disk_info_size(); ++i) {
    disk_util_percent = std::max(
        disk_util_percent,
        static_cast<float>(info.disk_info(i).util_percent()));
  }
  return disk_util_percent;
}

float GetTotalSendRate(const monitor::proto::MonitorInfo& info) {
  float send_rate = 0;
  for (int i = 0; i < info.net_info_size(); ++i) {
    send_rate += info.net_info(i).send_rate();
  }
  return send_rate;
}

float GetTotalRcvRate(const monitor::proto::MonitorInfo& info) {
  float rcv_rate = 0;
  for (int i = 0; i < info.net_info_size(); ++i) {
    rcv_rate += info.net_info(i).rcv_rate();
  }
  return rcv_rate;
}

ServerOverviewRecord BuildServerOverviewRecord(
    const std::string& server_name, const HostScore& host_score,
    const std::chrono::system_clock::time_point& now) {
  ServerOverviewRecord rec;
  rec.server_name = server_name;
  rec.score = static_cast<float>(host_score.score);
  rec.last_update = host_score.timestamp;

  const auto& info = host_score.info;
  if (info.cpu_stat_size() > 0) {
    rec.cpu_percent = info.cpu_stat(0).cpu_percent();
  }
  if (info.has_mem_info()) {
    rec.mem_used_percent = info.mem_info().used_percent();
  }
  if (info.has_cpu_load()) {
    rec.load_avg_1 = info.cpu_load().load_avg_1();
  }
  rec.disk_util_percent = GetMaxDiskUtilPercent(info);
  rec.send_rate = GetTotalSendRate(info);
  rec.rcv_rate = GetTotalRcvRate(info);

  const auto age =
      std::chrono::duration_cast<std::chrono::seconds>(now - rec.last_update);
  rec.status =
      age > kHostOnlineWindow ? ServerStatus::OFFLINE : ServerStatus::ONLINE;
  return rec;
}

struct CurrentClusterSnapshot {
  std::vector<ServerOverviewRecord> records;
  ClusterStats stats;
};

CurrentClusterSnapshot BuildCurrentClusterSnapshot(
    const std::unordered_map<std::string, HostScore>& host_scores) {
  CurrentClusterSnapshot snapshot;
  snapshot.records.reserve(host_scores.size());

  const auto now = std::chrono::system_clock::now();
  for (const auto& [server_name, host_score] : host_scores) {
    snapshot.records.push_back(
        BuildServerOverviewRecord(server_name, host_score, now));
  }

  std::sort(snapshot.records.begin(), snapshot.records.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
              }
              if (lhs.last_update != rhs.last_update) {
                return lhs.last_update > rhs.last_update;
              }
              return lhs.server_name < rhs.server_name;
            });

  snapshot.stats.total_servers = static_cast<int>(snapshot.records.size());
  if (snapshot.records.empty()) {
    return snapshot;
  }

  float total_score = 0;
  float max_score = std::numeric_limits<float>::lowest();
  float min_score = std::numeric_limits<float>::max();
  float best_online_score = std::numeric_limits<float>::lowest();
  float worst_online_score = std::numeric_limits<float>::max();
  std::string best_server;
  std::string worst_server;
  std::string best_online_server;
  std::string worst_online_server;

  for (const auto& rec : snapshot.records) {
    total_score += rec.score;

    if (rec.status == ServerStatus::ONLINE) {
      ++snapshot.stats.online_servers;
      if (rec.score > best_online_score) {
        best_online_score = rec.score;
        best_online_server = rec.server_name;
      }
      if (rec.score < worst_online_score) {
        worst_online_score = rec.score;
        worst_online_server = rec.server_name;
      }
    } else {
      ++snapshot.stats.offline_servers;
    }

    if (rec.score > max_score) {
      max_score = rec.score;
      best_server = rec.server_name;
    }
    if (rec.score < min_score) {
      min_score = rec.score;
      worst_server = rec.server_name;
    }
  }

  snapshot.stats.avg_score =
      total_score / static_cast<float>(snapshot.stats.total_servers);
  snapshot.stats.max_score = max_score;
  snapshot.stats.min_score = min_score;
  snapshot.stats.best_server =
      best_online_server.empty() ? best_server : best_online_server;
  snapshot.stats.worst_server =
      worst_online_server.empty() ? worst_server : worst_online_server;
  return snapshot;
}

::grpc::Status ValidateHistoricalQueryBackend(QueryManager* query_manager) {
  if (!query_manager) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Historical query backend not initialized");
  }
  if (!query_manager->IsInitialized()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Historical query backend is unavailable");
  }
  return grpc::Status::OK;
}

}  // namespace

QueryServiceImpl::QueryServiceImpl(HostManager* host_manager,
                                   QueryManager* query_manager)
    : host_manager_(host_manager), query_manager_(query_manager) {}

TimeRange QueryServiceImpl::ConvertTimeRange(
    const ::monitor::proto::TimeRange& proto_range) {
  TimeRange range;
  range.start_time = std::chrono::system_clock::from_time_t(
      proto_range.start_time().seconds());
  range.end_time = std::chrono::system_clock::from_time_t(
      proto_range.end_time().seconds());
  return range;
}

void QueryServiceImpl::SetTimestamp(
    ::google::protobuf::Timestamp* ts,
    const std::chrono::system_clock::time_point& tp) {
  auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch())
          .count();
  ts->set_seconds(seconds);
  ts->set_nanos(0);
}

::grpc::Status QueryServiceImpl::QueryBestHost(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryBestHostRequest* request,
    ::monitor::proto::QueryBestHostResponse* response) {
  (void)context;
  (void)request;

  if (!host_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Host manager not initialized");
  }

  const std::string best_host = host_manager_->GetBestHost();
  if (best_host.empty()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No hosts available");
  }

  const auto host_scores = host_manager_->GetAllHostScores();
  const auto it = host_scores.find(best_host);
  if (it == host_scores.end()) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "Selected best host not found in host score set");
  }

  const auto now = std::chrono::system_clock::now();
  const auto best_rec = BuildServerOverviewRecord(best_host, it->second, now);
  FillServerOverview(best_rec, response->mutable_best_server(),
                     [this](auto* ts, const auto& tp) { SetTimestamp(ts, tp); });

  const auto snapshot = BuildCurrentClusterSnapshot(host_scores);
  FillClusterStats(snapshot.stats, response->mutable_cluster_stats());
  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryClusterOverview(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryClusterOverviewRequest* request,
    ::monitor::proto::QueryClusterOverviewResponse* response) {
  (void)context;
  (void)request;

  if (!host_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Host manager not initialized");
  }

  const auto snapshot =
      BuildCurrentClusterSnapshot(host_manager_->GetAllHostScores());
  for (const auto& rec : snapshot.records) {
    FillServerOverview(rec, response->add_servers(),
                       [this](auto* ts, const auto& tp) { SetTimestamp(ts, tp); });
  }
  FillClusterStats(snapshot.stats, response->mutable_cluster_stats());
  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryPerformance(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryPerformanceRequest* request,
    ::monitor::proto::QueryPerformanceResponse* response) {
  (void)context;

  const auto backend_status = ValidateHistoricalQueryBackend(query_manager_);
  if (!backend_status.ok()) {
    return backend_status;
  }
  if (request->server_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Missing server_name");
  }

  TimeRange time_range = ConvertTimeRange(request->time_range());
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  int total_count = 0;
  auto records = query_manager_->QueryPerformance(
      request->server_name(), time_range, page, page_size, &total_count);
  for (const auto& rec : records) {
    FillPerformanceRecord(rec, response->add_records(),
                          [this](auto* ts, const auto& tp) {
                            SetTimestamp(ts, tp);
                          });
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);
  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryNetDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QueryNetDetailResponse* response) {
  (void)context;

  const auto backend_status = ValidateHistoricalQueryBackend(query_manager_);
  if (!backend_status.ok()) {
    return backend_status;
  }
  if (request->server_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Missing server_name");
  }

  TimeRange time_range = ConvertTimeRange(request->time_range());
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  int total_count = 0;
  auto records = query_manager_->QueryNetDetail(
      request->server_name(), time_range, page, page_size, &total_count);
  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_net_name(rec.net_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_rcv_bytes_rate(rec.rcv_bytes_rate);
    proto_rec->set_snd_bytes_rate(rec.snd_bytes_rate);
    proto_rec->set_rcv_packets_rate(rec.rcv_packets_rate);
    proto_rec->set_snd_packets_rate(rec.snd_packets_rate);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);
  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryDiskDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QueryDiskDetailResponse* response) {
  (void)context;

  const auto backend_status = ValidateHistoricalQueryBackend(query_manager_);
  if (!backend_status.ok()) {
    return backend_status;
  }
  if (request->server_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Missing server_name");
  }

  TimeRange time_range = ConvertTimeRange(request->time_range());
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  int total_count = 0;
  auto records = query_manager_->QueryDiskDetail(
      request->server_name(), time_range, page, page_size, &total_count);
  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_disk_name(rec.disk_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_read_bytes_per_sec(rec.read_bytes_per_sec);
    proto_rec->set_write_bytes_per_sec(rec.write_bytes_per_sec);
    proto_rec->set_read_iops(rec.read_iops);
    proto_rec->set_write_iops(rec.write_iops);
    proto_rec->set_avg_read_latency_ms(rec.avg_read_latency_ms);
    proto_rec->set_avg_write_latency_ms(rec.avg_write_latency_ms);
    proto_rec->set_util_percent(rec.util_percent);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);
  return grpc::Status::OK;
}

}  // namespace monitor
