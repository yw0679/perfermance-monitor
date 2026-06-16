#!/bin/bash
# 前端服务管理脚本

PID_FILE="/tmp/monitor-frontend.pid"

case "$1" in
    start)
        if [ -f "$PID_FILE" ]; then
            echo "服务似乎已在运行"
            cat "$PID_FILE"
            exit 1
        fi

        echo "启动服务..."
        cd "$(dirname "$0")"

        # 启动 API 服务器
        python3 simple-api-server.py > api.log 2>&1 &
        API_PID=$!

        # 启动 Web 服务器
        python3 -m http.server 8080 > web.log 2>&1 &
        WEB_PID=$!

        echo "$API_PID $WEB_PID" > "$PID_FILE"

        sleep 2

        if curl -s http://localhost:8000/api/health > /dev/null && \
           curl -s http://localhost:8080 > /dev/null; then
            echo "✓ 服务启动成功！"
            echo ""
            echo "  前端地址: http://localhost:8080"
            echo "  API地址:  http://localhost:8000"
            echo "  API PID:  $API_PID"
            echo "  Web PID:  $WEB_PID"
            echo ""
            echo "使用 './manage.sh stop' 停止服务"
        else
            echo "✗ 服务启动失败，请检查日志"
            cat "$PID_FILE" | xargs kill 2>/dev/null
            rm -f "$PID_FILE"
            exit 1
        fi
        ;;

    stop)
        if [ ! -f "$PID_FILE" ]; then
            echo "服务未运行"
            exit 0
        fi

        echo "停止服务..."
        cat "$PID_FILE" | xargs kill 2>/dev/null
        rm -f "$PID_FILE"
        echo "✓ 服务已停止"
        ;;

    restart)
        $0 stop
        sleep 2
        $0 start
        ;;

    status)
        if [ ! -f "$PID_FILE" ]; then
            echo "服务未运行"
            exit 0
        fi

        PIDS=$(cat "$PID_FILE")
        echo "服务状态:"
        for pid in $PIDS; do
            if ps -p $pid > /dev/null 2>&1; then
                echo "  ✓ PID $pid 正在运行"
            else
                echo "  ✗ PID $pid 已停止"
            fi
        done

        echo ""
        echo "测试连接:"
        if curl -s http://localhost:8000/api/health > /dev/null; then
            echo "  ✓ API 服务正常"
        else
            echo "  ✗ API 服务异常"
        fi

        if curl -s http://localhost:8080 > /dev/null; then
            echo "  ✓ Web 服务正常"
        else
            echo "  ✗ Web 服务异常"
        fi
        ;;

    logs)
        tail -f api.log web.log
        ;;

    *)
        echo "用法: $0 {start|stop|restart|status|logs}"
        echo ""
        echo "  start   - 启动前端服务"
        echo "  stop    - 停止前端服务"
        echo "  restart - 重启前端服务"
        echo "  status  - 查看服务状态"
        echo "  logs    - 查看实时日志"
        exit 1
        ;;
esac
