# Performance Monitor

分布式跨机性能监控系统：Worker（eBPF + 内核模块采集）→ gRPC → Manager（评分 + MySQL 存储）→ Flask API → Web 仪表盘。

## Build

```bash
cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build
```

编译后会在项目根目录创建 `compile_commands.json` 符号链接（`ln -sf build/compile_commands.json .`），供 VS Code C/C++ IntelliSense 使用。

CMake 会自动处理 subdirectory 依赖顺序：`proto/` → `proto` 静态库 → `worker` / `manager`。

eBPF skeleton（`net_stats.skel.h`）由 CMake 自定义命令触发 `make -C worker/src/ebpf` 生成，需要 `clang` + `bpftool` + `libbpf >= 1.0.0`。

内核模块单独编译：`make -C kernel/cpu_stat_module`。

**系统依赖：** protobuf, gRPC, protoc, grpc_cpp_plugin, libbpf, libelf, libz, libmysqlclient, clang, bpftool, linux-headers

## 运行（启动顺序不能错）

```
1. MySQL（manager 需要，默认 127.0.0.1:3306，用户 monitor/monitor123，库 monitor_db）
2. sudo insmod kernel/cpu_stat_module/cpu_stat_collector.ko  # 加载内核模块
3. ./build/manager/manager [监听地址，默认 0.0.0.0:50051]
4. sudo ./build/worker/worker [manager地址] [推送间隔秒，默认 1]
```

Worker 必须 `sudo`（eBPF map 操作 + 内核模块 mmap）。

## 参数

| 二进制 | 参数1 | 参数2 |
|--------|-------|-------|
| `manager` | 监听地址 默认 `0.0.0.0:50051` | — |
| `worker` | manager 地址 默认 `localhost:50051` | 推送间隔秒 默认 `1` |

## 前端

```bash
cd frontend-simple && ./manage.sh start   # API :8000 + Web :8080
./manage.sh stop|restart|status|logs
```

前端是独立进程（Python Flask + http.server），不依赖 manager/worker 的编译。

## 重要约定

- **无自动测试。** 验证靠手动：`stress --cpu $(nproc)` / `iperf3` 压测 → 观察 worker 日志输出 → MySQL 查询 → curl API
- **生成文件不要手动编辑：** `build/proto/*.pb.*`、`worker/include/monitor/net_stats.skel.h`、`*.ko`、`*.o` 都会被 .gitignore 排除
- **language:** C++17，CMake ≥ 3.10.2，Linux only（eBPF + 内核模块）
