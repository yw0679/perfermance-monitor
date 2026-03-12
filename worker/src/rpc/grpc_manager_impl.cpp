/**
 * 文件归类：历史/预留文件（当前版本未接入主线）
 * 说明：仓库保留代码，当前默认构建与运行流程不使用该文件。
 */

#include "rpc/grpc_manager_impl.h"

#include <iostream>

namespace monitor {

GrpcManagerImpl::GrpcManagerImpl() {
  collector_ = std::make_unique<MetricCollector>();
}

GrpcManagerImpl::~GrpcManagerImpl() {}

// SetMonitorInfo 已移除 - Server 端现在本地采集数据

::grpc::Status GrpcManagerImpl::GetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::google::protobuf::Empty* request,
    ::monitor::proto::MonitorInfo* response) {
  // 实时采集监控数据
  collector_->CollectAll(response);
  return grpc::Status::OK;
}

}  // namespace monitor
/**
 * 文件归类：历史/预留文件（当前版本未接入主线）
 * 说明：仓库保留代码，当前默认构建与运行流程不使用该文件。
 */
