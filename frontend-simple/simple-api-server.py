#!/usr/bin/env python3
"""
REST API 服务器，从 MySQL 读取数据并提供给前端。

优化要点：
  - 连接池复用，避免每个请求新建/关闭连接的开销
  - 参数化查询防注入（Python mysql.connector 原生支持 %s 占位符）
"""

from flask import Flask, jsonify
from flask_cors import CORS
import mysql.connector
from mysql.connector import pooling
from datetime import datetime, timedelta
import os

app = Flask(__name__)
CORS(app)

DB_CONFIG = {
    'host': os.getenv('MYSQL_HOST', '127.0.0.1'),
    'user': os.getenv('MYSQL_USER', 'monitor'),
    'password': os.getenv('MYSQL_PASS', 'monitor123'),
    'database': os.getenv('MYSQL_DB', 'monitor_db'),
    'ssl_disabled': True,
    'charset': 'utf8mb4',
    'autocommit': True,
}

# 连接池：5 个连接，可按需扩展到 10
cnx_pool = pooling.MySQLConnectionPool(
    pool_name='api_pool',
    pool_size=5,
    pool_reset_session=True,
    **DB_CONFIG
)

def get_db_connection():
    """从连接池获取连接"""
    try:
        return cnx_pool.get_connection()
    except Exception as e:
        print(f"数据库连接池获取失败: {e}")
        return None

@app.route('/api/servers')
def get_servers():
    """获取所有服务器的最新状态"""
    conn = get_db_connection()
    if not conn:
        return jsonify({'error': '数据库连接失败'}), 500

    try:
        cursor = conn.cursor(dictionary=True)

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

        total = len(servers)
        online = sum(1 for s in servers if s['is_online'])
        offline = total - online
        avg_score = sum(s['score'] or 0 for s in servers) / total if total > 0 else 0

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

@app.route('/api/server/<server_name>/net-process')
def get_server_net_process(server_name):
    """获取单台服务器最新一次采样的进程级网络占用 Top N

    数据来源是 worker 侧 eBPF 在 socket 层的归因（kprobe/tcp_sendmsg +
    kretprobe/tcp_recvmsg），属于 /proc 无法提供的维度：
    /proc/net/dev 只有设备级总量，/proc/<pid>/net/dev 又是 per-netns 而非 per-process。
    同一采样点内所有 Top N 行共享同一个 timestamp，用它定位采样点后一次取回。
    这里不能用 MAX(sequence) 定位：sequence 是 worker 进程内单调递增的，
    worker 重启后会从 1 重新开始，MAX(sequence) 会一直指向重启前那批数据。
    """
    conn = get_db_connection()
    if not conn:
        return jsonify({'error': '数据库连接失败'}), 500

    try:
        cursor = conn.cursor(dictionary=True)

        query = """
        SELECT pid, proc_name, snd_bytes_rate, rcv_bytes_rate,
               total_bytes_rate, share_percent, timestamp
        FROM server_net_process_detail
        WHERE server_name = %s
          AND timestamp = (
              SELECT MAX(timestamp) FROM server_net_process_detail
              WHERE server_name = %s
          )
        ORDER BY total_bytes_rate DESC
        """

        cursor.execute(query, (server_name, server_name))
        records = cursor.fetchall()

        timestamp = None
        total_rate = 0
        for record in records:
            if record['timestamp']:
                timestamp = record['timestamp'].isoformat()
                record['timestamp'] = timestamp
            total_rate += record['total_bytes_rate'] or 0

        return jsonify({
            'server_name': server_name,
            'timestamp': timestamp,
            'total_rate': total_rate,
            'processes': records
        })

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
    print("REST API 服务器启动中...")
    print(f"数据库: {DB_CONFIG['host']}/{DB_CONFIG['database']}")
    print(f"连接池: pool_size=5")
    print("API 地址: http://localhost:8000")
    print("=" * 50)
    app.run(host='0.0.0.0', port=8000, debug=True)
