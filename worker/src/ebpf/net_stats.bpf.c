// SPDX-License-Identifier: GPL-2.0
/**
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：TC hook 采集网卡维度流量，socket 层 kprobe 做进程维度归因。
 *
 * 两类 hook 覆盖两个互补的观测维度：
 *
 *   TC ingress/egress   包路径视角   哪块网卡、收发多少字节/包
 *   kprobe/kretprobe    socket 层    哪个进程、收发多少字节
 *
 * 为什么要做进程维度：这是 /proc 给不了的。
 *   - /proc/net/dev 只有设备级总量，SSH、NFS、备份和业务流量全混在一起；
 *   - /proc/<pid>/net/dev 是 per-netns 而不是 per-process，
 *     同一 netns 下所有进程读到的是同一份数据，无法用于归因。
 *
 * 两张 map 都用 BPF_MAP_TYPE_PERCPU_HASH，理由见各自定义处。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* TC 返回值：放行，不对数据包做任何修改 */
#define TC_ACT_OK 0

/* ============================================================
 * 网卡维度：TC ingress / egress hook
 * ============================================================ */

/* 与用户空间共享的网卡统计结构体 */
struct net_stats {
    __u64 rcv_bytes;      /* 接收字节数（累计） */
    __u64 rcv_packets;    /* 接收包数（累计） */
    __u64 snd_bytes;      /* 发送字节数（累计） */
    __u64 snd_packets;    /* 发送包数（累计） */
};

/*
 * key = ifindex，value = net_stats
 *
 * 用 PERCPU_HASH 而不是普通 HASH：TC hook 是每包触发，而 ifindex 基数极低
 * （通常就 1~2 块物理网卡），所有 CPU 都在写同一块网卡的同一个 value。
 * 普通 HASH 只能用原子操作保证并发安全，这条 cacheline 会在各核之间反复弹；
 * PERCPU_HASH 让每个 CPU 只写自己的副本，lookup 返回的就是本 CPU 的槽位，
 * 热路径退化成一条普通加法。
 * 代价：每个条目占 nr_cpus * sizeof(value) 内存，用户态读的时候要跨 CPU 归并。
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 64);        /* 最多 64 块网卡 */
    __type(key, __u32);             /* ifindex */
    __type(value, struct net_stats);
} net_stats_map SEC(".maps");

/*
 * 累计一块网卡的收发数据。
 *
 * stats 指向的是当前 CPU 的私有副本，所以下面直接做普通加法，
 * 不需要 __atomic_fetch_add —— 这正是选 PERCPU_HASH 的意义。
 */
static __always_inline void update_stats(__u32 ifindex, __u32 len, bool is_rx)
{
    struct net_stats *stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
    if (!stats) {
        /* 首次看到此网卡：初始化累计值。
         * 用 BPF_NOEXIST，避免和另一个 CPU 的创建互相覆盖。 */
        struct net_stats init = {};
        if (is_rx) {
            init.rcv_bytes = len;
            init.rcv_packets = 1;
        } else {
            init.snd_bytes = len;
            init.snd_packets = 1;
        }

        if (bpf_map_update_elem(&net_stats_map, &ifindex, &init, BPF_NOEXIST) == 0)
            return;

        /* 别的 CPU 抢先创建了条目，重新查一次，避免丢掉这次统计 */
        stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
        if (!stats)
            return;
    }

    if (is_rx) {
        stats->rcv_bytes += len;
        stats->rcv_packets += 1;
    } else {
        stats->snd_bytes += len;
        stats->snd_packets += 1;
    }
}

/*
 * TC Ingress Hook - 入方向流量统计
 * 数据包从网卡进入协议栈时触发，skb->ifindex 是入口网卡。
 */
SEC("tc/ingress")
int tc_ingress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;

    /* 过滤无效数据 */
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    update_stats(ifindex, len, true);
    return TC_ACT_OK;
}

/*
 * TC Egress Hook - 出方向流量统计
 * 数据包从协议栈发往网卡时触发。
 */
SEC("tc/egress")
int tc_egress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;

    /* 过滤无效数据 */
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    update_stats(ifindex, len, false);
    return TC_ACT_OK;
}

/* ============================================================
 * 进程维度：socket 层 kprobe / kretprobe
 * ============================================================ */

/* 进程名长度，与内核 TASK_COMM_LEN 一致 */
#define PROC_COMM_LEN 16

/* 与用户空间共享的进程统计结构体 */
struct proc_net_stats {
    __u64 snd_bytes;            /* 发送字节数（累计） */
    __u64 rcv_bytes;            /* 接收字节数（累计） */
    char  comm[PROC_COMM_LEN];  /* 进程名，只在条目创建时写一次 */
};

/*
 * key = tgid，value = proc_net_stats
 *
 * 和网卡维度一样用 PERCPU_HASH：tcp_sendmsg 每次 write 都会触发，
 * 线程池模型的服务会把流量集中到同一个 tgid 上，同样没必要在热路径做原子操作。
 * 代价同上：内存按 CPU 数放大，用户态要跨 CPU 归并。
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);                 /* tgid */
    __type(value, struct proc_net_stats);
} proc_net_map SEC(".maps");

/*
 * 把收发字节数累计到当前进程。
 *
 * bpf_get_current_pid_tgid() 低 32 位是 tid（线程 ID），高 32 位是 tgid（进程 ID）。
 * 这里按 tgid 聚合，保证多线程进程只占一个条目。
 */
static __always_inline void account_proc(__u64 bytes, bool is_send)
{
    if (bytes == 0)
        return;

    __u32 tgid = bpf_get_current_pid_tgid() >> 32;

    struct proc_net_stats *stats = bpf_map_lookup_elem(&proc_net_map, &tgid);
    if (!stats) {
        /* 首次看到该进程：带上进程名初始化，同样用 BPF_NOEXIST 防重复创建 */
        struct proc_net_stats init = {};
        if (is_send)
            init.snd_bytes = bytes;
        else
            init.rcv_bytes = bytes;
        bpf_get_current_comm(&init.comm, sizeof(init.comm));

        if (bpf_map_update_elem(&proc_net_map, &tgid, &init, BPF_NOEXIST) == 0)
            return;

        /* 别的 CPU 抢先创建了条目，重新查一次，避免丢掉这次统计 */
        stats = bpf_map_lookup_elem(&proc_net_map, &tgid);
        if (!stats)
            return;
    }

    if (is_send)
        stats->snd_bytes += bytes;
    else
        stats->rcv_bytes += bytes;
}

/*
 * 发送方向：kprobe/tcp_sendmsg
 *
 * 为什么挂在 socket 层而不是在 TC 里解析 IP 包：
 * 1. TC 拿不到发起这次收发的进程上下文，pid 只在 socket 层才有；
 * 2. TC 里要用 bpf_skb_load_bytes 逐包解析 IP/TCP 头，还得处理 VLAN / IPv6，
 *    开销和出错面都更大；
 * 3. tcp_sendmsg 是每次 write 触发一次，不是每包一次，频率低一个数量级。
 *
 * 统计口径是应用层交给 TCP 的字节数；重传走 tcp_retransmit_skb，不会重复计数。
 * 当前只覆盖 TCP，没挂 udp_sendmsg/udp_recvmsg——业务流量以 TCP 为主
 * （HTTP/gRPC/MySQL/Redis），补 UDP 只需再加两个 kprobe。
 */
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(trace_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    account_proc(size, true);
    return 0;
}

/*
 * 接收方向：kretprobe/tcp_recvmsg
 *
 * 接收的字节数是函数返回值（本次实际拷贝到用户态的字节数），
 * kprobe 入口拿不到，所以必须用 kretprobe 在返回时取值。
 * 返回值 <= 0 表示出错或对端关闭，直接跳过。
 */
SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(trace_tcp_recvmsg, int ret)
{
    if (ret > 0)
        account_proc((__u64)ret, false);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
