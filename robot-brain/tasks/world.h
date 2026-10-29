/*
 * world.h —— 一台差速底盘 + 几个圆形障碍物
 *
 * 这一篇讲"任务拆解"，所以世界要足够简单：只看得到位姿、速度、障碍，
 * 这样每个任务的成败都能用"离目标多远"这种硬指标衡量，不靠眼睛看。
 */
#ifndef WORLD_H
#define WORLD_H

#define WORLD_MAX_OBS	8

struct obstacle {
	float x, y, r;
};

struct world {
	/* 底盘状态 */
	float x, y, th;		/* 位姿：米、米、弧度 */
	float v, w;		/* 实际线速度、角速度 */
	float v_cmd, w_cmd;	/* 本拍下发的指令 */

	/* 底盘上限 */
	float vmax, wmax, amax;
	float dt;		/* 控制周期，0.02 s = 50 Hz */
	double t;		/* 仿真时间 */

	struct obstacle obs[WORLD_MAX_OBS];
	int nobs;
	int collided;		/* 撞上了就置 1 */
};

/* 前方视野的读数 */
struct ahead {
	float dist;		/* 到障碍表面，封顶 look */
	float bear;		/* 相对车头的方位角，左正右负 */
	int hit;		/* 视野里有没有东西 */
};

void world_init(struct world *w);
void world_step(struct world *w);
void world_add_obs(struct world *w, float x, float y, float r);
void world_ahead(const struct world *w, float look, float half_deg,
		 struct ahead *out);

#endif
