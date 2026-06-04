# 文件归类：当前版本使用文件（简化版主线）
# 说明：当前默认构建、运行或联调流程会直接使用该文件。

cmake_minimum_required(VERSION 3.10.2)
project(monitor_system LANGUAGES CXX) # 项目名称
set(CMAKE_CXX_STANDARD 17) # 定义版本 c++17

# Proto 定义（必须先编译）
add_subdirectory(proto)

# Worker - 工作者服务器，部署在被监控机器上，采集性能数据并推送给管理者
add_subdirectory(worker)

# Manager - 管理者服务器，部署在管理端，接收数据、存储、提供查询服务
add_subdirectory(manager)

# 文件归类：当前版本使用文件（简化版主线）
# 说明：当前默认构建流程会直接使用该构建入口。
