/*
 * cmd.c —— 一条指令的三道检查
 *
 * 顺序不能换：先看序号（旧指令直接扔），再看时间（过期也扔），
 * 最后才限幅。反过来先限幅，等于把一条本该扔掉的指令改小再执行。
 */
#include <math.h>

#include "cmd.h"

void cmd_make(struct motion_cmd *c, uint32_t seq, uint64_t t_ns,
	      uint32_t ttl_ms, float v, float w)
{
	c->seq = seq;
	c->ttl_ms = ttl_ms;
	c->t_ns = t_ns;
	c->v = v;
	c->w = w;
}

int cmd_accept(const struct motion_cmd *c, uint64_t now_ns, uint32_t last_seq)
{
	if (c->seq <= last_seq)
		return 0;		/* 旧指令，乱序到了 */
	if (now_ns < c->t_ns)
		return 0;		/* 时间倒退，两边的钟不是一个 */
	if ((now_ns - c->t_ns) / 1000000ull > (uint64_t)c->ttl_ms)
		return 0;		/* 过期 */
	return 1;
}

/*
 * 等比缩放。
 *
 * v 和 w 同乘一个 k，转弯半径 v/w 不变，只是走得慢一点。这是"超速"，
 * 不是"超转"——该拐的弯还是那个弯。
 */
void cmd_limit_ratio(struct motion_cmd *c, float vmax, float wmax)
{
	float k = 1.0f;

	if (fabsf(c->v) > vmax)
		k = fminf(k, vmax / fabsf(c->v));
	if (fabsf(c->w) > wmax)
		k = fminf(k, wmax / fabsf(c->w));

	c->v *= k;
	c->w *= k;
}

/*
 * 分别截断。
 *
 * 看起来更"直接"，实际改的是轨迹形状：v 砍到 0.30、w 砍到 1.50，
 * 转弯半径从 v/w 变成另一个数。想直着走却拐了弯，多半是这里。
 */
void cmd_limit_each(struct motion_cmd *c, float vmax, float wmax)
{
	if (c->v > vmax)
		c->v = vmax;
	if (c->v < -vmax)
		c->v = -vmax;
	if (c->w > wmax)
		c->w = wmax;
	if (c->w < -wmax)
		c->w = -wmax;
}
