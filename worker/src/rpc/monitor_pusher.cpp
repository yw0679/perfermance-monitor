/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include "rpc/monitor_pusher.h"

namespace {

constexpr auto kSetMonitorInfoDeadline = std::chrono::seconds(3);

void PrintCollectSummary(const monitor::proto::MonitorInfo& info) {
  std::cout << "[collect] ";

  if (info.has_host_info()) {
    std::cout << info.host_info().hostname() << " "
              << info.host_info().ip_address();
  } else {
    std::cout << info.name();
  }

  if (info.cpu_stat_size() > 0) {
    const auto& cpu = info.cpu_stat(0);
    std::cout << " cpu=" << cpu.cpu_percent() << "%";
  }

  if (info.has_mem_info()) {
    std::cout << " mem=" << info.mem_info().used_percent() << "%";
  }

  if (info.net_info_size() > 0) {
    const auto& net = info.net_info(0);
    std::cout << " net(" << net.name() << ")"
              << " rx=" << net.rcv_rate() << "KB/s"
              << " tx=" << net.send_rate() << "KB/s";
  }

  if (info.disk_info_size() > 0) {
    std::cout << " disk_count=" << info.disk_info_size();
  }

  std::cout << std::endl;
}

}  // namespace

namespace monitor {

MonitorPusher::MonitorPusher(const std::string& manager_address,int interval_seconds)
    : manager_address_(manager_address),
      interval_seconds_(interval_seconds),
      running_(false) {
  //grpc创建通道，InsecureChannelCredentials(不加密凭证)
  auto channel =grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
  //根据通道创建客户端stub，后面可以通过stub_调用远程rpc接口
  stub_ = monitor::proto::GrpcManager::NewStub(channel);
  collector_ = std::make_unique<MetricCollector>();
}

MonitorPusher::~MonitorPusher() {
  Stop();
}
//启动一个线程执行PushLoop。
void MonitorPusher::Start() {
  if (running_) {
    return;
  }
  running_ = true;
  thread_ = std::make_unique<std::thread>(&MonitorPusher::PushLoop, this);
  std::cout << "推送目标地址: " << manager_address_ << " 每 "
            << interval_seconds_ << " s推送一次" << std::endl;
}

void MonitorPusher::Stop() {
  running_ = false;
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void MonitorPusher::PushLoop() {
  while (running_) {
    if (!PushOnce()) {
      std::cerr << "Failed to push monitor data to " << manager_address_
                << std::endl;
    }

    for (int i = 0; i < interval_seconds_ && running_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}
//建立一个info，然后把所有数据装入info，通过setMonitorInfo传输。
bool MonitorPusher::PushOnce() {
  monitor::proto::MonitorInfo info;
  collector_->CollectAll(&info);
  //打印结果
  PrintCollectSummary(info);

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       kSetMonitorInfoDeadline);
  google::protobuf::Empty response;
  grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);

  if (status.ok()) {
    std::cout << ">>>向" << manager_address_ << " 推送成功 <<<" << std::endl;
    return true;
  }

  std::cerr << ">>> " << status.error_message() << " <<<" << std::endl;
  return false;
}

}  // namespace monitor
