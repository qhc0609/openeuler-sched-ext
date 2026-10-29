/*
 * bench.c —— 查表值多少钱
 *
 * 三个数：扫完整张表不匹配、命中一条边、以及那串 if。
 * 结论多半是"都不值钱"——这一篇想说的也不是性能。
 *
 * 输入都从 volatile 变量取，不然 -O2 会把整段算成常数，
 * 量出来是个假的纳秒数（第一版就踩了这个坑）。
 */
#include "fsm.h"

#include <stdio.h>
#include <time.h>

#define ITER 10000000

static volatile int g_sink;
static volatile float g_d = 0.80f;
static volatile int g_run = 1;
static volatile int g_estop = 0;
static volatile int g_ev = EV_CMD_START;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int naive_state(float d, int running, int estop)
{
	if (estop)
		return ST_FAULT;
	if (d < 0.45f)
		return ST_AVOID;
	if (running)
		return ST_RUN;
	return ST_IDLE;
}

int main(void)
{
	struct rb_fsm f;
	uint64_t t0, t1;
	int i;

	/* 1. 不匹配：停在 BOOT，发 START，整张表扫完都不匹配 */
	fsm_init(&f, 0);
	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		fsm_step(&f, g_ev, 0);
		g_sink = f.state;
	}
	t1 = now_ns();
	printf("%6.1f ns   fsm_step()，15 条边全不匹配\n",
	       (double)(t1 - t0) / (double)ITER);

	/* 2. 命中：停在 RUN，发 NEAR，第 5 条边匹配上 */
	fsm_init(&f, 0);
	fsm_step(&f, EV_RESET, 0);
	fsm_step(&f, EV_CMD_START, 0);
	f.state = ST_RUN;
	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		fsm_step(&f, EV_OBSTACLE_NEAR, (uint64_t)i * 1000000000ull);
		f.state = ST_RUN;
		g_sink = f.state;
	}
	t1 = now_ns();
	printf("%6.1f ns   fsm_step()，第 5 条边命中（含进入动作）\n",
	       (double)(t1 - t0) / (double)ITER);

	/* 3. if 链 */
	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		g_sink = naive_state(g_d, g_run, g_estop);
	}
	t1 = now_ns();
	printf("%6.1f ns   一串 if，从输入直接算状态\n",
	       (double)(t1 - t0) / (double)ITER);

	return 0;
}
