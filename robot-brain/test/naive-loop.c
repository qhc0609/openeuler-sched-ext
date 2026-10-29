/*
 * naive-loop.c —— 五件事塞进一个循环
 *
 * 场景和大脑/小脑那套完全一样：二连杆机械臂，末端沿半径 11 cm 的圆走，
 * 圆边上放了个半径 4.5 cm 的障碍物，靠近了要往外绕。
 *
 * 差别只有一处：这里不分进程。每一拍按顺序做完四件事——
 *
 *   1. 规划：算出未来 200 ms 的 21 个路点（势场 + 逆解）
 *   2. 取第一个点当这一拍的关节指令
 *   3. 伺服：一阶滞后，时间常数 20 ms，和 cerebellum 里同一个模型
 *   4. 记数
 *
 * think_us 是"规划变贵"的替身。现在的势场法只花 2 微秒，
 * 换成视觉检测、点云配准或者模型推理，就是 2 毫秒、20 毫秒。
 * 这一版没有任何缓冲：想要新指令，就得现在算。
 *
 * 编译：make bin/naive-loop
 * 跑法：./bin/naive-loop --hz 1000 --think-us 2000
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define L1		0.30f
#define L2		0.25f
#define TRAJ_POINTS	21		/* 200 ms，10 ms 一个点 */
#define TRAJ_DT		0.010f
#define NSAMP		20000

/* 和 brain.c 里同一组参数 */
#define CIRCLE_CX	0.32f
#define CIRCLE_CY	0.06f
#define CIRCLE_R	0.11f
#define OBST_X		0.36f
#define OBST_Y		0.12f
#define OBST_R		0.045f
#define OMEGA		1.1f

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

static void fk(float q1, float q2, float *x, float *y)
{
	*x = L1 * cosf(q1) + L2 * cosf(q1 + q2);
	*y = L1 * sinf(q1) + L2 * sinf(q1 + q2);
}

/* 二连杆逆解，和 brain.c 里同一个 */
static int ik(float x, float y, float *q1, float *q2)
{
	float c2 = (x * x + y * y - L1 * L1 - L2 * L2) / (2.0f * L1 * L2);
	float s2, t2;

	if (c2 > 1.0f || c2 < -1.0f)
		return -1;
	t2 = acosf(c2);
	s2 = sinf(t2);
	*q2 = t2;
	*q1 = atan2f(y, x) - atan2f(L2 * s2, L1 + L2 * c2);
	return 0;
}

/* 圆 + 势场避障，和 brain.c 里同一个 plan_path */
static void plan_path(float t, float *px, float *py)
{
	float ang = OMEGA * t;
	float x = CIRCLE_CX + CIRCLE_R * cosf(ang);
	float y = CIRCLE_CY + CIRCLE_R * sinf(ang);
	float dx = x - OBST_X, dy = y - OBST_Y;
	float d = sqrtf(dx * dx + dy * dy);
	float margin = OBST_R + 0.06f;

	if (d < margin && d > 1e-4f) {
		float push = (margin - d) / margin;

		push = push > 1.0f ? 1.0f : push;
		x += dx / d * push * 0.08f;
		y += dy / d * push * 0.08f;
	}
	*px = x;
	*py = y;
}

int main(int argc, char **argv)
{
	static long long per[NSAMP], body[NSAMP];
	int hz = 1000, n = 0, want_n = 0;
	long think_us = 2000, period, next, prev, t0, t1, missed = 0;
	float tau = 0.020f;
	float q1 = 0.0f, q2 = 0.0f;		/* 实际角度 */
	float q1_cmd = 0.0f, q2_cmd = 0.0f;	/* 指令角度 */
	float err_max = 0.0f, near_min = 1e9f;
	long long t_start;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hz") && i + 1 < argc)
			hz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--think-us") && i + 1 < argc)
			think_us = atol(argv[++i]);
		else if (!strcmp(argv[i], "--n") && i + 1 < argc)
			want_n = atoi(argv[++i]);
		else {
			fprintf(stderr, "用法: naive-loop [--hz N] [--think-us N] "
				"[--n N]\n");
			return 2;
		}
	}
	if (hz <= 0)
		return 2;
	period = 1000000000LL / hz;
	n = want_n > 0 ? want_n : NSAMP;

	printf("== 一个循环：画圆 + 绕障，规划每拍做一次，耗时 %ld us ==\n",
	       think_us);

	t_start = now_ns();
	prev = t_start;
	next = t_start;

	for (i = 0; i < n; i++) {
		float t_now, nq1 = q1, nq2 = q2;
		float dt, ex, ey, d;

		next += period;
		sleep_until(next);
		t0 = now_ns();

		t_now = (float)(t0 - t_start) / 1e9f;

		/* 1. 规划：未来 200 ms 的 21 个路点，全算一遍 */
		for (int k = 0; k < TRAJ_POINTS; k++) {
			float xk, yk, a, b;

			plan_path(t_now + (float)k * TRAJ_DT, &xk, &yk);
			if (ik(xk, yk, &a, &b) < 0) {
				a = q1;
				b = q2;
			}
			if (k == 0) {
				nq1 = a;
				nq2 = b;
				g_sink = a + b;
			}
		}
		busy_us(think_us);

		/* 2. 取第一个点当这一拍的指令 */
		q1_cmd = nq1;
		q2_cmd = nq2;

		/* 3. 伺服：一阶滞后，时间常数 20 ms */
		dt = (float)period / 1e9f;
		q1 += (q1_cmd - q1) * dt / tau;
		q2 += (q2_cmd - q2) * dt / tau;

		/*
		 * 4. 记数。前 1 秒不算——机械臂从零位起步，第一拍指令就跳到
		 * 圆周上，那一下的误差是启动瞬态，不是稳态跟随误差。
		 */
		if (t_now > 1.0f) {
			float e1 = fabsf(q1_cmd - q1);
			float e2 = fabsf(q2_cmd - q2);
			float e = e1 > e2 ? e1 : e2;

			if (e > err_max)
				err_max = e;

			fk(q1, q2, &ex, &ey);
			d = sqrtf((ex - OBST_X) * (ex - OBST_X) +
				  (ey - OBST_Y) * (ey - OBST_Y));
			if (d < near_min)
				near_min = d;
		}

		t1 = now_ns();
		per[i] = t1 - prev;
		body[i] = t1 - t0;
		if (per[i] > period * 3 / 2)
			missed++;
		prev = t1;
	}

	qsort(per, (size_t)n, sizeof(per[0]), cmp_ll);
	qsort(body, (size_t)n, sizeof(body[0]), cmp_ll);

	printf("  循环体耗时 p50 %.1f us，p99 %.1f us\n",
	       body[n / 2] / 1000.0, pct(body, n, 99) / 1000.0);
	printf("  周期 min %.1f / p50 %.1f / p90 %.1f / p99 %.1f / max %.1f us\n",
	       per[0] / 1000.0, per[n / 2] / 1000.0, pct(per, n, 90) / 1000.0,
	       pct(per, n, 99) / 1000.0, per[n - 1] / 1000.0);
	printf("  实际周期 p50 %.1f us → 电机指令更新 %.0f Hz（目标 %d Hz）\n",
	       per[n / 2] / 1000.0, 1e9 / (double)per[n / 2], hz);
	printf("  稳态跟随误差最大 %.3f rad（跳过起步 1 秒）\n", err_max);
	printf("  末端离障碍物最近 %.1f mm（障碍物半径 %.0f mm）\n",
	       near_min * 1000.0, OBST_R * 1000.0);
	printf("  超过 1.5 倍名义周期 %ld / %d 拍（%.1f%%）\n",
	       missed, n, 100.0 * (double)missed / (double)n);

	return 0;
}
