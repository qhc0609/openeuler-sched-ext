/*
 * naive-loop.c —— 一个循环干完所有事
 *
 * 这是很多人写的第一版机器人程序：读传感器、判断、输出。它没有错。
 * 问题出在"判断"长大成"规划"之后——这一拍有多长，就不再由你决定了。
 *
 * 循环体三件事，串行执行：
 *   1. 读状态（编码器 / 里程计）
 *   2. 伺服更新，和 cerebellum 里同一个一阶滞后模型
 *   3. 每 every 拍做一次"规划"，耗时 think_us
 *
 * 编译：make bin/naive-loop
 * 跑法：./bin/naive-loop --hz 1000 --think-us 2000 --every 50
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSAMP	20000

/* 防止编译器把"没有副作用"的计算全删掉 */
static volatile float g_sink;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(long long t)
{
	struct timespec ts = { t / 1000000000LL, t % 1000000000LL };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* 忙等，代表一次真正要花 CPU 的计算：规划、推理、点云配准都算这一类 */
static void busy_us(long us)
{
	long long until = now_ns() + us * 1000LL;

	while (now_ns() < until)
		;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

static long long pct(const long long *v, int n, int p)
{
	int i = n * p / 100;

	if (i >= n)
		i = n - 1;
	return v[i];
}

/*
 * 一拍的控制：一阶滞后伺服 + 一点三角函数。
 * 三角函数不是装饰——真实控制器里逆解、雅可比、滤波都要算，
 * 这里只是让它有和 cerebellum 同量级的开销。
 */
static float control_step(float q, float target, float dt, float tau)
{
	float dq = (target - q) / tau;

	q += dq * dt;
	q += sinf(q) * 1e-5f;
	g_sink = q;
	return q;
}

int main(int argc, char **argv)
{
	static long long per[NSAMP], body[NSAMP];
	static long long per_plan[NSAMP], body_plan[NSAMP];
	int hz = 1000, every = 50, n = 0, np = 0;
	long think_us = 2000, period, next, prev, t0, t1, missed = 0;
	float q = 0.0f;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hz") && i + 1 < argc)
			hz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--think-us") && i + 1 < argc)
			think_us = atol(argv[++i]);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc)
			every = atoi(argv[++i]);
		else {
			fprintf(stderr, "用法: naive-loop [--hz N] [--think-us N] "
				"[--every N]\n");
			return 2;
		}
	}
	if (hz <= 0 || every <= 0)
		return 2;
	period = 1000000000LL / hz;

	printf("== 一个循环：%d Hz，每 %d 拍规划一次，每次 %ld us ==\n",
	       hz, every, think_us);

	prev = now_ns();
	next = prev;

	while (n < NSAMP) {
		int planning = (n % every == every - 1);

		next += period;
		sleep_until(next);
		t0 = now_ns();

		/* 1. 读状态 */
		float q_now = q;

		/* 2. 伺服更新——控制周期就是这一拍的长度 */
		q = control_step(q_now, 1.0f, (float)period / 1e9f, 0.02f);

		/* 3. 规划，串在同一个循环里 */
		if (planning)
			busy_us(think_us);

		t1 = now_ns();

		per[n] = t1 - prev;
		body[n] = t1 - t0;
		if (planning) {
			per_plan[np] = t1 - prev;
			body_plan[np] = t1 - t0;
			np++;
		}
		if (per[n] > period * 3 / 2)
			missed++;
		prev = t1;
		n++;
	}

	qsort(per, (size_t)n, sizeof(per[0]), cmp_ll);
	qsort(body, (size_t)n, sizeof(body[0]), cmp_ll);
	qsort(per_plan, (size_t)np, sizeof(per_plan[0]), cmp_ll);
	qsort(body_plan, (size_t)np, sizeof(body_plan[0]), cmp_ll);

	printf("  循环体耗时：普通拍 p50 %.2f us，规划拍 p50 %.2f us\n",
	       body[n / 2] / 1000.0, body_plan[np / 2] / 1000.0);
	printf("  周期：p50 %.1f us，p90 %.1f us，p99 %.1f us，最大 %.1f us\n",
	       per[n / 2] / 1000.0, pct(per, n, 90) / 1000.0,
	       pct(per, n, 99) / 1000.0, per[n - 1] / 1000.0);
	printf("  规划那 %d 拍的周期：p50 %.1f us\n",
	       np, per_plan[np / 2] / 1000.0);
	printf("  超过 1.5 倍名义周期：%ld / %d 拍（%.1f%%）\n",
	       missed, n, 100.0 * (double)missed / (double)n);

	return 0;
}
