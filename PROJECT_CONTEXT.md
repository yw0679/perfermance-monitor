# Project Context

## How to use this file

This repository is a resume project designed around production-style
backend/system engineering problems. When starting a new Codex session, read
this file first to understand the project before editing code or polishing
resume text. Present it as an engineering-oriented distributed monitoring and
scheduling system, not as a small classroom demo.

Use the source code as the final truth. Some notes under `_markdown/` are useful
for interview preparation, but they may lag behind the current implementation.

## One-line summary

A lightweight distributed performance monitoring and scheduling system built
with C++, gRPC, Protobuf, MySQL, eBPF, and a Linux kernel module.

## Project positioning

The project targets a production-like high-concurrency web-service scenario
where multiple backend servers run the same application. A manager node receives
live metrics from worker agents, calculates a health score for each server,
stores historical records, and selects a suitable backend node using smooth
weighted round-robin.

Resume-friendly wording:

> Designed and implemented a production-oriented distributed performance
> monitoring and scheduling system in C++. Worker agents collect CPU, memory,
> disk, and network metrics from Linux hosts and report them to a manager
> service via gRPC/Protobuf. The manager computes health scores, persists
> metrics to MySQL asynchronously, and selects backend nodes with smooth
> weighted round-robin.

## Architecture

- `worker/`: runs on monitored machines. It periodically collects local
  performance metrics and pushes `MonitorInfo` to the manager through gRPC.
- `manager/`: receives worker reports, normalizes host identity, calculates
  scores and change rates, maintains the latest in-memory state, writes history
  to MySQL asynchronously, and exposes query APIs.
- `proto/`: Protobuf and gRPC definitions for metric reporting and query APIs.
- `kernel/cpu_stat_module/`: Linux kernel module that exports CPU time snapshots
  through `/dev/cpu_stat_monitor` and `mmap`.
- `shared/`: shared structs used by both kernel and user space.
- `frontend-simple/`: simple demo dashboard. A Flask REST API reads MySQL and
  a static HTML/Chart.js page shows server status and curves.
- `_markdown/`: personal notes for resume writing, interview review, gRPC
  discussion, and pressure-test commands.

Main runtime flow:

1. `worker` collects metrics every interval, defaulting to 1 second.
2. `worker` calls `GrpcManager.SetMonitorInfo` on the manager.
3. `manager` updates the latest host state and calculates a 0-100 health score.
4. `manager` enqueues a MySQL write task so the gRPC receive path does not wait
   on database I/O.
5. A background thread writes the sample into multiple MySQL tables in one
   transaction.
6. Query APIs and the demo frontend read current or historical data.

## Technology stack

- Language: C++17
- RPC: gRPC
- Serialization: Protobuf
- Database: MySQL
- Build: CMake
- Linux metrics: `/proc`, Linux kernel module, `mmap`, eBPF TC hook
- Frontend demo: Python Flask, plain HTML, JavaScript, Chart.js

## Implemented metric collection

CPU load:

- Reads `/proc/loadavg`.
- Reports 1/5/15 minute load values through `CpuLoad`.
- In proto/code the 5-minute field is named `load_avg_3`; mention it as
  5-minute load in human explanations.

CPU utilization:

- Implemented with a Linux kernel module rather than only reading `/proc/stat`.
- Kernel module uses `hrtimer` to periodically read per-CPU cumulative time
  fields, including user, nice, system, idle, iowait, irq, softirq, steal, guest,
  and guest_nice.
- Data is exposed through a misc character device:
  `/dev/cpu_stat_monitor`.
- User space maps the shared region with `mmap`.
- A `seq` field is used to avoid reading a half-written snapshot.
- Worker calculates percentages from the delta between current and previous
  snapshots.

Memory:

- Reads `/proc/meminfo`.
- Extracts total, free, available, buffers, cached, dirty, writeback,
  anonymous pages, reclaimable memory, and related fields.
- Calculates memory used percentage as `(MemTotal - MemAvailable) / MemTotal`.

Disk:

- Reads `/proc/diskstats`.
- Skips virtual devices such as loop, ram, fd, and sr.
- Calculates read/write throughput, IOPS, average read/write latency, and disk
  utilization by comparing two samples.

Network:

- Uses eBPF with TC ingress/egress hooks.
- Tracks per-interface receive/send bytes and packets in a BPF hash map keyed
  by ifindex.
- User space reads the BPF map through libbpf and calculates rates.
- The implementation filters to physical interfaces and ignores loopback and
  common virtual interfaces.

Host identity:

- Worker collects hostname and a primary non-loopback IPv4 address.
- Manager combines hostname and IP when available, so multiple hosts are easier
  to distinguish.

## Manager behavior

Current state:

- Stores each host's latest `MonitorInfo`, score, and timestamp in memory.
- Removes stale hosts after they have not reported for about 60 seconds.
- Query overview treats hosts older than about 60 seconds as offline.

Scoring:

- Calculates a 0-100 health score from CPU utilization, memory usage, load,
  disk utilization, and network rates.
- Current weights in source:
  - CPU: 0.35
  - Memory: 0.30
  - Load: 0.15
  - Disk: 0.15
  - Network: 0.05
- Higher pressure lowers the score.

Scheduling:

- Uses smooth weighted round-robin over host scores.
- The score becomes the scheduling weight.
- The manager smooths weight changes to reduce jitter.
- This is better to describe than simply saying "always choose the highest
  score", because it avoids sending every request to one host.

Database writing:

- gRPC receive path calculates score and change rates, then enqueues a write
  task.
- A background MySQL thread consumes the queue.
- It reuses a MySQL connection and prepared statements.
- It uses transactions so one sample's main, CPU, network, and disk rows are
  committed together.
- It uses `INSERT IGNORE` with `(server_name, sequence)` style unique keys to
  reduce duplicate writes.

Query APIs:

- `QueryBestHost`: returns the currently selected best host and cluster stats.
- `QueryClusterOverview`: returns latest server snapshots and cluster summary.
- `QueryPerformance`: queries historical performance records from MySQL.
- `QueryNetDetail`: queries historical network detail.
- `QueryDiskDetail`: queries historical disk detail.

## Worker reliability details

- Worker sends reports through a synchronous unary gRPC call.
- Current code sets a 3-second deadline for `SetMonitorInfo`.
- On push failure, the sample is stored in a fixed-size ring buffer.
- When the manager becomes reachable again, buffered samples are replayed.
- Each worker sample has an increasing `sequence` value.
- Manager uses `sequence` for per-host duplicate detection.

These are good resume/interview points because they show attention to failure
handling, not just the happy path.

## Database schema

Main SQL file:

- `manager/sql/init_server_performance.sql`

Tables:

- `server_performance`: main decision/history table with core metrics, score,
  change rates, sequence, and timestamp.
- `server_cpu_detail`: CPU state percentages.
- `server_net_detail`: per-network-interface rates.
- `server_disk_detail`: per-disk throughput, IOPS, latency, and utilization.

Schema notes:

- Historical queries are optimized around `(server_name, timestamp)`.
- Detail tables exist because a host can have multiple disks and network
  interfaces.
- A MySQL event cleans old records in batches.

## Frontend demo

The frontend is intentionally simple:

- `frontend-simple/simple-api-server.py`: Flask API that reads MySQL with a
  connection pool.
- `frontend-simple/index.html`: static dashboard using Chart.js.
- `frontend-simple/manage.sh`: starts/stops both API and static web server.

Use this as a demo/visualization layer, not the main technical selling point.
The main resume value is in the C++ worker/manager and Linux data collection.

## Build and run

Build:

```bash
cmake -B build -S .
cmake --build build
```

Load CPU kernel module:

```bash
make -C kernel/cpu_stat_module
sudo insmod kernel/cpu_stat_module/cpu_stat_collector.ko
```

Start manager:

```bash
./build/manager/manager 0.0.0.0:50051
```

Start worker:

```bash
sudo ./build/worker/worker 127.0.0.1:50051 1
```

Start frontend demo:

```bash
cd frontend-simple
./manage.sh start
```

Pressure tests:

```bash
stress --cpu "$(nproc)" --timeout 60s
iperf3 -c <peer_ip> -t 60 -P 4
iperf3 -c <peer_ip> -t 60 -P 4 -R
```

## Good resume bullet candidates

- Built a C++ worker-manager monitoring system using gRPC and Protobuf to
  collect Linux host metrics and report them to a central manager.
- Implemented CPU utilization collection with a custom Linux kernel module,
  exposing per-CPU snapshots to user space through a character device and
  `mmap`.
- Implemented eBPF TC ingress/egress network monitoring with a BPF hash map to
  calculate per-interface throughput and packet rates.
- Designed asynchronous MySQL persistence with prepared statements,
  transaction-based multi-table writes, and sequence-based duplicate handling.
- Calculated host health scores from CPU, memory, load, disk, and network
  metrics, then selected backend nodes with smooth weighted round-robin.
- Built a simple Flask and Chart.js dashboard to visualize server status and
  recent metric trends.

## Interview talking points

Why not only use `/proc` for everything?

- `/proc` is simple and suitable for low-frequency text-based metrics such as
  load, memory, and disk stats.
- CPU utilization was implemented with a kernel module and `mmap` to practice
  kernel/user-space interaction and reduce repeated text parsing.
- Network traffic is high-frequency and naturally sits on the packet path, so
  eBPF TC hooks are a better fit.

Why gRPC?

- The system is service-to-service communication, not browser-facing public API.
- gRPC gives typed interfaces, Protobuf serialization, HTTP/2 transport, and
  convenient code generation.

Why async MySQL writes?

- The manager should acknowledge worker reports quickly.
- Database I/O can be slower or temporarily unstable.
- Moving writes to a background queue keeps the RPC receive path lightweight.

Why smooth weighted round-robin?

- Always choosing the single highest score can overload the same host.
- Smooth weighted round-robin distributes requests according to score while
  still favoring healthier hosts.

## Positioning Boundaries

- Strongly emphasize production-oriented engineering design: low-overhead Linux
  metric collection, typed service communication, async persistence,
  transaction-based multi-table writes, retry/replay, duplicate handling, health
  scoring, and scheduling.
- Do not claim unimplemented facts such as Kubernetes integration, Prometheus
  compatibility, alerting, multi-replica high availability, real online traffic,
  or a deployed load-balancer plugin unless code is added for them.
- Do not claim full distributed fault tolerance. The worker has a local ring
  buffer and the manager has async writes, but this is not a replicated storage
  system.
- Do not make the frontend the main selling point; it is a visualization layer.
- Be careful with old markdown notes: they may say there is no gRPC deadline,
  but current code has a 3-second deadline.
- There is a query-side `QueryCpuDetail` helper in C++, but the exposed proto
  query service currently focuses on performance, network, and disk detail.

## Recommended new-session prompt

Use this prompt when opening a new Codex session:

```text
This is my production-oriented resume project. First read PROJECT_CONTEXT.md,
then inspect the relevant source files before answering. Keep descriptions
strong, engineering-focused, and grounded in implemented code.
```
