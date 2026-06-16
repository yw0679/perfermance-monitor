#!/usr/bin/env python3
"""
简单的 REST API 服务器，从 MySQL 读取数据并提供给前端
无需修改项目代码，直接查询数据库
"""

from flask import Flask, jsonify
from flask_cors import CORS
import mysql.connector
from datetime import datetime, timedelta
import os

app = Flask(__name__)
CORS(app)  # 允许跨域

# 数据库配置（与 manager 使用相同配置）
DB_CONFIG = {
    'host': os.getenv('MYSQL_HOST', '127.0.0.1'),
    'user': os.getenv('MYSQL_USER', 'monitor'),
    'password': os.getenv('MYSQL_PASS', 'monitor123'),
    'database': os.getenv('MYSQL_DB', 'monitor_db'),
    'ssl_disabled': True,  # 禁用 SSL 避免 Python 3.12 兼容性问题
}

def get_db_connection():
    """获取数据库连接"""
    try:
        conn = mysql.connector.connect(**DB_CONFIG)
        return conn
    except Exception as e:
        print(f"数据库连接失败: {e}")
        return None

@app.route('/api/servers')
def get_servers():
    """获取所有服务器的最新状态"""
    conn = get_db_connection()
    if not conn:
        return jsonify({'error': '数据库连接失败'}), 500

    try:
        cursor = conn.cursor(dictionary=True)

        # 获取每台服务器的最新记录（60秒内为在线）
        query = """
        SELECT
            server_name,
            cpu_percent,
            mem_used_percent,
            disk_util_percent,
            load_avg_1,
            send_rate,
            rcv_rate,
            score,
            timestamp,
            TIMESTAMPDIFF(SECOND, timestamp, NOW()) < 60 as is_online
        FROM server_performance
        WHERE (server_name, timestamp) IN (
            SELECT server_name, MAX(timestamp)
            FROM server_performance
            GROUP BY server_name
        )
        ORDER BY score DESC
        """

        cursor.execute(query)
        servers = cursor.fetchall()

        # 计算统计信息
        total = len(servers)
        online = sum(1 for s in servers if s['is_online'])
        offline = total - online
        avg_score = sum(s['score'] or 0 for s in servers) / total if total > 0 else 0

        # 格式化时间戳
        for server in servers:
            if server['timestamp']:
                server['timestamp'] = server['timestamp'].isoformat()

        return jsonify({
            'servers': servers,
            'stats': {
                'total': total,
                'online': online,
                'offline': offline,
                'avgScore': avg_score
            }
        })

    except Exception as e:
        print(f"查询失败: {e}")
        return jsonify({'error': str(e)}), 500

    finally:
        cursor.close()
        conn.close()

@app.route('/api/server/<server_name>/history')
def get_server_history(server_name):
    """获取单台服务器的历史数据"""
    conn = get_db_connection()
    if not conn:
        return jsonify({'error': '数据库连接失败'}), 500

    try:
        cursor = conn.cursor(dictionary=True)

        # 获取最近1小时的数据
        query = """
        SELECT *
        FROM server_performance
        WHERE server_name = %s
          AND timestamp >= DATE_SUB(NOW(), INTERVAL 1 HOUR)
        ORDER BY timestamp DESC
        LIMIT 100
        """

        cursor.execute(query, (server_name,))
        records = cursor.fetchall()

        # 格式化时间戳
        for record in records:
            if record['timestamp']:
                record['timestamp'] = record['timestamp'].isoformat()

        return jsonify({'records': records})

    except Exception as e:
        print(f"查询失败: {e}")
        return jsonify({'error': str(e)}), 500

    finally:
        cursor.close()
        conn.close()

@app.route('/api/health')
def health():
    """健康检查"""
    conn = get_db_connection()
    if conn:
        conn.close()
        return jsonify({'status': 'ok', 'database': 'connected'})
    else:
        return jsonify({'status': 'error', 'database': 'disconnected'}), 500

if __name__ == '__main__':
    print("=" * 50)
    print("简单 REST API 服务器启动中...")
    print(f"数据库: {DB_CONFIG['host']}:{DB_CONFIG['database']}")
    print("API 地址: http://localhost:8000")
    print("=" * 50)
    app.run(host='0.0.0.0', port=8000, debug=True)
