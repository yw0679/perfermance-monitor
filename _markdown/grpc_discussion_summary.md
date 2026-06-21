# gRPC 讨论整理

这份文档整理了当前项目中 gRPC 的使用方式、调用链路、并发模型，以及我们讨论过的优化方向，方便面试前快速复习。

## 1. 当前项目里 gRPC 在做什么

这个项目里，gRPC 主要承担两类职责：

- `worker -> manager` 的监控数据上报
- `manager -> 外部调用方` 的查询服务

对应的协议定义在：

- `proto/monitor_info.proto`
- `proto/query_api.proto`

其中最核心的是 `GrpcManager.SetMonitorInfo(MonitorInfo) -> Empty`，它负责让每台 worker 周期性把监控数据推送到 manager。

从系统角色上看，这是一种：

- 一台 `manager`
- 多台 `worker`
- `worker` 主动上报
- `manager` 统一接收、统一判活、统一查询

的模型。

## 2. gRPC 的整体调用流程

### 2.1 worker 侧

`worker/src/rpc/monitor_pusher.cpp` 中：

1. `MonitorPusher` 构造时创建 `channel`
2. 基于 `channel` 创建 `stub`
3. 推送线程周期性执行 `PushOnce()`
4. `PushOnce()` 中先采集 `MonitorInfo`
5. 再调用：

```cpp
grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);
```

这里用的是 **同步阻塞式 unary RPC**。

### 2.2 manager 侧

`manager/src/main.cpp` 中：

1. 创建 `GrpcServerImpl service`
2. 通过 `builder.RegisterService(&service)` 把服务注册到 gRPC server
3. `BuildAndStart()` 启动 gRPC server
4. `server->Wait()` 进入服务等待状态

请求到达后，gRPC runtime 会把 `SetMonitorInfo` 路由到：

- `manager/src/rpc/grpc_server.cpp`

里的：

```cpp
::grpc::Status GrpcServerImpl::SetMonitorInfo(...)
```

### 2.3 manager 收到请求后做什么

当前 `SetMonitorInfo` 的逻辑大致是：

1. 校验请求是否为空
2. 提取主机标识
3. 把最新上报内容写入 `host_data_`
4. 调用回调，把数据交给 `HostManager::OnDataReceived(...)`
5. 返回 `grpc::Status::OK`

而 `HostManager::OnDataReceived(...)` 会继续做：

- 主机名归一化
- 评分计算
- 变化率计算
- 更新内存态 `host_scores_`
- 异步入队 MySQL 写任务

注意：

- gRPC 返回成功，不代表 MySQL 已经落盘
- 它更准确表示：`manager 已接收请求，并完成当前 RPC 处理范围内的逻辑`

## 3. worker 调用 SetMonitorInfo 时会不会阻塞

会。

当前调用方式是同步阻塞调用，所以执行到：

```cpp
stub_->SetMonitorInfo(&context, info, &response);
```

这一行时，当前推送线程会等待这次 RPC 结束，然后才会继续向下执行。

也就是说，线程会在以下情况之一发生后返回：

- manager 正常处理完并返回 `Status::OK`
- 调用失败，返回错误状态
- 调用被取消
- 调用超时

但当前代码里 **没有设置 deadline/timeout**，所以如果链路异常，调用可能卡很久。

## 4. manager 是怎么“通知” worker 调用结束的

没有额外的通知机制，也没有手写回调。

当前是标准 unary RPC 模型：

1. worker 发请求
2. manager 执行 `SetMonitorInfo`
3. `SetMonitorInfo` 返回一个 `grpc::Status`
4. gRPC 框架把这个 RPC 的响应和状态码回传给 worker
5. worker 那个阻塞中的 `SetMonitorInfo(...)` 调用返回

所以所谓“通知”，本质上就是：

- 服务端返回响应
- 客户端收到响应
- 阻塞调用解除

## 5. `status.ok()` 是什么，怎么判断

`worker` 侧有这段代码：

```cpp
grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);
if (status.ok()) {
  ...
}
```

这里的 `status` 是 **gRPC 客户端库返回的 RPC 结果对象**。

`status.ok()` 的含义是：

- 这次 RPC 最终是否以 `StatusCode::OK` 结束

它为 `true`，通常表示：

- manager 收到了请求
- 对应的 service 方法正常执行结束
- 服务端返回了 `grpc::Status::OK`
- 客户端成功收到了这个结果

它为 `false`，可能有这些原因：

- 服务端主动返回错误，例如 `INVALID_ARGUMENT`
- 客户端连接不上 manager，常见是 `UNAVAILABLE`
- 网络断开
- deadline exceeded
- 调用被取消
- 底层传输异常

需要注意：

- `status.ok() == false` 并不绝对等于 “manager 一定完全没处理”
- 可能出现 “manager 已处理，但响应没成功返回客户端” 的情况

这也是为什么后续如果要加自动重试，必须考虑幂等性。

## 6. 如果这次调用没有真正发给 manager，会表现成什么样

如果请求根本没成功到达 manager，通常表现为：

### worker 侧

- `status.ok()` 为 `false`
- 会打印失败日志
- 本轮 `PushOnce()` 返回 `false`

### manager 侧

- 不会打印 `Received monitor data from ...`
- `host_data_` 不会更新
- `HostManager::OnDataReceived(...)` 不会执行
- MySQL 队列不会新增任务

但更严谨地说：

- 客户端失败不一定代表服务端完全没执行
- 只能说客户端没有拿到一个明确的成功结果

## 7. 多台 worker 同时调 `SetMonitorInfo`，manager 是并发的吗

是。

当前项目使用的是 gRPC C++ 的同步服务模型，但这不意味着请求串行执行。

在：

```cpp
builder.RegisterService(&service);
std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
```

之后，manager 的业务逻辑就运行在 gRPC server runtime 中了。

这个 runtime 会负责：

- 监听端口
- 接收请求
- 解析 HTTP/2 + protobuf
- 分发 RPC
- 调度内部线程处理多个请求

所以多台 worker 同时调用时：

- gRPC framework 会并发地把多个请求调度到 `SetMonitorInfo(...)`
- 不需要我们自己手写 socket 监听线程池

这就是为什么代码里必须对共享状态加锁，比如：

- `GrpcServerImpl::host_data_`
- `HostManager::host_scores_`
- MySQL 写队列

## 8. gRPC 只是框架，为什么还能赋予 manager 并发能力

因为 `manager` 启动后，不再只是一个普通 C++ 程序，而是运行在 **gRPC server runtime** 之上。

可以把它理解成：

- 我们写的是“请求处理函数”
- gRPC 写好了“服务端容器 + 网络层 + 调度器”

就像 Web 开发里：

- 你写 Controller
- Tomcat/Netty 帮你监听端口和并发处理请求

gRPC 也是一样：

- 我们不需要手写线程池
- 但请求处理仍然具备并发能力

这个并发能力来自：

- gRPC 运行时
- 内部调度线程
- HTTP/2 多路复用能力

## 9. `.proto` 只定义接口，为什么 gRPC 知道 `SetMonitorInfo` 该干什么

`.proto` 不负责业务逻辑，它只负责定义：

- 服务名
- 方法名
- 请求类型
- 响应类型

例如：

```proto
service GrpcManager {
  rpc SetMonitorInfo(MonitorInfo) returns (google.protobuf.Empty);
}
```

这一步只是在描述“有这么一个 RPC”。

然后 gRPC 会根据 `.proto` 生成两类代码：

- 客户端 stub
- 服务端基类/接口

manager 端再自己实现这个接口：

```cpp
class GrpcServerImpl : public monitor::proto::GrpcManager::Service
```

并覆写：

```cpp
::grpc::Status GrpcServerImpl::SetMonitorInfo(...)
```

所以完整关系是：

1. `.proto` 定义接口契约
2. gRPC 生成接口代码
3. 我们实现接口
4. `RegisterService(&service)` 把实现注册给 gRPC server
5. gRPC 收到请求后，按方法名路由到我们的 override 函数

换句话说：

- `.proto` 负责“长什么样”
- 生成代码负责“暴露接口”
- 我们的 C++ 代码负责“具体干什么”

## 10. 当前项目在 gRPC 上已经做了哪些工作

已经完成的主要工作有：

- 定义了 `worker -> manager` 的监控上报服务
- 定义了 `manager -> 外部` 的查询服务
- worker 端实现了固定周期上报
- manager 端实现了统一接收入口
- manager 收到后会更新内存态并异步落库
- 已经有基于最近上报时间的判活逻辑

也就是说，基础的 gRPC 通信闭环已经成立了。

## 11. 还可以优化什么，以及解决什么问题

### 11.1 超时控制

当前 `SetMonitorInfo` 没有显式 deadline。

可以优化为：

- 每次上报加一个 timeout/deadline

解决的问题：

- 避免单次调用长时间阻塞
- 防止 worker 推送线程卡死
- 故障边界更清晰

### 11.2 有限重试

当前失败就是记日志，下一轮再发。

可以优化为：

- 只对瞬时错误做少量重试
- 搭配退避和抖动

解决的问题：

- 应对短暂网络抖动
- 提高上报成功率

### 11.3 轻量熔断/降频

manager 如果短时间内异常，所有 worker 继续高频打请求会形成无效压力。

可以优化为：

- 连续失败达到阈值后，临时拉长上报间隔
- 再逐步恢复

解决的问题：

- manager 故障时避免 worker 一直猛发
- 降低惊群效应

### 11.4 链路指标监控

当前缺少专门的 gRPC 链路指标。

建议记录：

- 成功次数
- 失败次数
- 错误码分布
- 调用耗时
- 最近一次成功时间
- 连续失败次数

解决的问题：

- 帮助判断是单 worker 问题、链路问题还是 manager 整体问题
- 让可观测性更完整

### 11.5 幂等保护

当前历史库是普通 `INSERT`。

如果未来加上 timeout + retry，同一条采样可能重复写入。

解决的问题：

- 让重试安全
- 防止历史数据重复

### 11.6 更完整的并发安全

当前部分共享结构已经加锁，但仍要继续检查是否存在并发访问风险。

解决的问题：

- 避免多 worker 并发上报时的数据竞争

## 12. 这套 gRPC 设计在面试里怎么总结

可以这样说：

> 这个项目里 gRPC 的核心用途是让多台 worker 向一台 manager 周期性上报监控数据，同时让 manager 对外提供查询接口。  
> 当前上报链路使用的是同步 unary RPC，worker 调用 `SetMonitorInfo` 时会阻塞等待这次调用返回；manager 收到请求后会更新最新状态、做评分和变化率计算，并把历史写库任务异步入队，然后返回 `Status::OK`。  
> 多个 worker 同时上报时，manager 侧的并发处理能力由 gRPC server runtime 提供，不需要手写线程池，但共享状态需要我们自己加锁保护。  
> 在工程优化上，最值得补的是 timeout、有限重试、链路指标和幂等保护，这样可以解决调用卡死、瞬时故障丢样本、故障定位困难以及重试导致重复写的问题。

## 13. 一句话结论

当前项目的 gRPC 已经完成了“协议定义 + 周期上报 + 服务端接收 + 内存态更新 + 查询接口”的基础闭环；后续最值得优化的是：

- 超时控制
- 有限重试
- 失败退避
- 链路指标监控
- 幂等和并发安全

这些优化的目标不是堆复杂组件，而是让“一台 manager 对多台 worker”的上报链路更稳定、更清晰、更容易排障。
