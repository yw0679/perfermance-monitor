-- ============================================================
-- 监控数据库初始化脚本
-- ============================================================
-- 设计原则：
--   1. 实时决策走内存（低延迟 + 调度状态维护），历史回溯走 MySQL
--   2. append-only 写入模型（只有 INSERT/SELECT/DELETE，没有 UPDATE）
--   3. 写入路径用事务保证同一采样快照的三表一致性
--   4. 自动清理 7 天前数据，控制磁盘增长
--   5. 索引对齐查询模式：WHERE server_name + timestamp 范围 + ORDER BY timestamp DESC
-- ============================================================

CREATE DATABASE IF NOT EXISTS monitor_db
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE monitor_db;

-- 开启 Event Scheduler（定时清理依赖）
SET GLOBAL event_scheduler = ON;

DROP TABLE IF EXISTS server_softirq_detail;
DROP TABLE IF EXISTS server_mem_detail;
DROP TABLE IF EXISTS server_cpu_detail;
DROP TABLE IF EXISTS server_net_detail;
DROP TABLE IF EXISTS server_disk_detail;
DROP TABLE IF EXISTS server_performance;

-- ============================================================
-- 1. 服务器性能总表（只保留决策核心字段，明细走子表）
-- ============================================================
-- CPU 明细 → 子表 server_cpu_detail
-- 网络明细 → 子表 server_net_detail
-- 磁盘明细 → 子表 server_disk_detail
-- 查询模式：WHERE server_name = ? AND timestamp BETWEEN ? AND ? ORDER BY timestamp DESC
-- ============================================================
CREATE TABLE server_performance (
    id            INT UNSIGNED AUTO_INCREMENT,
    server_name   VARCHAR(255)       NOT NULL,
    cpu_percent   FLOAT              DEFAULT 0,
    load_avg_1    FLOAT              DEFAULT 0,
    load_avg_3    FLOAT              DEFAULT 0,
    load_avg_15   FLOAT              DEFAULT 0,
    mem_used_percent FLOAT           DEFAULT 0,
    disk_util_percent FLOAT          DEFAULT 0,
    send_rate     FLOAT              DEFAULT 0,
    rcv_rate      FLOAT              DEFAULT 0,
    score         FLOAT              DEFAULT 0,
    cpu_percent_rate      FLOAT      DEFAULT 0,
    load_avg_1_rate       FLOAT      DEFAULT 0,
    mem_used_percent_rate FLOAT      DEFAULT 0,
    disk_util_percent_rate FLOAT     DEFAULT 0,
    send_rate_rate        FLOAT      DEFAULT 0,
    rcv_rate_rate         FLOAT      DEFAULT 0,
    sequence      BIGINT UNSIGNED    DEFAULT 0,
    timestamp     DATETIME(3)        NOT NULL,

    PRIMARY KEY (id),
    UNIQUE KEY uk_host_seq (server_name, sequence),
    INDEX idx_server_time (server_name, timestamp),
    INDEX idx_timestamp (timestamp),
    INDEX idx_score (score)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 2. CPU 明细表
-- ============================================================
CREATE TABLE server_cpu_detail (
    id               INT UNSIGNED AUTO_INCREMENT,
    server_name      VARCHAR(255)     NOT NULL,
    usr_percent      FLOAT            DEFAULT 0,
    system_percent   FLOAT            DEFAULT 0,
    nice_percent     FLOAT            DEFAULT 0,
    idle_percent     FLOAT            DEFAULT 0,
    io_wait_percent  FLOAT            DEFAULT 0,
    irq_percent      FLOAT            DEFAULT 0,
    soft_irq_percent FLOAT            DEFAULT 0,
    sequence         BIGINT UNSIGNED  DEFAULT 0,
    timestamp        DATETIME(3)      NOT NULL,

    PRIMARY KEY (id),
    UNIQUE KEY uk_cpu_host_seq (server_name, sequence),
    INDEX idx_cpu_server_time (server_name, timestamp),
    INDEX idx_cpu_timestamp (timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 3. 网络明细表
-- ============================================================
-- 查询模式：WHERE server_name = ? AND net_name = ? AND timestamp BETWEEN ? AND ? ORDER BY timestamp DESC
-- 合并为单一复合索引 idx_net_server_time
-- ============================================================
CREATE TABLE server_net_detail (
    id               INT UNSIGNED AUTO_INCREMENT,
    server_name      VARCHAR(255)     NOT NULL,
    net_name         VARCHAR(64)      NOT NULL,
    rcv_bytes_rate   FLOAT            DEFAULT 0,
    rcv_packets_rate FLOAT            DEFAULT 0,
    snd_bytes_rate   FLOAT            DEFAULT 0,
    snd_packets_rate FLOAT            DEFAULT 0,
    sequence         BIGINT UNSIGNED  DEFAULT 0,
    timestamp        DATETIME(3)      NOT NULL,

    PRIMARY KEY (id),
    UNIQUE KEY uk_net_host_seq (server_name, sequence, net_name),
    INDEX idx_net_server_time (server_name, net_name, timestamp),
    INDEX idx_net_timestamp (timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 4. 磁盘明细表
-- ============================================================
CREATE TABLE server_disk_detail (
    id                  INT UNSIGNED AUTO_INCREMENT,
    server_name         VARCHAR(255)   NOT NULL,
    disk_name           VARCHAR(64)    NOT NULL,
    read_bytes_per_sec  FLOAT          DEFAULT 0,
    write_bytes_per_sec FLOAT          DEFAULT 0,
    read_iops           FLOAT          DEFAULT 0,
    write_iops          FLOAT          DEFAULT 0,
    avg_read_latency_ms FLOAT          DEFAULT 0,
    avg_write_latency_ms FLOAT         DEFAULT 0,
    util_percent        FLOAT          DEFAULT 0,
    sequence            BIGINT UNSIGNED DEFAULT 0,
    timestamp           DATETIME(3)    NOT NULL,

    PRIMARY KEY (id),
    UNIQUE KEY uk_disk_host_seq (server_name, sequence, disk_name),
    INDEX idx_disk_server_time (server_name, disk_name, timestamp),
    INDEX idx_disk_timestamp (timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 定时清理：每天删除 7 天前的数据
-- 分批删除避免长时间锁表
-- ============================================================
DROP EVENT IF EXISTS evt_cleanup_old_records;

DELIMITER $$

CREATE EVENT evt_cleanup_old_records
ON SCHEDULE EVERY 1 DAY
STARTS CURRENT_TIMESTAMP
ON COMPLETION PRESERVE
DO
BEGIN
    DECLARE done INT DEFAULT 0;

    cleanup_perf: LOOP
        DELETE FROM server_performance
        WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY)
        LIMIT 1000;
        IF ROW_COUNT() = 0 THEN LEAVE cleanup_perf; END IF;
        DO SLEEP(0.1);
    END LOOP;

    cleanup_cpu: LOOP
        DELETE FROM server_cpu_detail
        WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY)
        LIMIT 1000;
        IF ROW_COUNT() = 0 THEN LEAVE cleanup_cpu; END IF;
        DO SLEEP(0.1);
    END LOOP;

    cleanup_net: LOOP
        DELETE FROM server_net_detail
        WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY)
        LIMIT 1000;
        IF ROW_COUNT() = 0 THEN LEAVE cleanup_net; END IF;
        DO SLEEP(0.1);
    END LOOP;

    cleanup_disk: LOOP
        DELETE FROM server_disk_detail
        WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY)
        LIMIT 1000;
        IF ROW_COUNT() = 0 THEN LEAVE cleanup_disk; END IF;
        DO SLEEP(0.1);
    END LOOP;
END$$

DELIMITER ;
