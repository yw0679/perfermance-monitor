# 简单前端快速指南

## ✅ 已成功部署！

前端监控系统已经启动并运行。

## 📊 访问地址

- **前端界面**: http://localhost:8080
- **API接口**: http://localhost:8000/api/servers

## 🎮 管理命令

```bash
cd frontend-simple

# 启动服务
./manage.sh start

# 停止服务
./manage.sh stop

# 重启服务
./manage.sh restart

# 查看状态
./manage.sh status

# 查看日志
./manage.sh logs
```

## 📈 当前数据

根据数据库查询，系统正在监控：

- **服务器**: ubuntu24_192.168.63.133
- **CPU 使用率**: 38.2%
- **内存使用率**: 50.8%
- **磁盘使用率**: 1.5%
- **健康评分**: 71.15
- **网络流量**: 发送 36.8 MB/s, 接收 73.5 MB/s

⚠️ **注意**: 数据显示最后更新时间超过60秒（离线状态）。如需实时数据，请启动 worker：

```bash
# 在项目根目录
sudo ./build/worker/worker 127.0.0.1:50051 1
```

## 🔧 技术架构

```
浏览器 ──HTTP──> Python Flask API ──MySQL──> 数据库
         :8080               :8000
```

- 前端: 纯 HTML + JavaScript（无需编译）
- API: Flask REST 服务
- 数据源: 直接查询 MySQL

## 📁 文件说明

- `index.html` - 前端页面
- `simple-api-server.py` - REST API 服务
- `manage.sh` - 服务管理脚本
- `start.sh` - 首次启动脚本
- `api.log` - API 日志
- `web.log` - Web 服务器日志

## 🎯 功能特性

- ✅ 实时数据（每5秒自动刷新）
- ✅ 集群统计（在线/离线/评分）
- ✅ 服务器列表
- ✅ CPU/内存/磁盘进度条
- ✅ 网络流量显示
- ✅ 在线状态监控

## 🔍 故障排查

### API 无法连接
```bash
cd frontend-simple
./manage.sh status
cat api.log
```

### 数据库连接失败
```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 -e "USE monitor_db;"
```

### 启动 worker 获取实时数据
```bash
cd /home/celeste/project/perfermance-monitor
sudo ./build/worker/worker 127.0.0.1:50051 1
```

## 📝 下一步

1. 打开浏览器访问: http://localhost:8080
2. 启动 worker 查看实时数据更新
3. 如需更高级功能，考虑使用 React + gRPC-Web 方案（见 FRONTEND_INTEGRATION.md）

---

**提示**: 这是开发环境配置。生产部署建议使用 Gunicorn/uWSGI 替代 Flask 开发服务器。
