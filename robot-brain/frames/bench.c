/*
 * bench.c —— 坐标系变换和指令检查，各要多少时间
 *
 * 这一类开销经常被当成"要优化"的对象。量一下就知道：整个链路
 * 走一遍不到一微秒，而控制周期是 20 毫秒。
 */
#include <stdio.h>
#include <time.h>

#include "cmd.h"
#include "pose.h"

#define ITER	500000

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile float sink;
static volatile int isink;

int main(void)
{
	struct pose p = { 0.30f, 0.10f, 1.20f };
	struct vec2 v = { 0.80f, 0.60f };
	struct motion_cmd c;
	uint64_t t0, t1;
	double e_wrap, e_body, e_bear, e_nav, e_accept, e_all;

	t0 = now_ns();
	for (int i = 0; i < ITER; i++)
		sink = wrap_pi((float)i * 1e-4f);
	t1 = now_ns();
	e_wrap = (double)(t1 - t0) / ITER;

	t0 = now_ns();
	for (int i = 0; i < ITER; i++) {
		struct vec2 b = pose_to_body(&p, v);

		sink = b.x + b.y;
	}
	t1 = now_ns();
	e_body = (double)(t1 - t0) / ITER;

	t0 = now_ns();
	for (int i = 0; i < ITER; i++)
		sink = bearing_to(&p, v.x, v.y);
	t1 = now_ns();
	e_bear = (double)(t1 - t0) / ITER;

	/* 一次完整 nav：方位 + 距离 + 限幅 + 打包 */
	t0 = now_ns();
	for (int i = 0; i < ITER; i++) {
		float err = bearing_to(&p, v.x, v.y);
		float d = (v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y);
		float vv = 1.2f * d;
		float ww = 4.0f * err;

		if (vv > 0.30f)
			vv = 0.30f;
		if (ww > 1.50f)
			ww = 1.50f;
		if (ww < -1.50f)
			ww = -1.50f;
		cmd_make(&c, (uint32_t)i, (uint64_t)i * 20000000ull, 100, vv, ww);
		sink = c.v + c.w;
	}
	t1 = now_ns();
	e_nav = (double)(t1 - t0) / ITER;

	cmd_make(&c, 7, 1000000000ull, 100, 0.20f, 0.40f);
	t0 = now_ns();
	for (int i = 0; i < ITER; i++)
		isink = cmd_accept(&c, 1050000000ull, 6);
	t1 = now_ns();
	e_accept = (double)(t1 - t0) / ITER;

	e_all = e_nav + e_accept;

	printf("=== 一次变换要多久（%d 次平均）===\n\n", ITER);
	printf("%7.1f ns   折一次角度（wrap_pi）\n", e_wrap);
	printf("%7.1f ns   世界系 → 车体系（含 cos/sin）\n", e_body);
	printf("%7.1f ns   方位角（含 atan2）\n", e_bear);
	printf("%7.1f ns   生成一条指令（方位 + 距离 + 限幅）\n", e_nav);
	printf("%7.1f ns   收一条指令（seq + ttl）\n", e_accept);
	printf("\n整条链路 %.1f ns，占 20 ms 周期的 %.5f%%\n", e_all,
	       e_all / 20e6 * 100.0);
	printf("一个 20 ms 周期里能跑 %.0f 遍\n", 20e6 / e_all);
	printf("\n对照：一帧激光雷达要扫 100 ms，是这条链路的 %.0f 倍\n",
	       100e6 / e_all);
	return 0;
}
