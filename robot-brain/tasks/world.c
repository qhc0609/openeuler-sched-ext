/*
 * world.c —— 差速底盘的离散仿真
 *
 * 积分用的是最简单的欧拉法。真车当然不会这么走，但这一篇要看的不是
 * 底盘有多真，而是"同一台底盘、同一个场景，两种写法分别停在哪"。
 * 底盘一样，比较才成立。
 */
#include <math.h>

#include "world.h"

#define PI	3.14159265358979f
#define DEG2RAD	0.017453292519943295f

static float wrap_pi(float a)
{
	while (a > PI)
		a -= 2.0f * PI;
	while (a < -PI)
		a += 2.0f * PI;
	return a;
}

void world_init(struct world *w)
{
	*w = (struct world){ 0 };

	w->vmax = 0.30f;	/* 0.3 m/s，一个慢速巡检底盘的典型值 */
	w->wmax = 1.50f;	/* 1.5 rad/s，转一圈 4.2 秒 */
	w->amax = 0.60f;	/* 线加速度上限，不能瞬间到 0.3 m/s */
	w->dt = 0.02f;		/* 50 Hz */
}

void world_add_obs(struct world *w, float x, float y, float r)
{
	if (w->nobs < WORLD_MAX_OBS)
		w->obs[w->nobs++] = (struct obstacle){ x, y, r };
}

void world_step(struct world *w)
{
	float dv, lim;

	/* 指令先限幅，再按加速度上限逼近 */
	if (w->v_cmd > w->vmax)
		w->v_cmd = w->vmax;
	if (w->v_cmd < -w->vmax)
		w->v_cmd = -w->vmax;
	if (w->w_cmd > w->wmax)
		w->w_cmd = w->wmax;
	if (w->w_cmd < -w->wmax)
		w->w_cmd = -w->wmax;

	lim = w->amax * w->dt;
	dv = w->v_cmd - w->v;
	if (dv > lim)
		dv = lim;
	if (dv < -lim)
		dv = -lim;
	w->v += dv;
	w->w = w->w_cmd;

	w->x += w->v * cosf(w->th) * w->dt;
	w->y += w->v * sinf(w->th) * w->dt;
	w->th = wrap_pi(w->th + w->w * w->dt);
	w->t += w->dt;

	/* 撞没撞 */
	for (int i = 0; i < w->nobs; i++) {
		float dx = w->x - w->obs[i].x;
		float dy = w->y - w->obs[i].y;

		if (sqrtf(dx * dx + dy * dy) < w->obs[i].r)
			w->collided = 1;
	}
}

void world_ahead(const struct world *w, float look, float half_deg,
		 struct ahead *out)
{
	float half = half_deg * DEG2RAD;

	out->dist = look;
	out->bear = 0.0f;
	out->hit = 0;

	for (int i = 0; i < w->nobs; i++) {
		float dx = w->obs[i].x - w->x;
		float dy = w->obs[i].y - w->y;
		float d = sqrtf(dx * dx + dy * dy);
		float rel;

		if (d < 1e-4f)
			continue;
		rel = wrap_pi(atan2f(dy, dx) - w->th);
		if (fabsf(rel) > half)
			continue;
		d -= w->obs[i].r;	/* 从"到圆心"换成"到表面" */
		if (d < out->dist) {
			out->dist = d;
			out->bear = rel;
			out->hit = 1;
		}
	}
}
