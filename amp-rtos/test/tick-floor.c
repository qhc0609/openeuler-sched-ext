/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tick-floor.c —— 这台机器上"1 kHz 定时"到底能做到多准
 *
 * 为什么要有这个程序：
 *
 *   测出来 FreeRTOS 控制环的抖动以后，第一个问题是"这是 AMP 的锅，还是这台机器的锅"。
 *   这个程序把问题拆开：一个裸 pthread，SCHED_FIFO，绝对时刻 clock_nanosleep，
 *   1 kHz，量它每次醒来的偏差。
 *
 *   这就是下限。FreeRTOS 端口比它多出来的部分，是"用信号切换任务"的成本；
 *   比它差不了的部分，是虚拟机本身给不了的东西。
 *
 * 用法：./bin/tick_floor [秒数] [CPU]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#define BUCKETS	6
static const long long edge_ns[BUCKETS] = {
	50000, 100000, 200000, 500000, 1000000, 0
};

static int g_cpu = 3;
static int g_secs = 5;
static long g_spin_us = 0;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	struct sched_param sp;
	cpu_set_t set;
	struct timespec next;
	long long *dev;
	long long t0, grid0;
	int cap, n = 0, i, b;
	unsigned long hist[BUCKETS];
	pthread_t self = pthread_self();

	if (argc > 1)
		g_secs = atoi(argv[1]);
	if (argc > 2)
		g_cpu = atoi(argv[2]);
	if (argc > 3)
		g_spin_us = atol(argv[3]);
	if (g_secs <= 0)
		g_secs = 5;

	cap = g_secs * 1000 + 1000;
	dev = calloc((size_t)cap, sizeof(*dev));
	if (!dev) {
		perror("calloc");
		return 1;
	}

	CPU_ZERO(&set);
	CPU_SET(g_cpu, &set);
	if (pthread_setaffinity_np(self, sizeof(set), &set) != 0)
		perror("setaffinity");

	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = 50;
	if (pthread_setschedparam(self, SCHED_FIFO, &sp) != 0)
		perror("setschedparam（不是 root 就只能跑 SCHED_OTHER）");

	printf("== 裸 1 kHz 定时器：CPU%d，SCHED_FIFO %d，%d 秒 ==\n",
	       g_cpu, sp.sched_priority, g_secs);
	if (g_spin_us > 0)
		printf("  睡到还差 %ld us，剩下自旋（绝对时刻 clock_nanosleep）\n\n",
		       g_spin_us);
	else
		printf("  只用绝对时刻 clock_nanosleep，不自旋\n\n");

	clock_gettime(CLOCK_MONOTONIC, &next);
	grid0 = now_ns();
	t0 = grid0;

	for (i = 0; i < g_secs * 1000; i++) {
		struct timespec wake;
		long long want, got, d;

		next.tv_nsec += 1000000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec += 1;
		}
		want = grid0 + (long long)(i + 1) * 1000000LL;
		wake = next;

		if (g_spin_us > 0 && g_spin_us < 1000) {
			long back = g_spin_us * 1000L;

			if (wake.tv_nsec >= back) {
				wake.tv_nsec -= back;
			} else {
				wake.tv_sec -= 1;
				wake.tv_nsec += 1000000000L - back;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
			while (now_ns() < want)
				;	/* 自旋跨过最后一段 */
		} else {
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next,
					NULL);
		}

		got = now_ns();
		d = got - want;
		if (d < 0)
			d = -d;
		if (n < cap)
			dev[n++] = d;
	}

	memset(hist, 0, sizeof(hist));
	for (i = 0; i < n; i++) {
		for (b = 0; b < BUCKETS - 1; b++) {
			if (dev[i] < edge_ns[b])
				break;
		}
		hist[b]++;
	}

	qsort(dev, (size_t)n, sizeof(*dev), cmp_ll);

	printf("  拍了 %d 次，实际用时 %.3f 秒\n", n,
	       (double)(now_ns() - t0) / 1e9);
	printf("  偏差：p50 %.1f us，p90 %.1f us，p99 %.1f us，最大 %.1f us\n",
	       dev[n / 2] / 1000.0,
	       dev[(int)((long long)n * 90 / 100)] / 1000.0,
	       dev[(int)((long long)n * 99 / 100)] / 1000.0,
	       dev[n - 1] / 1000.0);

	printf("\n  分布：\n");
	for (b = 0; b < BUCKETS; b++) {
		if (b < BUCKETS - 1)
			printf("    < %6.0f us  %6lu  %5.1f%%\n",
			       edge_ns[b] / 1000.0, hist[b],
			       100.0 * (double)hist[b] / (double)n);
		else
			printf("    >= %5.0f us  %6lu  %5.1f%%\n",
			       edge_ns[b - 1] / 1000.0, hist[b],
			       100.0 * (double)hist[b] / (double)n);
	}

	free(dev);
	return 0;
}
