/**
 * 文件归类：历史/预留文件（当前版本未接入主线）
 * 说明：仓库保留代码，当前默认构建与运行流程不使用该文件。
 */

#pragma once

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include <string>

#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

// RPC 客户端 - 用于从远程主机获取监控数据
class RpcClient {
 public:
  explicit RpcClient(const std::string& host_address = "localhost:50051");
  ~RpcClient();

  // SetMonitorInfo 已移除 - Server 端现在本地采集数据

  // 从远程主机获取监控数据
  bool GetMonitorInfo(monitor::proto::MonitorInfo* monitor_info);

  // 获取连接的主机地址
  const std::string& GetHostAddress() const { return host_address_; }

 private:
  std::unique_ptr<monitor::proto::GrpcManager::Stub> stub_ptr_;
  std::string host_address_;
};

}  // namespace monitor
/**
 * 文件归类：历史/预留文件（当前版本未接入主线）
 * 说明：仓库保留代码，当前默认构建与运行流程不使用该文件。
 */
