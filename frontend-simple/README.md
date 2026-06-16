# 简单前端方案

这是一个**无需编译**的简单前端方案，直接使用 HTML + JavaScript。

## 架构

```
浏览器 ──HTTP──> Python REST API ──MySQL──> 数据库
```

不需要 gRPC-Web 代理，不需要 Node.js 编译，直接查询 MySQL。

## 启动方式

```bash
cd frontend-simple
./start.sh
```

然后访问: http://localhost:8080

## 功能

- ✅ 实时显示所有服务器状态
- ✅ CPU、内存、磁盘使用率可视化
- ✅ 在线/离线状态
- ✅ 健康评分
- ✅ 自动刷新（每5秒）

## 依赖

- Python 3
- Flask
- MySQL

所有依赖会自动安装。
