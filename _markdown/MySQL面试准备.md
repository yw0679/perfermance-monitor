# MySQL 相关面试准备

---

## 一、表结构

### 面试官：数据库有几张表？主表的字段能说一下吗？

**口述回答：**

三张表。一张性能总表，两张明细表。

总表 `server_performance` 我大概能当场列出来——首先是 `server_name` 标识哪台机器，然后是八个 CPU 字段：总百分比、usr%、system%、nice%、idle%、iowait%、irq%、softirq%，三个负载 `load_avg_1/3/15`，四个内存字段 `mem_used_percent、total、free、avail`，磁盘这里只存了一个 `disk_util_percent`，是所有磁盘里取的最大值。网络四个字段：收发速率和收发包速率。然后是核心的 `score` 评分，0 到 100。再就是六个变化率字段。最后 `timestamp` 时间戳。主键就是自增 id。

网络明细表 `server_net_detail` 存 `server_name + net_name + 收发速率四个字段 + timestamp`。磁盘明细表 `server_disk_detail` 存 `server_name + disk_name + 读写吞吐、IOPS、平均延迟、利用率 + timestamp`。

<details>
<summary>源码参考（点击展开）</summary>

`/home/celeste/project/perfermance-monitor/manager/sql/init_server_performance.sql`

```sql
CREATE TABLE server_performance (
    id INT AUTO_INCREMENT PRIMARY KEY,
    server_name VARCHAR(255) NOT NULL,
    cpu_percent FLOAT DEFAULT 0,
    usr_percent FLOAT DEFAULT 0,
    system_percent FLOAT DEFAULT 0,
    nice_percent FLOAT DEFAULT 0,
    idle_percent FLOAT DEFAULT 0,
    io_wait_percent FLOAT DEFAULT 0,
    irq_percent FLOAT DEFAULT 0,
    soft_irq_percent FLOAT DEFAULT 0,
    load_avg_1 FLOAT DEFAULT 0,
    load_avg_3 FLOAT DEFAULT 0,
    load_avg_15 FLOAT DEFAULT 0,
    mem_used_percent FLOAT DEFAULT 0,
    total FLOAT DEFAULT 0,
    mem_free FLOAT DEFAULT 0,
    avail FLOAT DEFAULT 0,
    disk_util_percent FLOAT DEFAULT 0,
    send_rate FLOAT DEFAULT 0,
    rcv_rate FLOAT DEFAULT 0,
    send_packets_rate FLOAT DEFAULT 0,
    rcv_packets_rate FLOAT DEFAULT 0,
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
);
```

</details>

---

## 二、索引设计

### 面试官：表上建了什么索引？为什么这么建？

**口述回答：**

总表上两个索引。第一个是 `(server_name, timestamp)` 联合索引，第二个是 `score` 单列索引。

为什么这么建？我这个项目所有查询模式全都是"查某台机器在某个时间范围内的数据"，WHERE 子句永远长这样：`server_name = ? AND timestamp BETWEEN ? AND ?`。所以联合索引第一列放 server_name 做精确匹配缩小范围，第二列放 timestamp 做范围扫描，一次索引覆盖完。如果反过来 `(timestamp, server_name)`——查单台机器的时候得先扫描所有时间戳范围内的数据，再过滤 server_name，扫描量大很多。

score 索引是给运维用的——比如说查当前得分最低的几台机器，`WHERE score < 30 ORDER BY score ASC`。

网络和磁盘明细多了一个 `(server_name, net_name, timestamp)` 组合索引，因为查询会精确到某台机器的某块网卡或磁盘。

**一句话：每个索引都是按实际查询 WHERE 子句建的，没有多余的。**

---

## 三、写入方式

### 面试官：数据是怎么落库的？每条 push 来一条 INSERT 还是批量写？

**口述回答：**

逐条 INSERT，不做批量攒批。后台有一个 MySQL 写线程 `MysqlWriteLoop`，从队列里 pop 一个任务，然后按顺序执行三条 INSERT——先写总表一条，再循环写网络明细表（每块网卡一条），再循环写磁盘明细表（每块盘一条）。存的是每轮采样的原始快照，不是汇总值。

为什么没做批量写？当前数据量小，一台 worker 一秒一条总表加几条明细，INSERT 本身开销不是瓶颈。但这个设计我也清楚不适合大规模——如果 10000 台 worker，第一改的就是这条，改成攒 100 条一批用一个 INSERT 写进去，fsync 次数直接降到百分之一。

<details>
<summary>源码参考（点击展开）</summary>

**队列消费：**
`/home/celeste/project/perfermance-monitor/manager/src/host_manager.cpp:117-170`

```cpp
void HostManager::MysqlWriteLoop() {
    MYSQL* conn = nullptr;
    while (true) {
        MysqlWriteTask task;
        {
            std::unique_lock<std::mutex> lock(mysql_queue_mtx_);
            mysql_queue_cv_.wait(lock, [this] {
                return !running_ || !mysql_write_queue_.empty();
            });
            if (!running_ && mysql_write_queue_.empty()) break;
            task = std::move(mysql_write_queue_.front());
            mysql_write_queue_.pop_front();
        }
        // 长连接，断连自动重连
        if (!conn) { /* mysql_real_connect ... */ }
        WriteToMysql(conn, task.host_name, task.host_score, ...);
    }
}
```

**逐条 INSERT：**
`/home/celeste/project/perfermance-monitor/manager/src/host_manager.cpp:531-601`

```cpp
// 1. INSERT INTO server_performance ...
oss << "INSERT INTO server_performance (server_name, cpu_percent, ..., timestamp) VALUES ('"
    << host_name << "'," << cpu_percent << ",...,'" << time_buf << "')";
mysql_query(conn, oss.str().c_str());

// 2. INSERT INTO server_net_detail ... (per NIC)
for (int i = 0; i < info.net_info_size(); ++i) {
    // INSERT INTO server_net_detail ... VALUES ...
    mysql_query(conn, oss.str().c_str());
}

// 3. INSERT INTO server_disk_detail ... (per disk)
for (int i = 0; i < info.disk_info_size(); ++i) {
    // INSERT INTO server_disk_detail ... VALUES ...
    mysql_query(conn, oss.str().c_str());
}
```

</details>

---

## 四、查询场景（现场写 SQL）

### 面试官："查某台服务器过去 1 小时 CPU 均值"，你能把这个 SQL 写出来吗？

**口述回答：**

```sql
SELECT AVG(cpu_percent) AS avg_cpu
FROM server_performance
WHERE server_name = 'host01_192.168.1.10'
  AND timestamp >= NOW() - INTERVAL 1 HOUR;
```

肯定能走索引，`(server_name, timestamp)` 联合索引正好覆盖这个查询。先按 server_name 精确定位，再按 timestamp 做范围扫描。

---

**追问："那如果查所有服务器的过去 1 小时 CPU 均值呢？"**

```sql
SELECT server_name, AVG(cpu_percent) AS avg_cpu
FROM server_performance
WHERE timestamp >= NOW() - INTERVAL 1 HOUR
GROUP BY server_name;
```

这个查询 `(server_name, timestamp)` 索引帮不上忙——WHERE 条件里只有 timestamp，索引第一列是 server_name 用不上。如果这种跨机器的聚合查询频繁，得加一个 `(timestamp, server_name)` 反向索引。不过当前项目不查这种跨服务器聚合，所以没加。

---

**追问："查某台机器过去一周磁盘利用率超过 80% 的时刻"**

```sql
SELECT timestamp, disk_util_percent
FROM server_performance
WHERE server_name = 'host01_192.168.1.10'
  AND timestamp >= NOW() - INTERVAL 7 DAY
  AND disk_util_percent > 80
ORDER BY timestamp DESC;
```

联合索引先过滤 server_name 再扫 timestamp 范围，disk_util_percent 的条件在回表后过滤。

---

**追问："查某台机器的某块网卡过去 1 小时收发速率"**

```sql
SELECT timestamp, rcv_bytes_rate, snd_bytes_rate
FROM server_net_detail
WHERE server_name = 'host01_192.168.1.10'
  AND net_name = 'eth0'
  AND timestamp >= NOW() - INTERVAL 1 HOUR
ORDER BY timestamp ASC;
```

这走到 `(server_name, net_name, timestamp)` 那个三列索引，精确匹配前两列，第三列范围扫描，效率最高。

---

## 五、追问

### Q: 历史数据怎么处理？定期删还是归档？表有没有做分区？

**口述回答：**

当前开发测试阶段都没做——没有分区，没有自动清理，手动清库就行。

但是生产化的话我很清楚要怎么做。第一步在 `server_performance` 上按 `timestamp` 做 RANGE 分区，按天切，保留 30 天。每天凌晨自动创建一个明天的分区，同时 DROP 掉 31 天前那个分区。不需要删数据——DROP PARTITION 是直接删文件，比 `DELETE FROM ... WHERE ...` 快无数倍。网络和磁盘明细表同理。

如果还有归档需求，就在 DROP 之前用 `mysqldump` 把那个分区导出再删。

---

### Q: 多个 worker 同时写入会不会有锁竞争？

**口述回答：**

MySQL 层不会——InnoDB 是行级锁，不同 worker 写不同行，互不阻塞。

Manager 层我是单消费者模型——一个 gRPC handler 线程把写入任务推到 deque 里，单独的 `MysqlWriteLoop` 线程一个一个 pop 出来执行。入队的时候用 mutex 保护，消费端是单线程顺序写，不会有死锁。

但有个点可以说——同一台 worker 补推 30 条历史加 1 条当前，31 条都是同一个 server_name，如果 timestamp 精度只到秒，可能有两条落到同一秒。当前项目没做去重，不会报错但会有重复记录。生产化的话加一个 `(server_name, timestamp)` 的 UNIQUE KEY，配合 `INSERT ... ON DUPLICATE KEY UPDATE` 做幂等写入。

---

### Q: 如果写入延迟突然变大，你怎么定位？

**口述回答：**

分三步。

第一步先看是不是 MySQL 的问题——看 manager 侧 MySQL 写入队列的长度。如果队列在持续涨，说明生产者比消费者快，瓶颈在 MySQL 写入本身。如果队列基本空的，那延迟是 gRPC 链路或者评分计算的问题，跟 MySQL 无关。

第二步定位 MySQL 慢在哪。开 slow query log 看哪条 INSERT 变慢了。可能的原因：磁盘 IO 抖了——用 iostat 看 await 和 util%；或者 buffer pool 不够导致大量刷脏页——看 InnoDB 的 buffer pool hit rate。

第三步修。如果确认是磁盘打满了，优先做批量写入——当前逐条 INSERT 每条都会触发 redo log flush，改成 100 条一批组装一个 INSERT，IO 次数直接降到百分之一，效果立竿见影。

另外我也知道这个项目本身缺少写入延迟的监控——这是一个改进点。应该在 `MysqlWriteLoop` 里加耗时埋点，把 p50/p99 延迟暴露出来，方便排障。
