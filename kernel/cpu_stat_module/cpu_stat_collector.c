/*
 * 文件归类：内核模块实现。
 * 说明：周期采集 CPU 原始累计时间，并通过字符设备共享给用户态。
 */

#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/timekeeping.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include "shared/cpu_stat_shared.h"
//1s
#define CPU_STAT_SAMPLE_INTERVAL_NS 1000000000LL

static struct cpu_stat_shared_region *g_shared_region;
static struct hrtimer g_sample_timer;

// 返回共享内存区域按页对齐后的映射大小。
static unsigned long cpu_stat_region_size(void)
{
	return PAGE_ALIGN(sizeof(struct cpu_stat_shared_region));
}

// 将内核 cpustat 的时间单位统一转换为微秒。
static u64 cpu_time_units_to_us(u64 value)
{
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	return div_u64(value, NSEC_PER_USEC);
#else
	return jiffies_to_usecs(value);
#endif
}

// 读取单个 CPU 的传统累计时间字段，并统一转换为微秒。
static u64 read_cpustat_us(const struct kernel_cpustat *cpustat,
			   enum cpu_usage_stat field,
			   int cpu)
{
	return cpu_time_units_to_us(kcpustat_field((struct kernel_cpustat *)cpustat,
						      field, cpu));
}

// 采集单个 CPU 的原始累计时间快照。
static void collect_cpu_stat_raw(int cpu, struct cpu_stat_shared_raw *dst)
{
	struct kernel_cpustat cpustat;
	u64 idle_us;
	u64 iowait_us;

	if (!dst)
		return;

	memset(dst, 0, sizeof(*dst));
	kcpustat_cpu_fetch(&cpustat, cpu);

	dst->user = read_cpustat_us(&cpustat, CPUTIME_USER, cpu);
	dst->nice = read_cpustat_us(&cpustat, CPUTIME_NICE, cpu);
	dst->system = read_cpustat_us(&cpustat, CPUTIME_SYSTEM, cpu);
	dst->irq = read_cpustat_us(&cpustat, CPUTIME_IRQ, cpu);
	dst->softirq = read_cpustat_us(&cpustat, CPUTIME_SOFTIRQ, cpu);
	dst->steal = read_cpustat_us(&cpustat, CPUTIME_STEAL, cpu);
	dst->guest = read_cpustat_us(&cpustat, CPUTIME_GUEST, cpu);
	dst->guest_nice = read_cpustat_us(&cpustat, CPUTIME_GUEST_NICE, cpu);

	idle_us = get_cpu_idle_time_us(cpu, NULL);
	if (idle_us == (u64)-1)
		idle_us = read_cpustat_us(&cpustat, CPUTIME_IDLE, cpu);
	dst->idle = idle_us;

	iowait_us = get_cpu_iowait_time_us(cpu, NULL);
	if (iowait_us == (u64)-1)
		iowait_us = read_cpustat_us(&cpustat, CPUTIME_IOWAIT, cpu);
	dst->iowait = iowait_us;
}

// 采集所有 CPU，并将结果写入共享内存中的稳定快照。
static void collect_all_cpu_stats(void)
{
	u32 cpu_count;
	u32 seq_before;
	u32 cpu;

	if (!g_shared_region)
		return;

	cpu_count = min_t(u32, num_possible_cpus(), CPU_STAT_MAX_CPUS);
	seq_before = READ_ONCE(g_shared_region->seq);

	WRITE_ONCE(g_shared_region->seq, seq_before + 1);
	smp_wmb();

	g_shared_region->version = CPU_STAT_SHARED_VERSION;
	g_shared_region->cpu_count = cpu_count;
	g_shared_region->sample_ns = ktime_get_ns();

	for (cpu = 0; cpu < cpu_count; ++cpu)
		collect_cpu_stat_raw(cpu, &g_shared_region->cpus[cpu]);

	smp_wmb();
	WRITE_ONCE(g_shared_region->seq, seq_before + 2);
}

// 高精度定时器回调，每秒刷新一次共享内存快照。
static enum hrtimer_restart cpu_stat_timer_callback(struct hrtimer *timer)
{
	collect_all_cpu_stats();
	hrtimer_forward_now(timer, ns_to_ktime(CPU_STAT_SAMPLE_INTERVAL_NS));
	return HRTIMER_RESTART;
}

// 处理用户态对 /dev/cpu_stat_monitor 的 mmap 请求。
static int cpu_stat_monitor_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long requested_size;

	if (!g_shared_region)
		return -ENODEV;

	requested_size = vma->vm_end - vma->vm_start;
	if (requested_size > cpu_stat_region_size())
		return -EINVAL;
		return remap_vmalloc_range(vma, g_shared_region, 0);

}

static const struct file_operations cpu_stat_monitor_fops = {
	.owner = THIS_MODULE,
	.mmap = cpu_stat_monitor_mmap,
};

static struct miscdevice cpu_stat_monitor_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cpu_stat_monitor",
	.fops = &cpu_stat_monitor_fops,
	.mode = 0444,
};

// 初始化共享内存、字符设备和定时采集器。
static int __init cpu_stat_collector_init(void)
{
	int ret;
	//申请共享内存
	g_shared_region = vmalloc_user(cpu_stat_region_size());
	if (!g_shared_region)
		return -ENOMEM;

	memset(g_shared_region, 0, cpu_stat_region_size());

	ret = misc_register(&cpu_stat_monitor_device);
	if (ret) {
		vfree(g_shared_region);
		g_shared_region = NULL;
		return ret;
	}

	collect_all_cpu_stats();

	hrtimer_init(&g_sample_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	g_sample_timer.function = cpu_stat_timer_callback;
	hrtimer_start(&g_sample_timer, ns_to_ktime(CPU_STAT_SAMPLE_INTERVAL_NS),
		      HRTIMER_MODE_REL);

	return 0;
}

// 注销定时器和字符设备，并释放共享内存。
static void __exit cpu_stat_collector_exit(void)
{
	hrtimer_cancel(&g_sample_timer);
	misc_deregister(&cpu_stat_monitor_device);

	if (g_shared_region) {
		vfree(g_shared_region);
		g_shared_region = NULL;
	}
}

module_init(cpu_stat_collector_init);
module_exit(cpu_stat_collector_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CPU stat collector with mmap shared memory");
