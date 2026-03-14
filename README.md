# performance-testing-system

一个适合面试讲解的分布式性能监控项目。

项目分成两部分：

- `worker` 部署在被监控机器上，采集 CPU、内存、磁盘、网络指标。
- `manager` 部署在管理端，通过 gRPC 接收数据，写入 MySQL，并计算服务器评分。

## 这版项目的主线亮点

1. 网络指标只保留 `eBPF` 方案，不再使用 `/proc/net/dev` 做网络统计。
2. `worker` 侧通过 `TC ingress/egress + eBPF map` 统计网卡收发字节数和包数。
3. 用户态读取 eBPF map 后计算 `KB/s` 和 `pkt/s`，再通过 `gRPC + Protobuf` 上报给 `manager`。
4. `manager` 将采集结果落到 MySQL，并做服务器评分和查询。

## eBPF 采集链路

关键文件：

- `worker/src/ebpf/net_stats.bpf.c`
- `worker/src/monitor/net_ebpf_monitor.cpp`
- `worker/src/monitor/metric_collector.cpp`
- `worker/src/rpc/monitor_pusher.cpp`

流程：

1. `net_stats.bpf.c` 在 TC 的 ingress/egress 挂点统计每个网卡的累计收发字节数、包数。
2. `net_ebpf_monitor.cpp` 加载 skeleton，把 eBPF 程序挂到网卡，并从 BPF map 读取累计值。
3. 用户态和上一次采样做差，算出每秒收发速率。
4. `metric_collector.cpp` 汇总所有指标。
5. `monitor_pusher.cpp` 通过 gRPC 推送到 `manager`。

## 构建

依赖：

- `protobuf`
- `gRPC`
- `libbpf`
- `libelf`
- `zlib`
- `clang`
- `bpftool`
- `mysqlclient`（只在构建 `manager` 时需要）

完整构建：

```bash
cmake -S . -B build
cmake --build build -j4
```

只构建 worker：

```bash
cmake -S . -B build -DBUILD_MANAGER=OFF
cmake --build build -j4
```

## 运行

启动 manager：

```bash
./build/manager/manager 0.0.0.0:50051
```

启动 worker：

```bash
sudo ./build/worker/worker 127.0.0.1:50051 10
```

说明：

- `worker` 需要 `sudo`，因为 eBPF TC Hook 需要较高权限。
- 当前网络监控主线默认就是 eBPF。

## 面试时可以这样讲

1. 我把监控系统拆成 `worker/manager` 两端，采集和管理职责分离。
2. CPU、内存、磁盘指标走常规采集；网络指标我单独用 eBPF 做内核态统计。
3. eBPF 的好处是能在内核网络路径上直接拿到更底层、更实时的流量数据，再把结果通过 gRPC 汇总到管理端。
