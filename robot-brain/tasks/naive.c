/*
 * naive.c —— 不拆的写法：一个循环，四个 if
 *
 * 同样一台底盘，同一个起点，同一个目标，同一根柱子。
 * 但任务用"增量"描述：
 *
 *   前进 0.45 米 → 前进 0.55 米 → 左转 90 度 → 前进 0.55 米 → 停
 *
 * 避障写成循环里的一个 if，绕行的那几拍照样在走，也照样被记进里程。
 *
 * 这是绝大多数人写第一版机器人程序时的样子，不是故意写错。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "world.h"

#define PI	3.14159265358979f
#define GOAL_X	1.00f
#define GOAL_Y	0.55f
#define MAX_TICK 3000

#define A_LOOK		0.20f
#define A_HALF		45.0f
#define A_TRIG		0.18f
#define A_BACK_M	0.15f
#define A_TURN_RAD	1.047f
#define A_FWD_M		0.55f

static float wrap_pi(float a)
{
	while (a > PI)
		a -= 2.0f * PI;
	while (a < -PI)
		a += 2.0f * PI;
	return a;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* 避障子状态，写在循环里的那一段 */
static int   esc;
static float esc_th0, esc_x, esc_y, esc_side;

int main(void)
{
	struct world w;
	int phase = 0, ticks = 0;
	float odom = 0.0f, turned = 0.0f;
	const char *phase_name[] = {
		"前进 0.45 m", "前进 0.55 m", "左转 90 度", "前进 0.55 m", "停车"
	};
	const char *prev = "";

	world_init(&w);
	world_add_obs(&w, 0.80f, 0.10f, 0.10f);

	printf("起点 (0.00, 0.00)，朝向 0 度；目标 (%.2f, %.2f)\n", GOAL_X, GOAL_Y);
	printf("障碍 (0.80, 0.10)，半径 10 cm\n\n");

	while (ticks < MAX_TICK) {
		struct ahead a;
		const char *label;

		world_ahead(&w, A_LOOK, A_HALF, &a);

		if (!esc && a.hit && a.dist <= A_TRIG) {
			esc = 1;
			esc_th0 = w.th;
			esc_x = w.x;
			esc_y = w.y;
			esc_side = a.bear > 0.0f ? -1.0f : 1.0f;
		}

		if (esc) {
			/* 绕行，和拆解版用的是同一组参数 */
			switch (esc) {
			case 1:
				w.v_cmd = -0.12f;
				w.w_cmd = 0.0f;
				if (hypotf(w.x - esc_x, w.y - esc_y) > A_BACK_M) {
					esc_x = w.x;
					esc_y = w.y;
					esc = 2;
				}
				break;
			case 2:
				w.v_cmd = 0.0f;
				w.w_cmd = esc_side * w.wmax;
				if (fabsf(wrap_pi(w.th - esc_th0)) > A_TURN_RAD) {
					esc_x = w.x;
					esc_y = w.y;
					esc = 3;
				}
				break;
			case 3:
				w.v_cmd = 0.25f;
				w.w_cmd = 0.0f;
				if (hypotf(w.x - esc_x, w.y - esc_y) > A_FWD_M)
					esc = 4;
				break;
			case 4:
				w.v_cmd = 0.0f;
				w.w_cmd = -esc_side * w.wmax;
				if (fabsf(wrap_pi(w.th - esc_th0)) < 0.05f) {
					w.w_cmd = 0.0f;
					esc = 0;
				}
				break;
			}
		} else {
			/* 正常任务 */
			switch (phase) {
			case 0:
				w.v_cmd = fminf(w.vmax,
						fmaxf(0.06f, 1.2f * (0.45f - odom)));
				w.w_cmd = 0.0f;
				if (odom >= 0.45f) {
					phase = 1;
					odom = 0.0f;
				}
				break;
			case 1:
				w.v_cmd = fminf(w.vmax,
						fmaxf(0.06f, 1.2f * (0.55f - odom)));
				w.w_cmd = 0.0f;
				if (odom >= 0.55f) {
					phase = 2;
					turned = 0.0f;
				}
				break;
			case 2:
				w.v_cmd = 0.0f;
				w.w_cmd = clampf(6.0f * (0.5f * PI - turned),
						 -w.wmax, w.wmax);
				if (turned >= 0.5f * PI - 0.008f) {
					phase = 3;
					odom = 0.0f;
				}
				break;
			case 3:
				w.v_cmd = fminf(w.vmax,
						fmaxf(0.06f, 1.2f * (0.55f - odom)));
				w.w_cmd = 0.0f;
				if (odom >= 0.55f)
					phase = 4;
				break;
			case 4:
				w.v_cmd = 0.0f;
				w.w_cmd = 0.0f;
				break;
			}
		}

		world_step(&w);
		ticks++;

		/* 里程：只要轮子在转就往上加，不管是往前往后、还是绕行 */
		odom += fabsf(w.v) * w.dt;
		if (phase == 2 && !esc)
			turned += w.w * w.dt;

		label = esc ? "避障（写在循环里）" : phase_name[phase];
		if (strcmp(prev, label)) {
			printf("[%6.2f s] %-14s 位置 (%5.2f, %5.2f)  里程 %.2f m\n",
			       w.t, label, w.x, w.y, odom);
			prev = label;
		}

		if (phase == 4 && fabsf(w.v) < 0.005f)
			break;
	}

	printf("\n结束，用了 %.2f 秒（%d 拍）\n", w.t, ticks);
	printf("停在 (%.3f, %.3f)，朝向 %.1f 度\n",
	       w.x, w.y, w.th * 57.2957795f);
	printf("离目标还剩 %.1f mm\n",
	       1000.0f * hypotf(GOAL_X - w.x, GOAL_Y - w.y));
	printf("路上撞了吗：%s\n", w.collided ? "撞了" : "没有");
	return 0;
}
