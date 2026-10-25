/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_prio.bpf.c — 给推理任务让路的 sched_ext 调度器
 *
 * 目标：机器被一堆批处理任务占满时，让一个指定进程（推理引擎）
 *       被唤醒后尽快拿到 CPU，而不是排队等公平份额。
 *
 * 和 minimal.bpf.c 的区别只有三处，但每一处都对应一个机制：
 *
 *   1. 多一个队列。普通任务进 SHARED_DSQ，推理任务进 AI_DSQ。
 *      dispatch 时先取 AI_DSQ，取到了就不再取普通的。
 *      → 这是"排队靠前"。
 *
 *   2. enqueue 里踢一下目标 CPU（KICK_PREEMPT）。
 *      → 这是"打断正在跑的人"，比排队靠前更硬。
 *
 *   3. 两个队列的时间片分开配，再加一条"连续占满 N 片就必须让一次"。
 *      → 这是"预算"：时间片有多长、最多连续占几片，别人最多等多久。
 *
 *   4. 普通任务唤醒时插队头并踢核（wake_boost）。
 *      → 这是把 CFS 原本免费提供的"唤醒抢占 + 按欠账排序"补回来。
 *        换掉调度器等于把 CFS 那套保护拆了，拆了得自己装回去。
 *
 * 四个机制可以单独开关，第 10、11 篇就是靠这个做对照实验的。
 *
 * 编译：make ai_prio
 * 运行：sudo ./ai_prio ./ai_prio.bpf.o <推理进程的 pid>
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/* ------------------------------------------------------------------
 * 队列编号
 *
 * 0 号给普通任务（和 minimal 里那个 SHARED_DSQ 是同一个位置），
 * 1 号留给推理任务。两个都在 init 里创建。
 * ------------------------------------------------------------------ */
#define SHARED_DSQ 0
#define AI_DSQ     1

/*
 * SCX_KICK_PREEMPT 的值。
 *
 * 这个常量定义在内核的 include/linux/sched/ext.h 里，是 enum 成员，
 * 不一定出现在 vmlinux.h 导出的 BTF 里。直接写值比引符号稳：
 * 换内核版本时符号可能不在，值不会变。
 *
 *   1 << 0  SCX_KICK_IDLE     只踢空闲的核
 *   1 << 1  SCX_KICK_PREEMPT  踢完强制重新调度，正在跑的任务被打断
 *   1 << 2  SCX_KICK_WAIT     踢完等它真的调度完
 */
#define KICK_PREEMPT (1 << 1)

/* ------------------------------------------------------------------
 * 配置
 *
 * 放在 map 里而不是写死，是为了让用户态能改：
 * 三个机制各自开关，才能做对照实验。
 * ------------------------------------------------------------------ */
struct cfg {
	u32 target_pid;      /* 要优待的进程（0 = 不优待任何人） */
	u32 kick_on_wake;    /* 1 = 推理任务唤醒时打断目标 CPU */
	u32 ai_burst;        /* 推理任务最多连续占几个时间片（0 = 不限） */
	u32 wake_boost;      /* 1 = 普通任务唤醒时插队头并踢核 */
	u64 ai_slice_ns;     /* 推理任务的时间片，纳秒 */
	u64 other_slice_ns;  /* 普通任务的时间片，纳秒 */
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(struct cfg));
	__uint(max_entries, 1);
} cfg_map SEC(".maps");

/* ------------------------------------------------------------------
 * 每个 CPU 自己的"连续占了几个时间片"计数
 *
 * PERCPU_ARRAY 只放一个元素就够：每个 CPU 查到的都是自己那一份，
 * 不用按 cpu 编号做键，也不用原子操作。
 * ------------------------------------------------------------------ */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u32));
	__uint(max_entries, 1);
} ai_burst SEC(".maps");

/* ------------------------------------------------------------------
 * 统计
 *
 * 五个计数器，分别对应"走到哪一步"：
 *   ST_AI_ENQ    推理任务入队次数
 *   ST_OTHER_ENQ 普通任务入队次数
 *   ST_KICK      推理任务唤醒时踢核次数
 *   ST_AI_PICK   dispatch 从 AI 队列取到任务的次数
 *   ST_OTHER_PICK dispatch 从普通队列取到任务的次数
 *   ST_WAKE_BOOST 普通任务唤醒时插队头 + 踢核的次数
 *
 * 如果 ST_AI_ENQ 在涨但 ST_AI_PICK 不涨，说明任务入了队却没被优先取走，
 * 那就是 dispatch 的逻辑有问题。分开计数就是为了能这样定位。
 *
 * ST_AI_PICK 和 ST_OTHER_PICK 的比例，就是"推理任务占了多少次调度机会"。
 * ------------------------------------------------------------------ */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 6);
} stats SEC(".maps");

enum {
	ST_AI_ENQ = 0,
	ST_OTHER_ENQ = 1,
	ST_KICK = 2,
	ST_AI_PICK = 3,
	ST_OTHER_PICK = 4,
	ST_WAKE_BOOST = 5,
};

static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
}

static __always_inline struct cfg *get_cfg(void)
{
	u32 zero = 0;

	return bpf_map_lookup_elem(&cfg_map, &zero);
}

/*
 * 这个任务是不是要优待的那个。
 *
 * 用 pid 比对，不用 comm 字符串比对。原因：
 *   - comm 只有 15 个字符，会被截断，重名也分不出来
 *   - 字符串比对在内核里是逐字节循环，开销比一次整数比较大得多
 *
 * 注意 task_struct 里的 pid 字段是线程 id，不是进程 id。
 * 单线程的推理引擎两者相同，直接拿 getpid() 就行；
 * 多线程的引擎要把每个线程的 tid 都配进来。
 */
static __always_inline bool is_ai(struct task_struct *p, struct cfg *c)
{
	return c && c->target_pid && p->pid == c->target_pid;
}

/* ==================================================================
 * select_cpu —— 唤醒时挑核
 *
 * 推理任务被唤醒时，如果挑中的核正好空闲，直接塞进它的本地队列。
 * 这一步省掉的是"入全局队列 → 等那个核 dispatch"这一整圈。
 * ================================================================== */
s32 BPF_STRUCT_OPS(ai_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	struct cfg *c = get_cfg();

	if (!c)
		return cpu;

	if (!is_idle)
		return cpu;

	if (is_ai(p, c)) {
		stat_inc(ST_AI_ENQ);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, c->ai_slice_ns, 0);
	} else {
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, c->other_slice_ns, 0);
	}

	return cpu;
}

/* ==================================================================
 * enqueue —— 入队
 *
 * 这里是策略的核心：分道。
 * ================================================================== */
void BPF_STRUCT_OPS(ai_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct cfg *c = get_cfg();

	if (!c) {
		scx_bpf_dispatch(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
		return;
	}

	if (!is_ai(p, c)) {
		u64 fl = enq_flags;
		bool wake = c->wake_boost && (enq_flags & SCX_ENQ_WAKEUP);

		stat_inc(ST_OTHER_ENQ);

		/*
		 * 刚睡醒的任务插到队头。
		 *
		 * 全局队列是按入队顺序排的。一个睡了 40 毫秒的交互式任务醒来时，
		 * 前面可能已经排着 8 个压测线程，每个占 3 毫秒 —— 它要等 24 毫秒。
		 * CFS 不会有这个问题：它按 vruntime 排序，刚睡醒的任务
		 * 欠账最少，天然排在最前面。我们换成自己的队列之后，
		 * 这个"天然"就没了，得自己补。
		 */
		if (wake)
			fl |= SCX_ENQ_HEAD;

		scx_bpf_dispatch(p, SHARED_DSQ, c->other_slice_ns, fl);

		/*
		 * 再踢一脚，让它所在的那个核立刻重新调度。
		 * 只对 SCX_ENQ_WAKEUP 做：被抢占后重新入队的任务不踢，
		 * 否则一次唤醒会引发一连串抢占，白烧 CPU。
		 */
		if (wake) {
			scx_bpf_kick_cpu(scx_bpf_task_cpu(p), KICK_PREEMPT);
			stat_inc(ST_WAKE_BOOST);
		}

		return;
	}

	stat_inc(ST_AI_ENQ);

	/*
	 * 进自己的队列。不是 LOCAL —— LOCAL 是"某个 CPU 的本地队列"，
	 * 得先知道跑哪个核；这里让它进全局的 AI 队列，
	 * 哪个核空了哪个核来取，跨核也能抢。
	 */
	scx_bpf_dispatch(p, AI_DSQ, c->ai_slice_ns, enq_flags);

	/*
	 * 关键一步：踢它所在的核。
	 *
	 * 不踢会怎样？那个核上正在跑一个普通任务，它的时间片可能是 6 ms。
	 * 推理任务得等这 6 ms 过完、那个任务自己让出来，才轮得到。
	 * 也就是说"优先级高"只体现在队列排序上，不影响正在跑的人。
	 *
	 * 踢一下 = 让那个核立刻重新调度。dispatch 会被调用，
	 * 它先看 AI 队列，发现有人，就把当前任务换下去。
	 *
	 * 代价是上下文切换。所以这个开关要能关——第 11 篇会对比开和关。
	 */
	if (c->kick_on_wake) {
		scx_bpf_kick_cpu(scx_bpf_task_cpu(p), KICK_PREEMPT);
		stat_inc(ST_KICK);
	}
}

/* ==================================================================
 * dispatch —— 某个核要挑下一个任务
 *
 * 两条规则，顺序不能换：
 *
 *   1. 预算没用完：先取 AI 队列（这是"优先"）
 *   2. 预算用完了：这一轮先取普通队列（这是"不让它把核焊死"）
 *
 * 为什么光有时间片不够：
 *   时间片到期时，推理任务被重新入队到 AI_DSQ，紧接着 dispatch 又被调用，
 *   第一条规则一看 AI_DSQ 有人，又把它自己取回来 —— 时间片等于白设。
 *   一个 40 毫秒的请求就这么把核按住 40 毫秒，别人的 p99 全是几十毫秒。
 *   第 2 条规则就是为了切断这个循环：连续占满 N 个时间片，必须让一次。
 * ================================================================== */
void BPF_STRUCT_OPS(ai_dispatch, s32 cpu, struct task_struct *prev)
{
	struct cfg *c = get_cfg();
	u32 zero = 0;
	u32 *used = bpf_map_lookup_elem(&ai_burst, &zero);

	if (!used) {
		if (scx_bpf_consume(AI_DSQ)) {
			stat_inc(ST_AI_PICK);
			return;
		}
		scx_bpf_consume(SHARED_DSQ);
		return;
	}

	if (c && c->ai_burst && *used >= c->ai_burst) {
		if (scx_bpf_consume(SHARED_DSQ)) {
			*used = 0;
			stat_inc(ST_OTHER_PICK);
			return;
		}
		/*
		 * 普通队列是空的：让也没人接，只能继续给推理任务。
		 * 预算不清零 —— 下一个普通任务一入队，这一轮就轮到它。
		 */
	}

	if (scx_bpf_consume(AI_DSQ)) {
		(*used)++;
		stat_inc(ST_AI_PICK);
		return;
	}

	if (scx_bpf_consume(SHARED_DSQ)) {
		*used = 0;
		stat_inc(ST_OTHER_PICK);
	}
}

/* ==================================================================
 * init —— 建两个队列
 *
 * 只能在这里建，因为分配内存要能睡眠。
 * 任何一个建失败都要返回非 0，否则调度器会带着半个初始化跑起来。
 * ================================================================== */
s32 BPF_STRUCT_OPS_SLEEPABLE(ai_init)
{
	s32 ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret)
		return ret;

	return scx_bpf_create_dsq(AI_DSQ, -1);
}

void BPF_STRUCT_OPS(ai_exit, struct scx_exit_info *ei)
{
	/*
	 * 真实调度器要在这里把 ei->reason 打出来。
	 * BPF 侧不方便格式化字符串，通常塞进一个 map 让用户态去读，
	 * 最小实现留空，加载器退出时会自己打印原因。
	 */
}

SCX_OPS_DEFINE(ai_prio_ops,
	       .select_cpu = (void *)ai_select_cpu,
	       .enqueue    = (void *)ai_enqueue,
	       .dispatch   = (void *)ai_dispatch,
	       .init       = (void *)ai_init,
	       .exit       = (void *)ai_exit,
	       .name       = "ai_prio");
