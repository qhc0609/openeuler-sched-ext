/*
 * common.h —— 三个程序共用的一点小工具
 *
 * 只有两件值得说的事：
 *
 * 1. wait_until() 不是简单的 clock_nanosleep。这台虚拟机上纯睡 1 ms，
 *    宿主机可能已经把 vCPU 挂起，一觉睡出几毫秒（第七篇量过）。所以先睡到
 *    差 150 微秒，再自旋到点。真机器人的伺服回路也是这么干的——睡到临近
 *    再自旋，精度才稳。
 *
 * 2. burn_ns() 用时钟自己卡时长，不数循环次数。换台机器不用重标定。
 */
#ifndef COMMON_H
#define COMMON_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PERIOD_NS	1000000ull	/* 1 kHz */
#define SPIN_NS		150000ull	/* 最后这一段自旋 */

static inline uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void sleep_until(uint64_t t)
{
	struct timespec ts = { (time_t)(t / 1000000000ull),
			       (long)(t % 1000000000ull) };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static inline void wait_until(uint64_t t)
{
	if (t > now_ns() + SPIN_NS)
		sleep_until(t - SPIN_NS);
	while (now_ns() < t)
		;
}

static inline void burn_ns(uint64_t ns)
{
	uint64_t t0 = now_ns();

	while (now_ns() - t0 < ns)
		;
}

static inline int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* 第 p 百分位。调用前数组要排好序。 */
static inline uint64_t pct(const uint64_t *v, int n, int p)
{
	int i = n * p / 100;

	if (n <= 0)
		return 0;
	if (i >= n)
		i = n - 1;
	return v[i];
}

#endif
