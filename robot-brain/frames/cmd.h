/*
 * cmd.h —— 标准化运动指令
 *
 * 底盘只认两个数：线速度 v、角速度 w。但一条能用的指令不止两个数。
 *
 *   v, w     车体系的速度。v 沿车头，w 绕车心
 *   t_ns     这条指令是什么时候算出来的
 *   ttl_ms   多久之内有效
 *   seq      序号，用来丢掉乱序到达的旧指令
 *
 * 少一个都不行：没有 ttl，一条 400 毫秒前的指令照样执行，车会朝错方向
 * 走；没有 seq，两条指令在路上交叉到达，后发先至，车会先转后直。
 */
#ifndef CMD_H
#define CMD_H

#include <stdint.h>

struct motion_cmd {
	uint32_t seq;
	uint32_t ttl_ms;
	uint64_t t_ns;
	float v, w;
};

void cmd_make(struct motion_cmd *c, uint32_t seq, uint64_t t_ns,
	      uint32_t ttl_ms, float v, float w);

/* 收不收：序号要比上一条新，时间没超过 ttl */
int cmd_accept(const struct motion_cmd *c, uint64_t now_ns, uint32_t last_seq);

/* 等比缩放：v 和 w 乘同一个系数，曲率 v/w 不变 */
void cmd_limit_ratio(struct motion_cmd *c, float vmax, float wmax);

/* 分别截断：v 归 v 的限、w 归 w 的限，曲率会变 */
void cmd_limit_each(struct motion_cmd *c, float vmax, float wmax);

#endif
