#!/bin/bash

echo "=== 启动简单前端 ==="
echo ""

# 检查 Python 和依赖
if ! command -v python3 &> /dev/null; then
    echo "错误: 未找到 python3"
    exit 1
fi

echo "1. 检查并安装 Python 依赖..."
# 尝试使用系统包管理器
if ! python3 -c "import flask" 2>/dev/null; then
    echo "安装 Flask..."
    if command -v apt-get &> /dev/null; then
        sudo apt-get update && sudo apt-get install -y python3-flask python3-flask-cors python3-mysql.connector
    elif command -v pip3 &> /dev/null; then
        pip3 install flask flask-cors mysql-connector-python
    elif python3 -m pip --version &> /dev/null; then
        python3 -m pip install flask flask-cors mysql-connector-python
    else
        echo "错误: 无法安装依赖，请手动安装: sudo apt-get install python3-flask python3-flask-cors python3-mysql.connector"
        exit 1
    fi
fi

echo ""
echo "2. 检查 MySQL 连接..."
if mysql -h127.0.0.1 -umonitor -pmonitor123 -e "USE monitor_db;" 2>/dev/null; then
    echo "✓ 数据库连接正常"
else
    echo "✗ 数据库连接失败，请检查 MySQL 配置"
    exit 1
fi

echo ""
echo "3. 启动 API 服务器（后台）..."
python3 simple-api-server.py &
API_PID=$!

sleep 2

# 检查 API 是否启动
if curl -s http://localhost:8000/api/health > /dev/null; then
    echo "✓ API 服务器运行在 http://localhost:8000"
else
    echo "✗ API 服务器启动失败"
    kill $API_PID 2>/dev/null
    exit 1
fi

echo ""
echo "4. 启动 Web 服务器..."
echo ""
echo "✓ 准备就绪！"
echo ""
echo "访问地址: http://localhost:8080"
echo ""
echo "按 Ctrl+C 停止所有服务"
echo ""

# 启动简单 HTTP 服务器
python3 -m http.server 8080

# 清理
kill $API_PID 2>/dev/null
