# Performance Monitor

This is a resume project designed around production-style backend/system
engineering problems. Present it as an engineering-oriented distributed
monitoring and scheduling system, not as a small classroom demo.

Before explaining the project, polishing resume text, or making non-trivial
code changes, read `PROJECT_CONTEXT.md` first. Use the source code as the final
truth because some files under `_markdown/` may lag behind the implementation.

## Project Summary

Distributed Linux performance monitoring and scheduling system:

`Worker` collects metrics with `/proc`, eBPF, and a kernel module -> reports via
gRPC/Protobuf -> `Manager` calculates health scores and writes MySQL history ->
simple Flask API and Web dashboard visualize data.

Main resume focus:

- C++17 worker/manager architecture
- gRPC and Protobuf service communication
- Linux metric collection through `/proc`, eBPF TC hooks, and kernel-module
  `mmap`
- Async MySQL persistence with transactions and prepared statements
- Sequence-based duplicate handling and worker-side ring-buffer replay
- Health scoring and smooth weighted round-robin scheduling

Use strong, production-oriented wording around the implemented engineering
ideas: low-overhead Linux metric collection, typed service communication,
async persistence, duplicate handling, replay after temporary manager failure,
scoring, and scheduling. Do not claim facts that are not implemented, such as
Kubernetes integration, Prometheus compatibility, alerting, multi-replica high
availability, real online traffic, or a deployed load-balancer plugin.

## Build

```bash
cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

CMake builds in this order: `proto/` -> `monitor_proto` -> `worker` and
`manager`.

eBPF skeleton generation is triggered by CMake through
`make -C worker/src/ebpf`. It requires `clang`, `bpftool`, and libbpf
dependencies.

The CPU kernel module is built separately:

```bash
make -C kernel/cpu_stat_module
```

System dependencies include protobuf, gRPC, `protoc`, `grpc_cpp_plugin`,
libbpf, libelf, zlib, libmysqlclient, clang, bpftool, and Linux headers.

## Run Order

Start services in this order:

1. MySQL, using default database settings from the code:
   `127.0.0.1`, user `monitor`, password `monitor123`, database `monitor_db`.
2. Load the CPU kernel module:

   ```bash
   sudo insmod kernel/cpu_stat_module/cpu_stat_collector.ko
   ```

3. Start manager:

   ```bash
   ./build/manager/manager 0.0.0.0:50051
   ```

4. Start worker:

   ```bash
   sudo ./build/worker/worker 127.0.0.1:50051 1
   ```

Worker usually needs `sudo` because it uses eBPF TC hooks and reads the kernel
module device.

## Frontend Demo

```bash
cd frontend-simple
./manage.sh start
./manage.sh stop
```

The frontend is a simple demo layer: Flask API on port `8000` and static web UI
on port `8080`.

## Validation

There is no automated test suite. Validate manually with:

```bash
stress --cpu "$(nproc)" --timeout 60s
iperf3 -c <peer_ip> -t 60 -P 4
iperf3 -c <peer_ip> -t 60 -P 4 -R
```

Then inspect worker logs, manager logs, MySQL rows, and the frontend/API.

For compile verification, run:

```bash
cmake --build build
```

## Editing Notes

- Do not manually edit generated files such as `build/proto/*.pb.*`,
  `worker/include/monitor/net_stats.skel.h`, `*.ko`, or `*.o`.
- Keep explanations resume/interview friendly and production-oriented, while
  staying grounded in implemented code.
- If a markdown note conflicts with source code, trust the source code.
- Preserve user changes in the working tree; do not revert unrelated edits.
