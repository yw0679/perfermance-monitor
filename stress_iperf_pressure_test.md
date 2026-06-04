# 使用 `stress` 和 `iperf3` 做压测

本文档说明如何在当前项目中使用 `stress` 做 CPU 压测、使用 `iperf3` 做网络压测，并给出启动命令、观测命令和预期结果。

## 1. 压测目标与项目链路

当前项目的链路如下：

- `worker` 每秒采集一次主机指标并推送到 `manager`
- `manager` 接收数据后写入 `MySQL`
- CPU 使用率来自内核模块导出的 `/dev/cpu_stat_monitor`
- 网络吞吐来自 `worker` 中的 eBPF TC hook

因此：

- 用 `stress` 可以验证 CPU 采集链路是否正常
- 用 `iperf3` 可以验证网络采集链路是否正常

## 2. 前置条件

### 2.1 安装压测工具

Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y stress iperf3 mysql-client
```

### 2.2 构建项目

如果仓库还没有编译：

```bash
cmake -S . -B build
cmake --build build -j
```

### 2.3 初始化 MySQL

如果你希望查看历史结果，需要先初始化数据库：

```bash
mysql -uroot -p < manager/sql/init_server_performance.sql
```

项目默认使用以下数据库配置：

- Host: `127.0.0.1`
- User: `monitor`
- Password: `monitor123`
- DB: `monitor_db`

如果本机没有这个账号，需要手动创建并授权。

### 2.4 加载 CPU 内核模块

`worker` 的 CPU 百分比依赖 `/dev/cpu_stat_monitor`，所以要先编译并加载内核模块：

```bash
make -C kernel/cpu_stat_module
sudo insmod kernel/cpu_stat_module/cpu_stat_collector.ko
ls -l /dev/cpu_stat_monitor
```

如果已经加载过，可以先检查：

```bash
lsmod | grep cpu_stat_collector
```

卸载命令：

```bash
sudo rmmod cpu_stat_collector
```

## 3. 启动项目

### 3.1 启动 manager

```bash
./build/manager/manager 0.0.0.0:50051
```

预期现象：

- 终端打印 `Listening on: 0.0.0.0:50051`
- 如果 MySQL 可用，会打印 `Historical query backend initialized successfully`

### 3.2 启动 worker

`worker` 建议用 `sudo` 启动，因为：

- CPU 采集依赖内核模块设备
- 网络采集依赖 eBPF TC hook，通常需要 root 权限

```bash
sudo ./build/worker/worker <manager_ip>:50051 1
```

示例：

```bash
sudo ./build/worker/worker 127.0.0.1:50051 1
```

预期现象：

- 终端打印推送地址和推送周期
- 每秒打印一条采集摘要
- 成功时会看到 `>>>向127.0.0.1:50051 推送成功 <<<`
- 写入 `MySQL` 时，`server_name` 通常是 `主机名_IP`，例如 `test-host_192.168.1.10`

典型日志类似：

```text
[collect] test-host 192.168.1.10 cpu=7.4% mem=32.1% net(enp0s3) rx=12.4KB/s tx=8.1KB/s disk_count=1
>>>向127.0.0.1:50051 推送成功 <<<
```

## 4. 用 `stress` 做 CPU 压测

### 4.1 推荐命令

在被 `worker` 监控的机器上执行：

```bash
stress --cpu 4 --timeout 60s
```

如果想压满更多核心，可以把 `4` 改成机器的 CPU 核数，例如：

```bash
stress --cpu "$(nproc)" --timeout 60s
```

### 4.2 预期结果

压测期间，`worker` 每秒打印的日志里：

- `cpu=...%` 会明显升高
- `load_avg_1` 会在 `manager` 的历史记录中逐步升高
- `mem=...%` 通常变化不大
- `net(...)` 通常不会明显变化

如果使用：

```bash
stress --cpu "$(nproc)" --timeout 60s
```

通常预期：

- `cpu_percent` 接近 `80%` 到 `100%`
- `idle_percent` 明显下降
- `usr_percent` 和 `system_percent` 会升高
- `load_avg_1` 会接近或超过 CPU 核数

注意：

- 第一次采样可能偏低，因为 CPU 百分比是通过前后两次快照差值计算出来的
- 如果 `/dev/cpu_stat_monitor` 不存在，`worker` 仍可能运行，但 `cpu_stat` 不会正常更新

### 4.3 观测命令

直接看 `worker` 日志即可，或者查 MySQL：

```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db -e "
SELECT server_name, timestamp, cpu_percent, usr_percent, system_percent, idle_percent, load_avg_1
FROM server_performance
ORDER BY timestamp DESC
LIMIT 10;"
```

预期看到：

- `cpu_percent` 持续升高
- `idle_percent` 持续降低
- `load_avg_1` 比压测前更高

## 5. 用 `iperf3` 做网络压测

### 5.1 重要限制

当前项目的网络监控只统计物理网卡，不统计：

- `lo`
- docker/veth/bridge 等虚拟网卡

所以：

- 不要用 `127.0.0.1`
- 不要用 `localhost`
- 最好在两台机器之间压测

否则 `iperf3` 跑起来了，但项目里 `net_info` 可能几乎没有变化。

### 5.2 准备一个对端主机

在对端主机启动 `iperf3` 服务端：

```bash
iperf3 -s
```

假设对端 IP 为 `192.168.1.20`，被监控主机 IP 为 `192.168.1.10`。

### 5.3 压发送方向

在被监控主机上执行：

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4
```

含义：

- `-t 60`: 持续 60 秒
- `-P 4`: 4 个并发流，方便更容易打满带宽

预期结果：

- `worker` 日志中的 `tx=...KB/s` 明显升高
- `send_rate` 明显升高
- `send_packets_rate` 明显升高
- `rx=...KB/s` 可能只有少量 ACK 流量
- 刚开始的第一个采样点可能偏低，因为网络速率也是由前后两次累计值差分计算出来的

### 5.4 压接收方向

仍然在被监控主机上执行：

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4 -R
```

`-R` 表示反向传输，此时主要是对端向被监控主机发数据。

预期结果：

- `worker` 日志中的 `rx=...KB/s` 明显升高
- `rcv_rate` 明显升高
- `rcv_packets_rate` 明显升高
- `tx=...KB/s` 通常只保留少量响应流量
- 刚开始的第一个采样点可能偏低，因为网络速率也是由前后两次累计值差分计算出来的

### 5.5 双向压测

如果需要同时观察收发两侧，可使用：

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4 --bidir
```

预期结果：

- `tx=...KB/s` 和 `rx=...KB/s` 都明显升高
- `send_rate` 和 `rcv_rate` 同时升高

## 6. 网络压测结果观测

### 6.1 直接看 `worker` 日志

发送方向压测时，日志类似：

```text
[collect] test-host 192.168.1.10 cpu=12.3% mem=32.4% net(enp0s3) rx=120.7KB/s tx=94321.8KB/s disk_count=1
```

接收方向压测时，日志类似：

```text
[collect] test-host 192.168.1.10 cpu=10.8% mem=32.5% net(enp0s3) rx=95102.6KB/s tx=98.4KB/s disk_count=1
```

### 6.2 查总表

```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db -e "
SELECT server_name, timestamp, send_rate, rcv_rate, send_packets_rate, rcv_packets_rate
FROM server_performance
ORDER BY timestamp DESC
LIMIT 10;"
```

预期看到：

- 发流量测试时 `send_rate` 明显高于压测前
- 收流量测试时 `rcv_rate` 明显高于压测前

如果不确定 `server_name`，可以先执行：

```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db -e "
SELECT DISTINCT server_name
FROM server_performance
ORDER BY timestamp DESC
LIMIT 10;"
```

### 6.3 查网络明细表

```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db -e "
SELECT server_name, net_name, timestamp, snd_bytes_rate, rcv_bytes_rate, snd_packets_rate, rcv_packets_rate
FROM server_net_detail
ORDER BY timestamp DESC
LIMIT 10;"
```

预期看到：

- 物理网卡名例如 `eth0`、`enp0s3`、`ens33`
- 对应网卡的收发速率在压测期间显著增大

## 7. 推荐压测组合

### 7.1 单独验证 CPU 采集

```bash
stress --cpu "$(nproc)" --timeout 60s
```

预期：

- `cpu_percent` 高
- `load_avg_1` 高
- `send_rate`/`rcv_rate` 基本稳定

### 7.2 单独验证网络采集

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4
iperf3 -c 192.168.1.20 -t 60 -P 4 -R
```

预期：

- 第一条命令主要拉高 `send_rate`
- 第二条命令主要拉高 `rcv_rate`

### 7.3 同时验证 CPU 与网络链路

先在一个终端执行：

```bash
stress --cpu "$(nproc)" --timeout 60s
```

同时在另一个终端执行：

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4 --bidir
```

预期：

- `cpu_percent` 升高
- `load_avg_1` 升高
- `send_rate` 和 `rcv_rate` 同时升高
- `manager` 中该主机的 `score` 通常会下降

## 8. 常见问题

### 8.1 `worker` 有推送成功日志，但没有 CPU 百分比

优先检查：

```bash
ls -l /dev/cpu_stat_monitor
lsmod | grep cpu_stat_collector
```

### 8.2 `iperf3` 有吞吐，但项目里网络指标不变

优先检查：

- 是否使用了 `127.0.0.1`
- 是否走了 `lo` 或 docker/veth/bridge 网卡
- `worker` 是否用 `sudo` 启动
- eBPF TC hook 是否成功附着到物理网卡

### 8.3 `manager` 能收实时数据，但查不到历史

优先检查：

- `manager` 是否启用了 MySQL 支持
- MySQL 是否存在 `monitor_db`
- `monitor` 用户是否可登录
- 是否已执行 `manager/sql/init_server_performance.sql`

## 9. 一组最小可用命令

如果你只想快速验证一遍，按下面顺序执行即可。

终端 1，启动 manager：

```bash
./build/manager/manager 0.0.0.0:50051
```

终端 2，加载 CPU 模块并启动 worker：

```bash
make -C kernel/cpu_stat_module
sudo insmod kernel/cpu_stat_module/cpu_stat_collector.ko
sudo ./build/worker/worker 127.0.0.1:50051 1
```

终端 3，压 CPU：

```bash
stress --cpu "$(nproc)" --timeout 60s
```

终端 4，压网络：

```bash
iperf3 -c 192.168.1.20 -t 60 -P 4 -R
```

最核心的预期结果：

- `stress` 期间，`cpu_percent`、`load_avg_1` 上升
- `iperf3` 期间，`send_rate` 或 `rcv_rate` 上升
- `worker` 日志每秒都能看到变化
- 开启 MySQL 时，`server_performance` 和 `server_net_detail` 中能查到对应记录
