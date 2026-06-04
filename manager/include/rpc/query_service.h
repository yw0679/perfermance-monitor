/**
 * 文件归类：1
 * 说明：定义查询 gRPC 服务实现类及其对外提供的最小查询接口集。
 */

#pragma once

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "query_api.grpc.pb.h"
#include "query_manager.h"

namespace monitor {

class HostManager;

class QueryServiceImpl : public monitor::proto::QueryService::Service {
 public:
  QueryServiceImpl(HostManager* host_manager, QueryManager* query_manager);
  ~QueryServiceImpl() override = default;

  ::grpc::Status QueryBestHost(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryBestHostRequest* request,
      ::monitor::proto::QueryBestHostResponse* response) override;

  ::grpc::Status QueryClusterOverview(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryClusterOverviewRequest* request,
      ::monitor::proto::QueryClusterOverviewResponse* response) override;

  ::grpc::Status QueryPerformance(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryPerformanceRequest* request,
      ::monitor::proto::QueryPerformanceResponse* response) override;

  ::grpc::Status QueryNetDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QueryNetDetailResponse* response) override;

  ::grpc::Status QueryDiskDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QueryDiskDetailResponse* response) override;

 private:
  TimeRange ConvertTimeRange(const ::monitor::proto::TimeRange& proto_range);
  void SetTimestamp(::google::protobuf::Timestamp* ts,
                    const std::chrono::system_clock::time_point& tp);

  HostManager* host_manager_;
  QueryManager* query_manager_;
};

}  // namespace monitor
