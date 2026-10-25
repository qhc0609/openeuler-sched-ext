/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tiny_infer.c — 一个最小但完整的推理引擎
 *
 * 它不是一个玩具计时器，是真的在做前向计算：
 * 两层全连接（128 → 256 → 64），中间 ReLU，权重由固定种子的
 * 线性同余生成器算出来，所以不需要任何模型文件，编译完就能跑，
 * 每次运行的结果也完全一致。
 *
 * 为什么不用 PyTorch：
 *   1. 这个实验要测的是"进程被唤醒后多久拿到 CPU"，
 *      推理框架越重，噪声越大，测出来的东西越不像调度问题。
 *   2. 框架自己的线程池会把"单进程"这件事搞没，
 *      sched_ext 那边按 pid 优待就不成立了。
 *
 * 关键设计：测的不是"算得多快"，而是"该跑的时候多久才真的跑起来"。
 *
 *   每个请求有一个计划时刻 t_target（绝对时间，第 i 个请求 = t0 + i*间隔）。
 *   到点睡着，被唤醒时记 t_wake，算完记 t_done。
 *
 *     sched_latency = t_wake - t_target   ← 纯调度延迟，这才是 sched_ext 影响的量
 *     work_time     = t_done - t_wake     ← 干活时间，跟调度器基本无关
 *
 *   如果只测 t_done - t_target，会把"算得慢"和"排不上队"混在一起，
 *   最后无法判断改善到底来自哪里。
 *
 * 编译：make
 * 用法：./tiny_infer [选项]
 *   --requests N     请求数（默认 600）
 *   --interval-ms N  请求间隔毫秒（默认 10）
 *   --passes N       每个请求重复前向次数（默认 16）
 *   --warmup N       预热请求数，不计入统计（默认 50）
 *   --pidfile PATH   启动后把 pid 写进去，供调度器读取
 *   --wait PATH      等这个文件出现才开始计时（实验同步用）
 *   --json           只输出一行机器可读结果
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IN_DIM   128
#define HID_DIM  256
#define OUT_DIM  64

/* 权重放静态区：W1 有 32768 个 float，放栈上会直接爆 */
static float W1[HID_DIM][IN_DIM];
static float b1[HID_DIM];
static float W2[OUT_DIM][HID_DIM];
static float b2[OUT_DIM];

static float input[IN_DIM];
static float hidden[HID_DIM];
static float output[OUT_DIM];

/* ------------------------------------------------------------------
 * 权重初始化
 *
 * 线性同余生成器，参数是 Numerical Recipes 那组经典值。
 * 用它而不是 rand()，是因为 rand() 在不同 libc 上序列不同，
 * 换台机器跑出来的权重就不一样，"同一份代码同一个结果"就没了。
 * ------------------------------------------------------------------ */
static unsigned int lcg_state = 20261025u;

static float lcg_next(void)
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	/* 取高 16 位映射到 [-1, 1)，低位随机性差，不能用 */
	return ((float)(lcg_state >> 16) / 32768.0f) - 1.0f;
}

static void init_weights(void)
{
	for (int h = 0; h < HID_DIM; h++) {
		for (int i = 0; i < IN_DIM; i++)
			W1[h][i] = lcg_next() * 0.08f;
		b1[h] = lcg_next() * 0.01f;
	}
	for (int o = 0; o < OUT_DIM; o++) {
		for (int h = 0; h < HID_DIM; h++)
			W2[o][h] = lcg_next() * 0.05f;
		b2[o] = lcg_next() * 0.01f;
	}
	for (int i = 0; i < IN_DIM; i++)
		input[i] = lcg_next();
}

/* ------------------------------------------------------------------
 * 一次前向
 *
 * 内层循环写成 row[i] * x[i] 的顺序，而不是 i 在外层，
 * 是为了让同一行的权重在缓存里连续 —— 32768 个 float 有 128 KB，
 * 已经超过 L1，访存顺序直接决定这段代码是几百微秒还是几毫秒。
 * ------------------------------------------------------------------ */
static void forward(void)
{
	for (int h = 0; h < HID_DIM; h++) {
		float s = b1[h];
		const float *row = W1[h];

		for (int i = 0; i < IN_DIM; i++)
			s += row[i] * input[i];

		/* ReLU：负数归零。写成三元而不是 fmaxf，避免多一次函数调用 */
		hidden[h] = s > 0.0f ? s : 0.0f;
	}

	for (int o = 0; o < OUT_DIM; o++) {
		float s = b2[o];
		const float *row = W2[o];

		for (int h = 0; h < HID_DIM; h++)
			s += row[h] * hidden[h];

		output[o] = s;
	}
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

/* 百分位：取排序后第 p% 个，向上取整，避免样本少时取到 0 */
static long long pct(long long *v, int n, int p)
{
	int idx;

	if (n <= 0)
		return 0;
	idx = (int)(((long long)n * p + 99) / 100) - 1;
	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	return v[idx];
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "requests", required_argument, NULL, 'n' },
		{ "interval-ms", required_argument, NULL, 'i' },
		{ "passes", required_argument, NULL, 'p' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "pidfile", required_argument, NULL, 'f' },
		{ "wait", required_argument, NULL, 'W' },
		{ "json", no_argument, NULL, 'j' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int requests = 600, interval_ms = 10, passes = 16, warmup = 50;
	const char *pidfile = NULL, *waitfile = NULL;
	int json = 0, opt;

	while ((opt = getopt_long(argc, argv, "n:i:p:w:f:W:jh", opts, NULL)) != -1) {
		switch (opt) {
		case 'n': requests = atoi(optarg); break;
		case 'i': interval_ms = atoi(optarg); break;
		case 'p': passes = atoi(optarg); break;
		case 'w': warmup = atoi(optarg); break;
		case 'f': pidfile = optarg; break;
		case 'W': waitfile = optarg; break;
		case 'j': json = 1; break;
		default:
			fprintf(stderr, "用法: %s [--requests N] [--interval-ms N] "
				"[--passes N] [--warmup N] [--pidfile PATH] "
				"[--wait PATH] [--json]\n", argv[0]);
			return 2;
		}
	}

	if (requests <= 0 || interval_ms <= 0 || passes <= 0)
		return 2;

	init_weights();

	/*
	 * 把 pid 交出去。调度器要靠它决定优待谁，
	 * 所以必须在真正开始计时之前写好。
	 */
	if (pidfile) {
		FILE *f = fopen(pidfile, "w");

		if (!f) {
			fprintf(stderr, "写不了 %s: %s\n", pidfile, strerror(errno));
			return 1;
		}
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}

	/*
	 * 等开始信号。
	 *
	 * 为什么需要这个握手：调度器必须在我们开始计时之前就挂好。
	 * 如果先跑再挂，前几十个请求是在没有调度器的状态下测的，
	 * 会被算进统计，把结论稀释掉。
	 *
	 * 用 usleep(2000) 轮询而不是 inotify：这里对精度要求是毫秒级，
	 * 轮询简单、不会因为事件队列出问题。
	 */
	if (waitfile) {
		while (access(waitfile, F_OK) != 0)
			usleep(2000);
	}

	long long *sched = calloc((size_t)requests, sizeof(long long));
	long long *work = calloc((size_t)requests, sizeof(long long));
	int n = 0, early = 0;

	if (!sched || !work)
		return 1;

	/*
	 * 预热。前几次调用会把权重和代码推进缓存，
	 * 这段开销不属于调度延迟，混进去会让 p50 偏大。
	 * 预热期间也照常睡，保证进入正式计时时进程状态一致。
	 */
	for (int i = 0; i < warmup; i++) {
		for (int k = 0; k < passes; k++)
			forward();
		usleep((useconds_t)interval_ms * 1000);
	}

	long long t0 = now_ns();
	long long interval_ns = (long long)interval_ms * 1000000LL;

	for (int i = 0; i < requests; i++) {
		/*
		 * 绝对时间睡眠。
		 *
		 * 用 TIMER_ABSTIME 而不是"睡 interval 纳秒"，是因为后者会累积误差：
		 * 每次算完再睡 10ms，实际周期 = 10ms + 计算时间 + 调度误差，
		 * 几百次之后偏出去很远，最后一个请求的计划时刻完全对不上。
		 * 绝对时间每次重新对齐到 t0 + i*interval，误差不累积。
		 */
		struct timespec next;
		long long target = t0 + (long long)(i + 1) * interval_ns;

		next.tv_sec = target / 1000000000LL;
		next.tv_nsec = target % 1000000000LL;

		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;

		long long t_wake = now_ns();

		for (int k = 0; k < passes; k++)
			forward();

		long long t_done = now_ns();
		long long s = t_wake - target;
		long long w = t_done - t_wake;

		if (s < 0) {
			/* 提前醒来（极少见），钳到 0，但记一笔 */
			early++;
			s = 0;
		}

		sched[n] = s / 1000;
		work[n] = w / 1000;
		n++;
	}

	qsort(sched, (size_t)n, sizeof(long long), cmp_ll);
	qsort(work, (size_t)n, sizeof(long long), cmp_ll);

	long long sp50 = pct(sched, n, 50), sp90 = pct(sched, n, 90);
	long long sp99 = pct(sched, n, 99), smax = n ? sched[n - 1] : 0;
	long long wp50 = pct(work, n, 50), wp99 = pct(work, n, 99);

	if (json) {
		printf("{\"requests\":%d,\"sched_p50_us\":%lld,\"sched_p90_us\":%lld,"
		       "\"sched_p99_us\":%lld,\"sched_max_us\":%lld,"
		       "\"work_p50_us\":%lld,\"work_p99_us\":%lld}\n",
		       n, sp50, sp90, sp99, smax, wp50, wp99);
	} else {
		printf("请求数 %d，间隔 %d ms，每请求 %d 次前向，预热 %d\n",
		       n, interval_ms, passes, warmup);
		printf("调度延迟（醒来比计划晚了多久，微秒）\n");
		printf("  p50=%-6lld p90=%-6lld p99=%-6lld max=%-6lld\n",
		       sp50, sp90, sp99, smax);
		printf("计算耗时（微秒）\n");
		printf("  p50=%-6lld p99=%-6lld\n", wp50, wp99);
		if (early)
			printf("提前醒来的次数: %d\n", early);
		printf("RESULT sched_p50_us=%lld sched_p90_us=%lld sched_p99_us=%lld "
		       "sched_max_us=%lld work_p50_us=%lld\n",
		       sp50, sp90, sp99, smax, wp50);
	}

	free(sched);
	free(work);
	return 0;
}
