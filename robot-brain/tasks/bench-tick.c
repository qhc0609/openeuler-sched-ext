/*
 * bench-tick.c —— 拆开以后每拍多花多少
 *
 * 拆解不是免费的：每拍要 tick 整棵树，还要在任务之间传返回值。
 * 这个工具量两件事：
 *
 *   1. 顺序里挂 1/2/4/8 个任务，走完一个子任务平均花多少
 *   2. 文章里那棵完整的树（抢占 + 顺序 4 + 避障）tick 一次多少钱
 *
 * 第 1 项要"走完"，所以空任务返回 SUCCESS——返回 RUNNING 的话，
 * 顺序永远停在第 0 个，量出来的是常数，跟挂几个任务没关系。
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "task.h"
#include "world.h"

#define PERIOD_NS 20000000LL	/* 50 Hz，一拍 20 ms */

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int dummy_tick(struct task *t, struct world *w)
{
	(void)w;
	g_active = t->name;
	return TASK_SUCCESS;
}

static struct task *dummy(void)
{
	struct task *t = calloc(1, sizeof(*t));

	t->name = "空任务";
	t->tick = dummy_tick;
	t->st = calloc(1, sizeof(int));
	return t;
}

/* 走完一条 n 个任务的顺序，返回"走完一遍多少 ns" */
static double bench_walk(struct task *root, struct world *w, int n, long iters)
{
	long long t0, t1;

	for (long i = 0; i < 1000; i++)
		task_tick(root, w);

	t0 = now_ns();
	for (long i = 0; i < iters; i++) {
		task_reset(root);
		for (int k = 0; k < n; k++)
			task_tick(root, w);
	}
	t1 = now_ns();

	return (double)(t1 - t0) / (double)iters;
}

int main(void)
{
	struct world w;
	struct task *s1, *s2, *s4, *s8;
	struct task *kids[4], *root, *leaf;
	double a1, a2, a4, a8, tree, leafns;
	long iters = 200000;

	world_init(&w);

	s1 = seq_n((struct task *[]){ dummy() }, 1);
	s2 = seq_n((struct task *[]){ dummy(), dummy() }, 2);
	s4 = seq_n((struct task *[]){ dummy(), dummy(), dummy(), dummy() }, 4);
	s8 = seq_n((struct task *[]){ dummy(), dummy(), dummy(), dummy(),
					dummy(), dummy(), dummy(), dummy() }, 8);

	printf("== 走完一遍顺序要多久（含一次 task_reset）==\n\n");
	a1 = bench_walk(s1, &w, 1, iters);
	a2 = bench_walk(s2, &w, 2, iters);
	a4 = bench_walk(s4, &w, 4, iters);
	a8 = bench_walk(s8, &w, 8, iters);
	printf("  挂 1 个                  %5.1f ns\n", a1);
	printf("  挂 2 个                  %5.1f ns\n", a2);
	printf("  挂 4 个                  %5.1f ns\n", a4);
	printf("  挂 8 个                  %5.1f ns\n", a8);
	printf("  ── 每多挂一个任务        %5.1f ns\n\n", (a8 - a1) / 7.0);

	/* 单个真实叶子：前进（含一次 atan2f、一次 sqrtf） */
	leaf = task_goto(0.45f, 0.00f);
	{
		long long t0, t1;
		long n = 5000000;

		for (long i = 0; i < 10000; i++)
			task_tick(leaf, &w);
		t0 = now_ns();
		for (long i = 0; i < n; i++)
			task_tick(leaf, &w);
		t1 = now_ns();
		leafns = (double)(t1 - t0) / (double)n;
	}
	printf("== 整棵树 ==\n\n");
	printf("  一个真实叶子（前进）     %5.1f ns\n", leafns);

	kids[0] = task_goto(0.45f, 0.00f);
	kids[1] = task_goto(1.00f, 0.00f);
	kids[2] = task_goto(1.00f, 0.55f);
	kids[3] = task_stop();
	root = arb(task_avoid(), seq_n(kids, 4));
	{
		long long t0, t1;
		long n = 2000000;

		for (long i = 0; i < 10000; i++)
			task_tick(root, &w);
		t0 = now_ns();
		for (long i = 0; i < n; i++)
			task_tick(root, &w);
		t1 = now_ns();
		tree = (double)(t1 - t0) / (double)n;
	}

	printf("  整棵树（抢占+顺序4+避障） %5.1f ns\n", tree);
	printf("  占 20 ms 周期            %.4f%%\n",
	       100.0 * tree / (double)PERIOD_NS);
	return 0;
}
