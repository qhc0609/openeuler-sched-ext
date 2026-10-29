/*
 * test-tasks.c —— 拆开以后才能做的事：把每个任务单独拎出来跑一千遍
 *
 * 揉在一个循环里的版本没法这么测，因为你没法只要"转弯"、不要"前进"。
 * 拆开之后每个任务都是一个函数，喂一个假世界进去就能跑。
 *
 * 三组随机试验，每组 1000 次，随机数用固定种子，结果可复现。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "task.h"
#include "world.h"

#define N	1000
#define PI	3.14159265358979f

static unsigned rng = 20261029u;

static unsigned xr(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static float frand(float lo, float hi)
{
	return lo + (hi - lo) * ((float)(xr() & 0xFFFFFF) / 16777215.0f);
}

static float wrap_pi(float a)
{
	while (a > PI)
		a -= 2.0f * PI;
	while (a < -PI)
		a += 2.0f * PI;
	return a;
}

static int cmp_f(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return (x > y) - (x < y);
}

static void stats(const char *label, float *v, int n, const char *unit)
{
	qsort(v, n, sizeof(float), cmp_f);
	printf("  %-16s p50 %7.2f   p99 %7.2f   最大 %7.2f  %s\n",
	       label, (double)v[n / 2], (double)v[n * 99 / 100],
	       (double)v[n - 1], unit);
}

/* ---------------------------- 转弯 ---------------------------- */

static void test_turn(void)
{
	static float ferr[N], ftick[N];
	int flips = 0, fail = 0;

	for (int i = 0; i < N; i++) {
		struct world w;
		struct task *t;
		float tgt = frand(-PI, PI);
		float prev_err = 0.0f;
		int n = 0, r = TASK_RUNNING;

		world_init(&w);
		w.th = frand(-PI, PI);
		t = task_turn(tgt);

		while (n < 800) {
			float e;

			r = task_tick(t, &w);
			world_step(&w);
			n++;
			if (r != TASK_RUNNING)
				break;

			e = wrap_pi(tgt - w.th);
			if (prev_err != 0.0f && e != 0.0f
			    && (e > 0.0f) != (prev_err > 0.0f))
				flips++;
			prev_err = e;
		}
		if (r != TASK_SUCCESS)
			fail++;
		ferr[i] = fabsf(wrap_pi(tgt - w.th)) * 57.2957795f;
		ftick[i] = (float)n;
	}

	printf("\n== 转弯：1000 组随机起始朝向、随机目标角 ==\n");
	stats("最终角度误差", ferr, N, "度");
	stats("用时", ftick, N, "拍");
	printf("  800 拍内没转完：%d 组；中途转过头：%d 次\n", fail, flips);
}

/* ---------------------------- 前进 ---------------------------- */

static void test_goto(void)
{
	static float ferr[N], ftick[N];
	int fail = 0, hit = 0;

	for (int i = 0; i < N; i++) {
		struct world w;
		struct task *t;
		float gx = frand(-1.0f, 1.0f), gy = frand(-1.0f, 1.0f);
		int n = 0, r = TASK_RUNNING;

		world_init(&w);
		w.x = frand(-0.6f, 0.6f);
		w.y = frand(-0.6f, 0.6f);
		w.th = frand(-PI, PI);
		t = task_goto(gx, gy);

		while (n < 1500) {
			r = task_tick(t, &w);
			world_step(&w);
			n++;
			if (r != TASK_RUNNING)
				break;
		}
		if (r != TASK_SUCCESS)
			fail++;
		if (w.collided)
			hit++;
		ferr[i] = 1000.0f * hypotf(gx - w.x, gy - w.y);
		ftick[i] = (float)n;
	}

	printf("\n== 前进：1000 组随机起点、随机目标点（无障碍）==\n");
	stats("终点误差", ferr, N, "mm");
	stats("用时", ftick, N, "拍");
	printf("  1500 拍内没到：%d 组\n", fail);
	(void)hit;
}

/* ---------------------------- 避障 ---------------------------- */

static void test_avoid(void)
{
	static float ftick[N];
	int fail = 0, hit = 0;

	for (int i = 0; i < N; i++) {
		struct world w;
		struct task *t;
		float r = frand(0.05f, 0.12f);
		float d = r + frand(0.05f, 0.17f);	/* 表面距离 5~17 cm */
		float lat = frand(-0.10f, 0.10f);
		int n = 0, rc = TASK_RUNNING;

		world_init(&w);
		world_add_obs(&w, d, lat, r);
		t = task_avoid();

		while (n < 800) {
			rc = task_tick(t, &w);
			world_step(&w);
			n++;
			if (rc != TASK_RUNNING)
				break;
		}
		if (rc != TASK_SUCCESS)
			fail++;
		if (w.collided)
			hit++;
		ftick[i] = (float)n;
	}

	printf("\n== 避障：1000 组随机障碍（表面距离 5~17 cm，半径 5~12 cm）==\n");
	stats("绕完用时", ftick, N, "拍");
	printf("  800 拍内没绕完：%d 组；撞上：%d 组\n", fail, hit);
}

int main(void)
{
	test_turn();
	test_goto();
	test_avoid();
	return 0;
}
