/* SPDX-License-Identifier: GPL-2.0 */
/*
 * loadgen.c — 造 CPU 竞争，并报告自己完成了多少活
 *
 * 它做两件事：
 *   1. 开 N 个线程，每个线程死循环做整数运算，不睡、不阻塞、不碰 IO。
 *      这样每个线程都是"永远可运行"的状态，把 CPU 占满。
 *   2. 数每个线程跑完了多少批，退出时打印总量和速率。
 *
 * 为什么用死循环而不是跑真实业务：
 *   真实负载会有 IO 等待、锁竞争，行为不干净。死循环是 CPU 竞争的最坏情况，
 *   也最容易复现：同样的线程数，每次跑出来的排队情况几乎一样。
 *
 * 为什么线程数要超过核数：
 *   如果线程数 <= 核数，每个线程都有自己的核，没人排队，
 *   推理引擎一醒来就能上 CPU —— 那就测不出调度器的价值了。
 *   要制造排队，线程数必须大于核数。run-demo.sh 默认给 2 倍。
 *
 * 为什么要数迭代次数：
 *   只测"AI 引擎变快了多少"是半个实验。优先级是有方向的：引擎拿到的每一微秒，
 *   都是从别的任务手里拿走的。吞吐（iters/秒）就是被拿走的那一侧的度量。
 *   没有这个数，"代价"只能靠形容词。
 *
 * 用法：./loadgen <线程数> <秒数>
 *   正常跑完或被 SIGTERM/SIGINT 打断，都会在 stderr 打印一行：
 *   loadgen: elapsed=8.02s iters=123456789 rate=15395000/s
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile int stop_flag;
static volatile unsigned long long sink;   /* 防止编译器把循环优化掉 */

/* 每个线程各写自己那一格，退出时再汇总，避免在热循环里抢同一个变量 */
static unsigned long long *iters;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
	(void)sig;
	stop_flag = 1;   /* 让线程自己退，主线程照常汇总 */
}

static void *spinner(void *arg)
{
	unsigned long long x = (unsigned long long)(size_t)arg * 2654435761ULL + 1;
	unsigned long long done = 0;

	while (!stop_flag) {
		/*
		 * 一串依赖前一次的乘加：不能并行、不能提前算完，
		 * 每个线程都实打实地占住一个执行单元。
		 *
		 * 如果写成 x = x * 3; 这种可预测的形式，编译器可能直接消掉。
		 * volatile 的 sink 在循环外写一次，保证整段有副作用。
		 */
		for (int i = 0; i < 10000; i++)
			x = x * 6364136223846793005ULL + 1442695040888963407ULL;
		sink = x;
		done++;
	}

	iters[(size_t)arg - 1] = done;
	return NULL;
}

int main(int argc, char **argv)
{
	int nthreads, seconds;
	pthread_t *th;
	struct sigaction sa;
	double t0, elapsed;
	unsigned long long total = 0;

	if (argc < 3) {
		fprintf(stderr, "用法: %s <线程数> <秒数>\n", argv[0]);
		return 2;
	}

	nthreads = atoi(argv[1]);
	seconds = atoi(argv[2]);
	if (nthreads <= 0 || seconds <= 0) {
		fprintf(stderr, "参数必须是正整数\n");
		return 2;
	}

	th = calloc((size_t)nthreads, sizeof(pthread_t));
	iters = calloc((size_t)nthreads, sizeof(unsigned long long));
	if (!th || !iters)
		return 1;

	/*
	 * 被 kill 时要先把数交出来再死。
	 * run-demo.sh 在引擎跑完后会 kill 掉压测进程，默认的 SIGTERM 行为是
	 * 直接终止，一个字节都不会打——那就白数了。
	 */
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	t0 = now_s();

	for (int i = 0; i < nthreads; i++) {
		if (pthread_create(&th[i], NULL, spinner, (void *)(size_t)(i + 1)) != 0) {
			fprintf(stderr, "创建线程 %d 失败\n", i);
			return 1;
		}
	}

	fprintf(stderr, "loadgen: %d 个线程，跑 %d 秒\n", nthreads, seconds);
	fflush(stderr);

	sleep((unsigned int)seconds);
	stop_flag = 1;

	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);

	elapsed = now_s() - t0;
	for (int i = 0; i < nthreads; i++)
		total += iters[i];

	/*
	 * 一行机器可读的汇总。rate 是主指标：三组的实际运行时长不完全相同
	 * （引擎跑完才 kill），直接比 iters 会有偏差，比速率才公平。
	 */
	fprintf(stderr, "loadgen: elapsed=%.2fs iters=%llu rate=%.0f/s\n",
		elapsed, total, elapsed > 0 ? (double)total / elapsed : 0.0);

	free(th);
	free(iters);
	return 0;
}
