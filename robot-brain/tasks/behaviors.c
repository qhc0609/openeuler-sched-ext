/*
 * behaviors.c —— 四个叶子任务：前进、转弯、避障、停车
 *
 * 四个任务都遵守同一条规矩：
 *
 *   **成功条件是"世界到了某个状态"，不是"我执行了多少步"。**
 *
 * 前进写成"到 (0.80, 0.00) 这个点"，不写成"再走 0.80 米"。
 * 这条规矩是拆解能不能成立的关键，第六节会用量出来的数说明为什么。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "task.h"
#include "world.h"

#define PI	3.14159265358979f

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

/* ------------------------------ 转弯 ------------------------------ */

struct turn_st {
	float target;
};

static int turn_tick(struct task *t, struct world *w)
{
	struct turn_st *s = t->st;
	float err = wrap_pi(s->target - w->th);

	g_active = t->name;
	w->v_cmd = 0.0f;
	w->w_cmd = clampf(6.0f * err, -w->wmax, w->wmax);

	/* 角度到位，而且真的停下来了，才算转完 */
	if (fabsf(err) < 0.008f && fabsf(w->w) < 0.05f)
		return TASK_SUCCESS;
	return TASK_RUNNING;
}

struct task *task_turn(float target_th)
{
	struct task *t = calloc(1, sizeof(*t));
	struct turn_st *s = calloc(1, sizeof(*s));

	s->target = wrap_pi(target_th);
	t->name = "转弯";
	t->tick = turn_tick;
	t->st = s;
	return t;
}

/* ------------------------------ 前进 ------------------------------ */

enum { G_TURN = 0, G_DRIVE = 1 };

struct goto_st {
	float tx, ty;
	int phase;
	char label[40];
};

static int goto_tick(struct task *t, struct world *w)
{
	struct goto_st *s = t->st;
	float dx = s->tx - w->x;
	float dy = s->ty - w->y;
	float d = sqrtf(dx * dx + dy * dy);
	float err;

	g_active = t->name;

	/*
	 * 到点判定用的是"离目标还剩多远"，不是"这一趟走了多远"。
	 * 中途被避障打断、绕了一圈回来，这个数自动是对的。
	 */
	if (d < 0.012f) {
		w->v_cmd = 0.0f;
		w->w_cmd = 0.0f;
		return TASK_SUCCESS;
	}

	err = wrap_pi(atan2f(dy, dx) - w->th);

	if (s->phase == G_TURN) {
		w->v_cmd = 0.0f;
		w->w_cmd = clampf(6.0f * err, -w->wmax, w->wmax);
		if (fabsf(err) < 0.05f)
			s->phase = G_DRIVE;
		return TASK_RUNNING;
	}

	/* 边走边修方向；偏得太多就先停下重新对准 */
	if (fabsf(err) > 0.6f) {
		s->phase = G_TURN;
		w->v_cmd = 0.0f;
		w->w_cmd = 0.0f;
		return TASK_RUNNING;
	}

	w->w_cmd = clampf(4.0f * err, -w->wmax, w->wmax);
	w->v_cmd = fminf(w->vmax, 1.2f * d);	/* 快到点就减速 */
	return TASK_RUNNING;
}

static void goto_reset(struct task *t)
{
	struct goto_st *s = t->st;

	s->phase = G_TURN;	/* 下次启用重新对准 */
}

struct task *task_goto(float x, float y)
{
	struct task *t = calloc(1, sizeof(*t));
	struct goto_st *s = calloc(1, sizeof(*s));

	s->tx = x;
	s->ty = y;
	s->phase = G_TURN;
	snprintf(s->label, sizeof(s->label), "前进→(%.2f,%.2f)", x, y);

	t->name = s->label;
	t->tick = goto_tick;
	t->reset = goto_reset;
	t->st = s;
	return t;
}

/* ------------------------------ 避障 ------------------------------ */

enum { A_IDLE = 0, A_BACK, A_TURN, A_FWD, A_RETURN };

struct avoid_st {
	int phase;
	float th0;		/* 触发时的朝向，绕完要转回来 */
	float ox, oy;		/* 当前阶段起点的位置 */
	float side;		/* +1 往左绕，-1 往右绕 */
};

#define A_LOOK		0.20f	/* 前方 20 厘米内有东西才算挡路 */
#define A_HALF		45.0f	/* 只看正前方 ±45 度 */
#define A_TRIG		0.18f	/* 到表面小于 18 厘米就动手 */
#define A_BACK_M	0.15f
#define A_TURN_RAD	1.047f	/* 60 度 */
#define A_FWD_M		0.55f

static int avoid_tick(struct task *t, struct world *w)
{
	struct avoid_st *s = t->st;
	struct ahead a;

	world_ahead(w, A_LOOK, A_HALF, &a);

	if (s->phase == A_IDLE) {
		if (!a.hit || a.dist > A_TRIG)
			return TASK_SUCCESS;	/* 没挡路，让位给下面的任务 */

		s->th0 = w->th;
		s->ox = w->x;
		s->oy = w->y;
		/* 障碍偏左就往右绕 */
		s->side = a.bear > 0.0f ? -1.0f : 1.0f;
		s->phase = A_BACK;
	}

	g_active = t->name;

	switch (s->phase) {
	case A_BACK:
		w->v_cmd = -0.12f;
		w->w_cmd = 0.0f;
		if (hypotf(w->x - s->ox, w->y - s->oy) > A_BACK_M) {
			s->ox = w->x;
			s->oy = w->y;
			s->phase = A_TURN;
		}
		break;

	case A_TURN:
		w->v_cmd = 0.0f;
		w->w_cmd = s->side * w->wmax;
		if (fabsf(wrap_pi(w->th - s->th0)) > A_TURN_RAD) {
			s->ox = w->x;
			s->oy = w->y;
			s->phase = A_FWD;
		}
		break;

	case A_FWD:
		w->v_cmd = 0.25f;
		w->w_cmd = 0.0f;
		if (hypotf(w->x - s->ox, w->y - s->oy) > A_FWD_M) {
			s->phase = A_RETURN;
		}
		break;

	case A_RETURN:
		w->v_cmd = 0.0f;
		w->w_cmd = -s->side * w->wmax;
		if (fabsf(wrap_pi(w->th - s->th0)) < 0.05f) {
			w->w_cmd = 0.0f;
			s->phase = A_IDLE;
			return TASK_SUCCESS;
		}
		break;
	}

	return TASK_RUNNING;
}

static void avoid_reset(struct task *t)
{
	struct avoid_st *s = t->st;

	s->phase = A_IDLE;
}

struct task *task_avoid(void)
{
	struct task *t = calloc(1, sizeof(*t));

	t->name = "避障";
	t->tick = avoid_tick;
	t->reset = avoid_reset;
	t->st = calloc(1, sizeof(struct avoid_st));
	return t;
}

/* ------------------------------ 停车 ------------------------------ */

static int stop_tick(struct task *t, struct world *w)
{
	g_active = t->name;
	w->v_cmd = 0.0f;
	w->w_cmd = 0.0f;

	/* 下了 0 不等于停了，要等速度真到 0 */
	if (fabsf(w->v) < 0.005f && fabsf(w->w) < 0.01f)
		return TASK_SUCCESS;
	return TASK_RUNNING;
}

struct task *task_stop(void)
{
	struct task *t = calloc(1, sizeof(*t));

	t->name = "停车";
	t->tick = stop_tick;
	t->st = calloc(1, sizeof(int));
	return t;
}
