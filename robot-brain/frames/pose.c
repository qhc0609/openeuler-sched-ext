/*
 * pose.c —— 三个函数，加起来不到三十行
 *
 * 坐标系变换在机器人里被说得玄乎，其实就一件事：把同一个向量
 * 换个轴看。换个轴 = 乘一个旋转矩阵，旋转矩阵里只有 cos 和 sin。
 */
#include <math.h>

#include "pose.h"

/*
 * 折进 (-π, π]。
 *
 * 用 while 不用 fmod：fmod 对负数的行为是"向零取整"，-3π 折出来是 -π，
 * 不在一半的区间里。这一点在 ±π 附近会咬人，宁可多写两行。
 */
float wrap_pi(float a)
{
	while (a > PI_F)
		a -= 2.0f * PI_F;
	while (a <= -PI_F)
		a += 2.0f * PI_F;
	return a;
}

/*
 * 世界系 → 车体系。
 *
 * 车头朝 th，那么"世界系的 +x 轴"在车体系里看就是 -th 方向。所以是
 * 绕 -th 转：矩阵 [[cos, sin], [-sin, cos]]。
 */
struct vec2 pose_to_body(const struct pose *p, struct vec2 w)
{
	float c = cosf(p->th);
	float s = sinf(p->th);
	struct vec2 b;

	b.x = c * w.x + s * w.y;
	b.y = -s * w.x + c * w.y;
	return b;
}

/* 车体系 → 世界系，反着转回去 */
struct vec2 body_to_world(const struct pose *p, struct vec2 b)
{
	float c = cosf(p->th);
	float s = sinf(p->th);
	struct vec2 w;

	w.x = c * b.x - s * b.y;
	w.y = s * b.x + c * b.y;
	return w;
}

/*
 * 方位角 = 世界系的指向，减掉车头朝向。
 *
 * 那一句 wrap_pi 不是保险，是必须：atan2 给的是 (-π, π]，减完 th 之后
 * 可能落到 (-2π, 2π)。不折回去，车会为了 2 度的误差转 358 度。
 */
float bearing_to(const struct pose *p, float tx, float ty)
{
	return wrap_pi(atan2f(ty - p->y, tx - p->x) - p->th);
}
