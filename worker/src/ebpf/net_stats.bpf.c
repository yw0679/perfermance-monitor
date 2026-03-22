// SPDX-License-Identifier: GPL-2.0
/*
 * 文件归类：eBPF 模块文件（当前网络采集主线）
 * 说明：worker 通过 TC ingress/egress + eBPF map 统计网卡流量。
 */
/*
 * net_stats.bpf.c - 基于 TC Hook 的 eBPF 网络流量统计程序
 *
 * 功能：
 * 1. 使用 TC (Traffic Control) hook 挂载到网络协议栈
 * 2. 在 ingress/egress 方向分别统计流量
 * 3. 通过 BPF map 将数据暴露给用户空间
 *
 * Hook 点：
 * - TC ingress: 入方向（接收）- 数据包进入协议栈时
 * - TC egress: 出方向（发送）- 数据包离开协议栈时
 *
 * TC hook 位于网络协议栈的 L2/L3 边界，可以：
 * - 访问完整的 skb 结构
 * - 修改、丢弃、重定向数据包
 * - 获取精确的网卡信息
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* TC 返回值 */
#define TC_ACT_OK       0
#define TC_ACT_SHOT     2
#define TC_ACT_UNSPEC   -1

/* 网络统计结构体 - 与用户空间共享 */
struct net_stats {
    __u64 rcv_bytes;      /* 接收字节数 */
    __u64 rcv_packets;    /* 接收包数 */
    __u64 snd_bytes;      /* 发送字节数 */
    __u64 snd_packets;    /* 发送包数 */
};

/*
 * BPF Hash Map: key = ifindex (网卡索引), value = net_stats
 *
 * 这里选择 HASH 而不是 ARRAY/QUEUE/RINGBUF：
 * 1. ifindex 是整数，但通常不连续，用 HASH 更节省空间；
 * 2. 每块网卡都需要按 key 单独维护一份累计状态；
 * 3. 用户空间会再通过 if_indextoname() 将 ifindex 转成网卡名。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);        /* 最多支持 64 个网卡 */
    __type(key, __u32);             /* ifindex */
    __type(value, struct net_stats);
} net_stats_map SEC(".maps");

/*
 * 更新网卡统计数据
 * 使用原子操作确保多 CPU 并发安全
 */
static __always_inline void update_stats(__u32 ifindex, __u32 len, bool is_rx)
{
    struct net_stats *stats;
    struct net_stats new_stats = {};

    stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);//根bpfmap的key来查
    if (!stats) {
        /* 首次看到此网卡，初始化统计 */
        if (is_rx) {
            new_stats.rcv_bytes = len;
            new_stats.rcv_packets = 1;
        } else {
            new_stats.snd_bytes = len;
            new_stats.snd_packets = 1;
        }
        bpf_map_update_elem(&net_stats_map, &ifindex, &new_stats, BPF_ANY);
    } else {
        /* 使用原子操作更新已有统计 */
        if (is_rx) {
            __atomic_fetch_add(&stats->rcv_bytes,len , __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&stats->rcv_packets,1 , __ATOMIC_SEQ_CST);
        } else {
            __atomic_fetch_add(&stats->snd_bytes, len, __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&stats->snd_packets, 1, __ATOMIC_SEQ_CST);
        }
    }
}

/*
 * TC Ingress Hook - 入方向流量统计
 * 
 * 当数据包从网卡进入协议栈时触发
 * ctx->ingress_ifindex 包含入口网卡的 ifindex
 */
SEC("tc/ingress")//对libbpf申明下面函数放在tc/ingress段
int tc_ingress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;

    /* 过滤无效数据 */
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    update_stats(ifindex, len, true);

    /* TC_ACT_OK: 继续正常处理，不影响数据包 */
    return TC_ACT_OK;
}

/*
 * TC Egress Hook - 出方向流量统计
 * 
 * 当数据包从协议栈发送到网卡时触发
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

    /* TC_ACT_OK: 继续正常处理，不影响数据包 */
    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
