/* SPDX-License-Identifier: GPL-2.0 */
/*
 * loadgen.c — 造 CPU 竞争
 *
 * 它只做一件事：开 N 个线程，每个线程死循环做整数运算，不睡、不阻塞、
 * 不碰 IO。这样每个线程都是"永远可运行"的状态，把 CPU 占满。
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
 * 用法：./loadgen <线程数> <秒数>
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int stop_flag;
static volatile unsigned long long sink;   /* 防止编译器把循环优化掉 */

static void *spinner(void *arg)
{
	unsigned long long x = (unsigned long long)(size_t)arg * 2654435761ULL + 1;

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
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int nthreads, seconds;

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

	pthread_t *th = calloc((size_t)nthreads, sizeof(pthread_t));

	if (!th)
		return 1;

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

	fprintf(stderr, "loadgen: 结束（sink=%llu）\n", sink);
	free(th);
	return 0;
}
