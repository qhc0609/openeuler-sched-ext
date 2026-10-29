/*
 * chatter.c —— 阈值附近，状态会翻多少次
 *
 * 场景：一条直线走廊，五个"人"先后从车前面横穿过去。传感器报的距离
 * 是分段线性的，叠了 ±2 cm 的噪声（1 cm 分辨率 + 抖动）。
 *
 * 五个坑很密：每个只持续 150 ms，坑和坑之间隔 200 ms。
 * 于是三种配置会给出三个完全不同的答案：
 *
 *   A  单阈值 0.45，进出都用这一个数，没有驻留
 *   B  双阈值 0.40 进 / 0.55 出（迟滞），没有驻留
 *   C  双阈值 + 进入避障后至少待 300 ms（驻留）
 *
 * A 的问题是噪声一穿阈值就翻；B 解决了噪声，但五个坑还是五进五出；
 * C 把间隔 200 ms 的坑合成一次避障。
 *
 * 这里用的是模拟时间（i × 1 ms），不是墙上时钟。跑多快都一样，结果可复现。
 */
#include "fsm.h"

#include <math.h>
#include <stdio.h>

#define HZ		1000
#define DUR_S		6
#define NTICK		(HZ * DUR_S)
#define NOISE_M		0.02f

static const struct { float t, d; } PROF[] = {
	{ 0.00f, 0.80f },
	{ 3.00f, 0.80f },
	{ 3.10f, 0.32f },
	{ 3.25f, 0.80f },
	{ 3.45f, 0.32f },
	{ 3.60f, 0.80f },
	{ 3.80f, 0.32f },
	{ 3.95f, 0.80f },
	{ 4.15f, 0.32f },
	{ 4.30f, 0.80f },
	{ 4.50f, 0.32f },
	{ 4.65f, 0.80f },
	{ 6.00f, 0.80f },
};

#define NPROF ((int)(sizeof PROF / sizeof PROF[0]))

static float dist_at(float t)
{
	int i;

	if (t <= PROF[0].t)
		return PROF[0].d;
	for (i = 1; i < NPROF; i++) {
		if (t <= PROF[i].t) {
			float span = PROF[i].t - PROF[i - 1].t;
			float k = (t - PROF[i - 1].t) / span;

			return PROF[i - 1].d + k * (PROF[i].d - PROF[i - 1].d);
		}
	}
	return PROF[NPROF - 1].d;
}

/* 定种子的 xorshift32：两次跑出来的噪声一模一样 */
static uint32_t g_rng = 0x9E3779B9u;

static float noise(void)
{
	uint32_t x = g_rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_rng = x;
	return ((float)(x >> 8) / 16777216.0f - 0.5f) * 2.0f * NOISE_M;
}

struct result {
	int trans;
	int enters;
	float t_avoid;
	float v_sum;
};

static struct result run(const char *tag, float near, float clear,
			 uint32_t hold_ms)
{
	struct rb_fsm f;
	struct result r = { 0, 0, 0.0f, 0.0f };
	int i;

	g_rng = 0x9E3779B9u;
	fsm_set_hold(ST_AVOID, hold_ms);
	fsm_init(&f, 0);

	for (i = 0; i < NTICK; i++) {
		uint64_t t_ns = (uint64_t)i * 1000000ull;
		float d = dist_at((float)i / (float)HZ) + noise();
		int ev = EV_NONE;

		if (i == 0) {
			ev = EV_RESET;
		} else if (i == 1) {
			ev = EV_CMD_START;
		} else if (f.state == ST_RUN && d < near) {
			ev = EV_OBSTACLE_NEAR;
		} else if (f.state == ST_AVOID && d > clear) {
			ev = EV_OBSTACLE_CLEAR;
		}
		fsm_step(&f, ev, t_ns);

		if (f.state == ST_AVOID)
			r.t_avoid += 1.0f / (float)HZ;
		r.v_sum += f.v_cmd;
	}

	r.trans = (int)f.n_trans;
	r.enters = f.n_enter[ST_AVOID];
	printf("-- %s\n", tag);
	printf("   状态翻转 %4d 次   进入避障 %3d 次\n", r.trans, r.enters);
	printf("   避障时间占比 %5.1f%%   平均速度 %.3f m/s\n\n",
	       r.t_avoid * 100.0f / (float)DUR_S,
	       r.v_sum / (float)NTICK);
	return r;
}

int main(void)
{
	printf("== 五个 150 ms 的坑，间隔 200 ms，距离噪声 ±2 cm ==\n");
	printf("   1 kHz，6 秒，共 %d 拍\n\n", NTICK);

	run("A  单阈值 0.45 / 0.45，无驻留", 0.45f, 0.45f, 0);
	run("B  双阈值 0.40 / 0.55，无驻留", 0.40f, 0.55f, 0);
	run("C  双阈值 0.40 / 0.55，驻留 300 ms", 0.40f, 0.55f, 300);

	fsm_set_hold(ST_AVOID, 0);
	return 0;
}
