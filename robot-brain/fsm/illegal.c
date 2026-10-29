/*
 * illegal.c —— 表里没写的边，能不能走过去
 *
 * 左边是表驱动：事件交给 fsm_step()，表里没有这条边就是不走。
 * 右边是所有人第一版都会写的那串 if：状态是每拍从输入重新算出来的。
 *
 * 两种写法在"一切正常"的时候一模一样。差别只在出事的时候。
 */
#include "fsm.h"

#include <stdio.h>

struct step {
	float t;
	int ev;
	float d;	/* 传感器报的距离 */
	int running;	/* 上层要求运行 */
	int estop;	/* 急停按钮电平 */
	const char *note;
};

static const struct step SEQ[] = {
	{ 0.00f, EV_RESET,          0.80f, 0, 0, "上电复位" },
	{ 0.10f, EV_CMD_START,      0.80f, 1, 0, "上层发开始" },
	{ 0.50f, EV_ESTOP,          0.80f, 1, 1, "急停按下" },
	{ 0.60f, EV_NONE,           0.80f, 1, 0, "急停松开，没人复位" },
	{ 1.00f, EV_CMD_START,      0.80f, 1, 0, "上层重发开始" },
	{ 1.50f, EV_OBSTACLE_NEAR,  0.30f, 1, 0, "前方出现障碍" },
	{ 2.00f, EV_RESET,          0.80f, 1, 0, "有人按了复位" },
};

#define NSEQ ((int)(sizeof SEQ / sizeof SEQ[0]))

/*
 * 第一版：状态每拍从输入重新算一遍。
 * 优先级写死在 if 的顺序里：急停 > 障碍 > 运行。
 */
static int naive_state(float d, int running, int estop)
{
	if (estop)
		return ST_FAULT;
	if (d < 0.45f)
		return ST_AVOID;
	if (running)
		return ST_RUN;
	return ST_IDLE;
}

int main(void)
{
	struct rb_fsm f;
	int i, diff = 0;

	fsm_init(&f, 0);

	printf("== 一段事件序列，两种写法 ==\n\n");
	printf("  时间   距离   事件     表驱动   if 链\n");

	for (i = 0; i < NSEQ; i++) {
		const struct step *s = &SEQ[i];
		uint64_t t_ns = (uint64_t)(s->t * 1e9f);
		int ns, same;

		fsm_step(&f, s->ev, t_ns);
		ns = naive_state(s->d, s->running, s->estop);
		same = (ns == f.state);
		if (!same)
			diff++;

		printf("  %4.2f   %.2f   %-7s  %-6s   %-6s  %s\n",
		       (double)s->t, (double)s->d, ev_name(s->ev),
		       st_name(f.state), st_name(ns), same ? "" : "← 不一致");
		printf("        %s\n", s->note);
	}

	printf("\n  表驱动拒绝了 %u 个事件（表里没有这条边）\n", f.n_reject);
	printf("  两种写法有 %d 个时刻不一致\n\n", diff);
	printf("  表里一共 %d 条边，这一趟走过 %d 条\n",
	       fsm_edges_total(), fsm_edges_seen(&f));
	printf("  走过的边：\n");
	for (i = 0; i < ST_N; i++) {
		int j;

		for (j = 0; j < EV_N; j++)
			if (f.edge[i][j])
				printf("    %s --%s--> %d 次\n", st_name(i),
				       ev_name(j), f.edge[i][j]);
	}
	return 0;
}
