-- 文件归类：1
-- 说明：面试展示版最简数据库结构，只保留总表、网络明细、磁盘明细。

CREATE DATABASE IF NOT EXISTS monitor_db DEFAULT CHARACTER SET utf8mb4;
USE monitor_db;

DROP TABLE IF EXISTS server_softirq_detail;
DROP TABLE IF EXISTS server_mem_detail;
DROP TABLE IF EXISTS server_net_detail;
DROP TABLE IF EXISTS server_disk_detail;
DROP TABLE IF EXISTS server_performance;

-- 1. 服务器性能总表
CREATE TABLE IF NOT EXISTS server_performance (
    id INT AUTO_INCREMENT PRIMARY KEY,
    server_name VARCHAR(255) NOT NULL,
    cpu_percent FLOAT DEFAULT 0,
    usr_percent FLOAT DEFAULT 0,
    system_percent FLOAT DEFAULT 0,
    load_avg_1 FLOAT DEFAULT 0,
    load_avg_3 FLOAT DEFAULT 0,
    load_avg_15 FLOAT DEFAULT 0,
    mem_used_percent FLOAT DEFAULT 0,
    total FLOAT DEFAULT 0,
    avail FLOAT DEFAULT 0,
    disk_util_percent FLOAT DEFAULT 0,
    send_rate FLOAT DEFAULT 0,
    rcv_rate FLOAT DEFAULT 0,
    score FLOAT DEFAULT 0,
    cpu_percent_rate FLOAT DEFAULT 0,
    load_avg_1_rate FLOAT DEFAULT 0,
    mem_used_percent_rate FLOAT DEFAULT 0,
    disk_util_percent_rate FLOAT DEFAULT 0,
    send_rate_rate FLOAT DEFAULT 0,
    rcv_rate_rate FLOAT DEFAULT 0,
    timestamp DATETIME NOT NULL,
    INDEX idx_perf_server_time(server_name, timestamp),
    INDEX idx_perf_score(score)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 2. 网络明细表
CREATE TABLE IF NOT EXISTS server_net_detail (
    id INT AUTO_INCREMENT PRIMARY KEY,
    server_name VARCHAR(255) NOT NULL,
    net_name VARCHAR(64) NOT NULL,
    rcv_bytes_rate FLOAT DEFAULT 0,
    rcv_packets_rate FLOAT DEFAULT 0,
    snd_bytes_rate FLOAT DEFAULT 0,
    snd_packets_rate FLOAT DEFAULT 0,
    timestamp DATETIME NOT NULL,
    INDEX idx_net_server_time(server_name, timestamp),
    INDEX idx_net_server_name_time(server_name, net_name, timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 3. 磁盘明细表
CREATE TABLE IF NOT EXISTS server_disk_detail (
    id INT AUTO_INCREMENT PRIMARY KEY,
    server_name VARCHAR(255) NOT NULL,
    disk_name VARCHAR(64) NOT NULL,
    read_bytes_per_sec FLOAT DEFAULT 0,
    write_bytes_per_sec FLOAT DEFAULT 0,
    read_iops FLOAT DEFAULT 0,
    write_iops FLOAT DEFAULT 0,
    avg_read_latency_ms FLOAT DEFAULT 0,
    avg_write_latency_ms FLOAT DEFAULT 0,
    util_percent FLOAT DEFAULT 0,
    timestamp DATETIME NOT NULL,
    INDEX idx_disk_server_time(server_name, timestamp),
    INDEX idx_disk_server_name_time(server_name, disk_name, timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
