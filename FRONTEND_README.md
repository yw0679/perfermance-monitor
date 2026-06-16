# 性能监控系统 - 前端部署指南

## 📊 已部署：实时曲线图监控界面

这是一个**轻量级**的前端监控方案，无需编译，直接运行。

## ✨ 特性

- ⚡ **1秒实时刷新** - 动态曲线图
- 📈 **4个监控图表** - CPU、内存、网络、负载
- 🎨 **暗色主题** - GitHub 风格界面
- 📊 **60秒历史** - 滚动窗口显示趋势
- 🚀 **零依赖编译** - 纯 HTML + Python

## 🏗️ 架构

```
浏览器 ──HTTP──> Python Flask API ──MySQL──> 数据库
  :8080              :8000
```

- **前端**: HTML + JavaScript + Chart.js
- **后端**: Flask REST API
- **数据源**: MySQL 数据库

## 🚀 快速开始

### 1. 启动服务

```bash
cd frontend-simple
./manage.sh start
```

### 2. 访问界面

打开浏览器访问：**http://localhost:8080**

### 3. 启动数据采集（可选）

如果需要实时数据：

```bash
# 终端1: 启动 Manager
./build/manager/manager 0.0.0.0:50051

# 终端2: 启动 Worker
sudo ./build/worker/worker 127.0.0.1:50051 1
```

## 🎮 管理命令

```bash
cd frontend-simple

./manage.sh start    # 启动服务
./manage.sh stop     # 停止服务
./manage.sh restart  # 重启服务
./manage.sh status   # 查看状态
./manage.sh logs     # 查看日志
```

## 📁 目录结构

```
frontend-simple/
├── index.html              # 前端页面（实时曲线图）
├── simple-api-server.py    # REST API 服务
├── manage.sh              # 服务管理脚本
├── start.sh               # 首次启动脚本
├── QUICKSTART.md          # 快速指南
├── REALTIME_CHARTS.md     # 曲线图功能说明
├── README.md              # 说明文档
├── api.log                # API 日志
└── web.log                # Web 日志
```

## 📊 功能展示

### 实时指标卡片
- CPU 使用率
- 内存使用率
- 磁盘使用率
- 健康评分

### 动态曲线图
- 🔥 CPU 使用率趋势（红色）
- 💾 内存使用率趋势（蓝色）
- 🌐 网络流量趋势（绿色+紫色）
- ⚡ 系统负载趋势（橙色）

## 🔧 技术栈

- **前端**: Chart.js 4.4.0
- **后端**: Flask + Flask-CORS
- **数据库**: MySQL 8.0
- **Web服务**: Python http.server

## 📝 配置

### 数据库连接

编辑 `simple-api-server.py`：

```python
DB_CONFIG = {
    'host': '127.0.0.1',
    'user': 'monitor',
    'password': 'monitor123',
    'database': 'monitor_db',
}
```

### 刷新频率

编辑 `index.html`：

```javascript
// 每1秒更新一次
setInterval(fetchRealtimeData, 1000);

// 显示最近60个数据点
const MAX_DATA_POINTS = 60;
```

## 🐛 故障排查

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

### 无实时数据

检查 Worker 是否运行：
```bash
ps aux | grep worker
```

## 📖 详细文档

- [QUICKSTART.md](frontend-simple/QUICKSTART.md) - 快速入门
- [REALTIME_CHARTS.md](frontend-simple/REALTIME_CHARTS.md) - 曲线图详解

## 🎯 使用场景

- ✅ 实时监控服务器性能
- ✅ 压力测试可视化
- ✅ 性能趋势分析
- ✅ 故障排查辅助

## 🚀 性能

- API 服务器 CPU: < 0.5%
- 浏览器渲染 CPU: < 5%
- 内存占用: < 50MB

---

**提示**: 这是开发环境配置。生产部署建议使用 Gunicorn/uWSGI + Nginx。
