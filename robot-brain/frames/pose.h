/*
 * pose.h —— 位姿、角度、两个坐标系
 *
 * 位姿是三个数，不是两个。少了 θ，你只知道车在哪儿，不知道它朝哪儿，
 * 后面所有"往前"都不成立。
 *
 * 两个坐标系：
 *   世界系  x 向右、y 向上，不随车动。地图、目标点、障碍都在这里。
 *   车体系  x 指向车头、y 指向车左侧，跟着车转。传感器读数、
 *           速度指令都在这里。
 *
 * 全部函数都默认：角度用弧度，逆时针为正，左正右负。
 */
#ifndef POSE_H
#define POSE_H

#define PI_F	3.14159265358979f
#define DEG2RAD	0.017453292519943295f
#define RAD2DEG	57.29577951308232f

struct pose {
	float x, y;	/* 米，世界系 */
	float th;	/* 弧度，世界系，从 +x 轴逆时针 */
};

struct vec2 {
	float x, y;
};

/* 折进 (-π, π]。周期量，不折就会越攒越大 */
float wrap_pi(float a);

/* 世界系向量 → 车体系：绕 -th 转一次 */
struct vec2 pose_to_body(const struct pose *p, struct vec2 w);

/* 车体系向量 → 世界系：绕 +th 转一次 */
struct vec2 body_to_world(const struct pose *p, struct vec2 b);

/* 世界系一个点，相对车头的方位角。左正右负，0 是正前 */
float bearing_to(const struct pose *p, float tx, float ty);

#endif
