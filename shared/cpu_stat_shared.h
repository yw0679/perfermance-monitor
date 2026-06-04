/*
 * 文件归类：内核态与用户态共享头文件。
 * 说明：定义 /dev/cpu_stat_monitor mmap 共享内存布局。
 */

#ifndef SHARED_CPU_STAT_SHARED_H_
#define SHARED_CPU_STAT_SHARED_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define CPU_STAT_SHARED_VERSION 1U
#define CPU_STAT_MAX_CPUS 256U

/* 单个 CPU 的原始累计时间快照。 */
struct cpu_stat_shared_raw {
  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t iowait;
  uint64_t irq;
  uint64_t softirq;
  uint64_t steal;
  uint64_t guest;
  uint64_t guest_nice;
};

/*
 * 整个共享内存区域的布局。
 * seq 可用于用户态做简单一致性检查：
 * 写入前加 1，写完后再加 1，偶数表示稳定快照。
 */
struct cpu_stat_shared_region {
  uint32_t version;
  uint32_t cpu_count;
  uint32_t seq;
  //保留字段
  uint32_t reserved;
  //采样的时间戳
  uint64_t sample_ns;
  struct cpu_stat_shared_raw cpus[CPU_STAT_MAX_CPUS];
};

#endif  // SHARED_CPU_STAT_SHARED_H_
