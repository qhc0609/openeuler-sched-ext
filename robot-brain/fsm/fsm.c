/*
 * fsm.c —— 一张表，一个查表循环
 *
 * 整个状态机就是 TBL 这张表加上下面三十行的 fsm_step()。
 * 加一个状态、改一条转换，都是改表，不改代码。
 */
#include "fsm.h"

#include <stddef.h>

struct rule {
	uint8_t from;
	uint8_t ev;
	uint8_t to;
	uint16_t hold_ms;	/* 进入 to 之后至少待这么久 */
};

/*
 * 表里没有的边，就是不许发生的边。
 *
 * 特别注意 FAULT 这一行：只有 EV_RESET 能出去，没有 EV_CMD_START，
 * 也没有 EV_OBSTACLE_NEAR。故障状态不会被"上层又发了开始指令"顶掉，
 * 也不会被"前方有障碍"顶掉。要出来，只能复位。
 */
static const struct rule TBL[] = {
	{ ST_BOOT,  EV_RESET,          ST_IDLE,    0   },
	{ ST_BOOT,  EV_ESTOP,          ST_FAULT,   0   },

	{ ST_IDLE,  EV_CMD_START,      ST_RUN,     100 },
	{ ST_IDLE,  EV_ESTOP,          ST_FAULT,   0   },

	{ ST_RUN,   EV_OBSTACLE_NEAR,  ST_AVOID,   0   },
	{ ST_RUN,   EV_CMD_STOP,       ST_STOP,    0   },
	{ ST_RUN,   EV_ESTOP,          ST_FAULT,   0   },
	{ ST_RUN,   EV_FAULT,          ST_FAULT,   0   },

	{ ST_AVOID, EV_OBSTACLE_CLEAR, ST_RUN,     0   },
	{ ST_AVOID, EV_CMD_STOP,       ST_STOP,    0   },
	{ ST_AVOID, EV_ESTOP,          ST_FAULT,   0   },
	{ ST_AVOID, EV_FAULT,          ST_FAULT,   0   },

	{ ST_STOP,  EV_CMD_START,      ST_RUN,     100 },
	{ ST_STOP,  EV_ESTOP,          ST_FAULT,   0   },

	{ ST_FAULT, EV_RESET,          ST_IDLE,    500 },
};

#define NTBL ((int)(sizeof TBL / sizeof TBL[0]))

/* -1 表示用表里的值；>=0 表示临时覆盖，给对照实验用 */
static int32_t g_hold[ST_N] = { [0 ... ST_N - 1] = -1 };

static int g_urgent = 1;

void fsm_set_hold(int s, uint32_t ms)
{
	if (s >= 0 && s < ST_N)
		g_hold[s] = (int32_t)ms;
}

void fsm_set_urgent(int on)
{
	g_urgent = on;
}

static const char *const ST_NAME[ST_N] = {
	"BOOT", "IDLE", "RUN", "AVOID", "STOP", "FAULT",
};

static const char *const EV_NAME[EV_N] = {
	"-", "START", "STOP", "NEAR", "CLEAR", "ESTOP", "RESET", "FAULT",
};

const char *st_name(int s)
{
	return (s >= 0 && s < ST_N) ? ST_NAME[s] : "?";
}

const char *ev_name(int e)
{
	return (e >= 0 && e < EV_N) ? EV_NAME[e] : "?";
}

/*
 * 进入动作。真机上这里还要做别的事：AVOID 要清掉上一段规划、
 * FAULT 要关使能、IDLE 要把里程计归零。这里只留速度，因为
 * 后面的实验只量速度。
 */
static void on_enter(struct rb_fsm *f, int s, uint64_t now)
{
	switch (s) {
	case ST_RUN:
		f->v_cmd = 0.30f;
		break;
	case ST_AVOID:
		f->v_cmd = 0.10f;
		break;
	default:		/* BOOT / IDLE / STOP / FAULT */
		f->v_cmd = 0.0f;
		break;
	}
	f->t_enter = now;
	f->n_enter[s]++;
}

void fsm_init(struct rb_fsm *f, uint64_t now)
{
	int i, j;

	f->state = ST_BOOT;
	f->hold_until = 0;
	f->n_trans = 0;
	f->n_reject = 0;
	f->v_cmd = 0.0f;
	for (i = 0; i < ST_N; i++) {
		f->n_enter[i] = 0;
		for (j = 0; j < EV_N; j++)
			f->edge[i][j] = 0;
	}
	on_enter(f, ST_BOOT, now);
}

/*
 * 事件层：传感器给的是距离，状态机要的是事件。
 *
 * 迟滞写在这一层，不写进表里。因为表只认离散事件——它不知道"距离"是什么，
 * 也不需要知道。换一个传感器（超声换激光、换视觉），只改这个函数，
 * 表一行都不用动。
 *
 * near 和 clear 是两个数，不是同一个数。这就是迟滞。
 */
int rb_event_from(float d, int state, float near, float clear)
{
	if (state == ST_RUN && d < near)
		return EV_OBSTACLE_NEAR;
	if (state == ST_AVOID && d > clear)
		return EV_OBSTACLE_CLEAR;
	return EV_NONE;
}

/*
 * 走一步。返回 1 表示真的转了，0 表示没转（事件不适用、或者还在驻留期）。
 */
int fsm_step(struct rb_fsm *f, int ev, uint64_t now)
{
	int urgent = g_urgent && (ev == EV_ESTOP || ev == EV_FAULT);
	uint32_t hold;
	int i;

	if (ev <= EV_NONE || ev >= EV_N)
		return 0;

	for (i = 0; i < NTBL; i++) {
		const struct rule *r = &TBL[i];

		if (r->from != f->state || r->ev != ev)
			continue;

		/* 驻留期没到就不许走——但安全事件不走这条 */
		if (!urgent && now < f->hold_until)
			return 0;

		hold = (g_hold[r->to] >= 0) ? (uint32_t)g_hold[r->to] : r->hold_ms;
		f->edge[f->state][ev]++;
		f->state = r->to;
		f->hold_until = now + (uint64_t)hold * 1000000ull;
		on_enter(f, r->to, now);
		f->n_trans++;
		return 1;
	}

	f->n_reject++;
	return 0;
}

int fsm_edges_total(void)
{
	return NTBL;
}

int fsm_edges_seen(const struct rb_fsm *f)
{
	int i, j, n = 0;

	for (i = 0; i < ST_N; i++)
		for (j = 0; j < EV_N; j++)
			if (f->edge[i][j])
				n++;
	return n;
}
