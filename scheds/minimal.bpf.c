/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minimal.bpf.c — 一个能跑的最小 sched_ext 调度器
 *
 * 这个文件编译成 BPF 字节码，加载进内核后接管 CPU 调度。
 * 策略简单到不能再简单：所有任务进同一个全局队列，先来先跑（FIFO）。
 * 目的不是好用，是把 sched_ext 的接口用最少的代码走一遍，
 * 让你看清内核到底交出了哪几个决策点。
 *
 * 编译：make
 * 运行：sudo ./minimal ./minimal.bpf.o
 *
 * 依赖内核源码树的 include 目录（Makefile 里的 SCX_INCLUDE），
 * 原因见下面 scx/common.bpf.h 那段注释。
 */

/*
 * 这一个头文件里包含四样东西：
 *
 *   1. vmlinux.h —— 从运行中内核的 BTF 导出的类型定义，
 *      有 struct task_struct、struct sched_ext_ops、SCX_DSQ_LOCAL 等。
 *
 *   2. bpf_helpers.h / bpf_tracing.h —— libbpf 的 SEC()、BPF_STRUCT_OPS()。
 *
 *   3. 所有 scx_bpf_ 开头的 kfunc 声明。
 *      这些函数实现在内核的 kernel/sched/ext.c 里，
 *      BPF 程序通过 __ksym 引用，加载时由 libbpf 解析成内核地址。
 *      自己写 extern 声明容易和内核版本对不上，所以直接用官方头。
 *
 *   4. SCX_OPS_DEFINE 宏，用来声明那张回调表。
 *
 * 注意：libbpf 1.2 的 bpf_tracing.h 里没有 BPF_STRUCT_OPS 宏，
 * 它只在 sched_ext 自己的头文件里定义。这是必须引入 SCX_INCLUDE 的直接原因。
 */
#include <scx/common.bpf.h>

/* 内核只接受 GPL 兼容许可证，写别的加载会失败 */
char _license[] SEC("license") = "GPL";

/* ------------------------------------------------------------------
 * 常量
 * ------------------------------------------------------------------ */

/*
 * 我们自己建的全局队列编号。
 * 为什么不用现成的 SCX_DSQ_GLOBAL？因为内置 DSQ 不能当优先级队列用，
 * 后面想改成按 vtime 排序就得自己建一个。这里先用 0 占位。
 */
#define SHARED_DSQ 0

/* ------------------------------------------------------------------
 * 统计用的 BPF map
 *
 * 为什么需要它？因为"加载成功"和"真的在调度"是两件事。
 * 这个 map 在回调里计数，用户态每秒读一次：
 * 数字在涨，就证明内核真的在调用你的 BPF 函数，而不是加载完晾着。
 *
 * PERCPU_ARRAY 表示每个 CPU 一份独立副本，写的时候不用加锁。
 * ------------------------------------------------------------------ */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2);
} stats SEC(".maps");

/* map 的两个下标 */
enum {
	STAT_LOCAL  = 0,   /* 直接塞进某个 CPU 本地队列的次数 */
	STAT_SHARED = 1,   /* 塞进全局队列的次数 */
};

/* 取当前 CPU 的计数器并 +1。取不到就静默返回，不能让统计失败影响调度。 */
static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
}

/* ==================================================================
 * 回调 1：select_cpu —— 任务要跑了，给它挑个核
 *
 * 调用时机：任务从睡眠中被唤醒、或者刚被创建，内核要决定放哪个 CPU。
 * 这是 sched_ext 交出的第一个决策点。
 *
 * 参数：
 *   p          要调度的任务
 *   prev_cpu   它上次跑在哪个核（缓存亲和性通常希望还回这个核）
 *   wake_flags 唤醒标志，比如 WF_SYNC 表示唤醒者要让出 CPU
 *
 * 返回：选中的 CPU 号
 * ================================================================== */
s32 BPF_STRUCT_OPS(minimal_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	/*
	 * 不自己实现选核逻辑，借内核的默认实现。
	 * 它会考虑缓存亲和性、空闲核、NUMA 距离。
	 * 真实调度器（如 scx_lavd）在这里写自己的逻辑，最小实现没必要重复造轮子。
	 *
	 * is_idle 是出参：回填挑中的那个核当时是否空闲。
	 */
	bool is_idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

	/*
	 * 如果那个核当时空闲，就别绕远路了 —— 直接把任务塞进它的本地 DSQ。
	 * SCX_DSQ_LOCAL 会被解析成"当前 CPU 的本地队列"。
	 *
	 * 这一步是性能关键：不这么做，任务要先入全局队列，
	 * 等那个核下次触发 dispatch 才能被取走，中间多一次唤醒延迟。
	 */
	if (is_idle) {
		stat_inc(STAT_LOCAL);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}

	return cpu;
}

/* ==================================================================
 * 回调 2：enqueue —— 任务要进队列了
 *
 * 调用时机：任务变成可运行状态，需要排队等 CPU。
 * 这是第二个决策点：**放哪个队列**。
 *
 * 参数：
 *   p         要入队的任务
 *   enq_flags 入队原因（新任务、被唤醒、时间片用完、被抢占等）
 * ================================================================== */
void BPF_STRUCT_OPS(minimal_enqueue, struct task_struct *p, u64 enq_flags)
{
	stat_inc(STAT_SHARED);

	/*
	 * 全部塞进同一个全局队列。
	 * SCX_SLICE_DFL 是默认时间片（20 毫秒）：这个任务这次最多连续跑 20 ms，
	 * 然后回队列重新排。
	 *
	 * 这里就是"策略"的全部：先来先跑，谁先入队谁先出队。
	 * 想按权重排序，把这行换成 scx_bpf_dispatch_vtime()，
	 * 传一个根据 p->scx.weight 算出来的虚拟时间即可。
	 */
	scx_bpf_dispatch(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
}

/* ==================================================================
 * 回调 3：dispatch —— 某个 CPU 空出来了，给它派活
 *
 * 调用时机：一个 CPU 要挑下一个任务执行。
 * 这是第三个决策点：**从队列里取谁**。
 *
 * 参数：
 *   cpu  正在挑活的 CPU
 *   prev 刚从这个 CPU 上下来的任务（可能为 NULL）
 * ================================================================== */
void BPF_STRUCT_OPS(minimal_dispatch, s32 cpu, struct task_struct *prev)
{
	/*
	 * 从全局队列取一个。取到了内核就把它切上去；
	 * 没取到（队列空）返回 false，内核让这个 CPU 进 idle。
	 *
	 * 注意这里没有循环：一次 dispatch 只取一个。
	 * 想批量取可以写 while，但必须有可证明的上界，
	 * 否则 verifier 会因为"无法证明循环会结束"而拒绝加载。
	 */
	scx_bpf_consume(SHARED_DSQ);
}

/* ==================================================================
 * 回调 4：init —— 调度器注册前的一次性初始化
 *
 * SLEEPABLE 后缀表示允许睡眠，所以能调用会分配内存的 kfunc。
 * create_dsq 就要分配内存，只能在这里调。
 * ================================================================== */
s32 BPF_STRUCT_OPS_SLEEPABLE(minimal_init)
{
	/*
	 * 建一个全局队列，编号 SHARED_DSQ。
	 * 第二个参数是 NUMA 节点，-1 表示不绑定节点。
	 * 返回 0 成功；非 0 会导致整个调度器加载失败。
	 */
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

/* ==================================================================
 * 回调 5：exit —— 调度器被卸载时的收尾
 *
 * ei 里带着退出原因：用户态主动退出、出错、被看门狗踢掉等。
 * 最小实现里什么都不做，但真实调度器一定要在这里打印原因，
 * 否则出问题时你根本不知道为什么被卸载。
 * ================================================================== */
void BPF_STRUCT_OPS(minimal_exit, struct scx_exit_info *ei)
{
}

/* ==================================================================
 * 把上面这些函数注册成内核认得的回调表
 *
 * SEC(".struct_ops.link") 是 libbpf 约定的段名。
 * 加载时 libbpf 会读这张表，用 BTF 检查每个函数指针的签名
 * 跟内核 struct sched_ext_ops 里对应的成员是否一致，然后注册上去。
 * 类型对不上，加载直接失败 —— 这就是第 2 篇强调 BTF 的原因。
 *
 * .name 会出现在 /sys/kernel/sched_ext/root/ops 里，用来确认谁在跑。
 * ================================================================== */
SCX_OPS_DEFINE(minimal_ops,
	       .select_cpu = (void *)minimal_select_cpu,
	       .enqueue    = (void *)minimal_enqueue,
	       .dispatch   = (void *)minimal_dispatch,
	       .init       = (void *)minimal_init,
	       .exit       = (void *)minimal_exit,
	       .name       = "minimal");
