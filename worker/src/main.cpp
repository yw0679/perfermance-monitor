/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>

#include "rpc/monitor_pusher.h"

constexpr char kDefaultManagerAddress[] = "localhost:50051";
constexpr int kDefaultPushInterval = 10;  // 秒


void PrintUsage(const std::string &ManagerAddress,int PushInterval) {
  std::cout <<"manager_address:" << ManagerAddress << std::endl;
  std::cout << "Push interval: " << PushInterval << " s" << std::endl;
}



namespace {
volatile std::sig_atomic_t g_should_exit = 0;

void HandleSignal(int signal) {
  if (signal == SIGINT) {
    g_should_exit = 1;
  }
}
}  // namespace



int main(int argc, char* argv[]) {
  std::string manager_address = kDefaultManagerAddress;
  int interval_seconds = kDefaultPushInterval;

  // 解析命令行参数
  if (argc > 1) {
    manager_address = argv[1];
  }
  if (argc > 2) {
    interval_seconds = std::stoi(argv[2]);
    if (interval_seconds <= 0) {
      interval_seconds = kDefaultPushInterval;
    }
  }
  
  //打印启动日志
  PrintUsage(manager_address,interval_seconds);
  //注册Ctrl + c的处理函数
  std::signal(SIGINT, HandleSignal);

  // 创建并启动推送器
  monitor::MonitorPusher pusher(manager_address, interval_seconds);
  pusher.Start(); 

  while (!g_should_exit)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::cout << "main() exit" << std::endl;
  return 0;
}
/**
 * 文件归类：当前版本使用文件（简化版主线）
 * 说明：当前默认构建、运行或联调流程会直接使用该文件。
 */
