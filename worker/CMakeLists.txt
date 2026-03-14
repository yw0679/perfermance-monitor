# 文件归类：当前版本使用文件（简化版主线）
# 说明：当前默认构建、运行或联调流程会直接使用该文件。

# Worker - 工作者服务器
# 部署在被监控机器上，采集性能数据并推送给管理者服务器

# 查找 gRPC
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    if(NOT TARGET gRPC::grpc++)
        find_library(GRPC_LIBRARY grpc)
        find_library(GRPCPP_LIBRARY grpc++)
    endif()
    if(NOT TARGET gRPC::grpc++ AND GRPC_LIBRARY AND GRPCPP_LIBRARY)
        add_library(grpc_lib INTERFACE)
        target_link_libraries(grpc_lib INTERFACE ${GRPC_LIBRARY} ${GRPCPP_LIBRARY})
        add_library(gRPC::grpc++ ALIAS grpc_lib)
    endif()
endif()

# eBPF 是当前网络采集主线
find_library(LIBBPF_LIBRARY bpf PATHS /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_library(LIBELF_LIBRARY elf)
find_library(LIBZ_LIBRARY z)

if(EXISTS "/usr/include/bpf/libbpf.h")
    set(LIBBPF_INCLUDE_DIR "/usr/include")
else()
    find_path(LIBBPF_INCLUDE_DIR bpf/libbpf.h)
endif()

if(NOT LIBBPF_LIBRARY OR NOT LIBELF_LIBRARY OR NOT LIBZ_LIBRARY OR NOT LIBBPF_INCLUDE_DIR)
    message(FATAL_ERROR "eBPF dependencies not found. Please install libbpf/libelf/zlib headers and libraries.")
endif()

message(STATUS "eBPF support enabled")

# eBPF skeleton 生成
set(EBPF_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ebpf)
set(EBPF_SKEL_H ${CMAKE_CURRENT_SOURCE_DIR}/include/monitor/net_stats.skel.h)

add_custom_command(
    OUTPUT ${EBPF_SKEL_H}
    COMMAND make -C ${EBPF_SRC_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${EBPF_SRC_DIR}/.output/net_stats.skel.h ${EBPF_SKEL_H}
    DEPENDS ${EBPF_SRC_DIR}/net_stats.bpf.c
    COMMENT "Building eBPF program and generating skeleton"
)
add_custom_target(ebpf_skeleton DEPENDS ${EBPF_SKEL_H})

# 源文件列表
set(WORKER_SOURCES
    src/main.cpp
    src/monitor/metric_collector.cpp
    src/monitor/cpu_load_monitor.cpp
    src/monitor/cpu_stat_monitor.cpp
    src/monitor/mem_monitor.cpp
    src/monitor/net_ebpf_monitor.cpp
    src/monitor/disk_monitor.cpp
    src/monitor/host_info_monitor.cpp
    src/rpc/monitor_pusher.cpp
    src/utils/read_file.cpp
)

# 可执行文件: worker (工作者服务器)
add_executable(worker ${WORKER_SOURCES})

target_include_directories(worker PUBLIC
    ${PROJECT_SOURCE_DIR}/worker/include
)

# 基础链接库
set(WORKER_LIBS monitor_proto gRPC::grpc++)

# eBPF 依赖
add_dependencies(worker ebpf_skeleton)
target_compile_definitions(worker PRIVATE ENABLE_EBPF=1)
target_include_directories(worker PRIVATE ${LIBBPF_INCLUDE_DIR})
list(APPEND WORKER_LIBS ${LIBBPF_LIBRARY} ${LIBELF_LIBRARY} ${LIBZ_LIBRARY})

target_link_libraries(worker PUBLIC ${WORKER_LIBS})
# 文件归类：当前版本使用文件（简化版主线）
# 说明：当前默认构建流程会直接使用该 Worker 构建配置。
#find_package
#查找外部依赖
#option
#定义构建开关
#set
##定义变量
#add_executable
#生成可执行程序
##target_include_directories
#添加头文件目录
#target_link_libraries
#链接依赖库
#add_custom_command
#自定义生成步骤
#add_custom_target
#定义一个可执行的构建目标
#add_dependencies
#声明构建顺序依赖
