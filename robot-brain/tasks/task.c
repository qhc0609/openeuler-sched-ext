/*
 * task.c —— 两个组合器：顺序、抢占
 *
 * 只有这两个就够把"前进、转弯、避障、停车"串成一个任务。
 * 行为树里那些 Sequence / Fallback / Parallel 的名字，剥掉术语之后
 * 干的就是这两件事：排队，和插队。
 */
#include <stdlib.h>

#include "task.h"
#include "world.h"

const char *g_active = "-";

int task_tick(struct task *t, struct world *w)
{
	return t->tick(t, w);
}

void task_reset(struct task *t)
{
	if (t->reset)
		t->reset(t);
}

/* ------------------------------ 顺序 ------------------------------ */

struct seq_st {
	struct task *kid[8];
	int n;
	int i;
};

/*
 * 一拍只推进一个子任务。
 *
 * 子任务返回 SUCCESS 时，这一拍不接着跑下一个，而是把它留到下一拍。
 * 代价是任务之间固定隔 20 毫秒；好处是每一拍干了什么，日志里一行不差。
 */
static int seq_tick(struct task *t, struct world *w)
{
	struct seq_st *s = t->st;
	int r;

	if (s->i >= s->n)
		return TASK_SUCCESS;

	r = task_tick(s->kid[s->i], w);

	if (r == TASK_SUCCESS) {
		task_reset(s->kid[s->i]);
		s->i++;
		if (s->i >= s->n)
			return TASK_SUCCESS;
		return TASK_RUNNING;
	}
	return r;
}

static void seq_reset(struct task *t)
{
	struct seq_st *s = t->st;

	for (int k = 0; k < s->i; k++)
		task_reset(s->kid[k]);
	s->i = 0;
}

struct task *seq_n(struct task **kids, int n)
{
	struct task *t = calloc(1, sizeof(*t));
	struct seq_st *s = calloc(1, sizeof(*s));

	if (n > 8)
		n = 8;
	for (int k = 0; k < n; k++)
		s->kid[k] = kids[k];
	s->n = n;

	t->name = "顺序";
	t->tick = seq_tick;
	t->reset = seq_reset;
	t->st = s;
	return t;
}

/* ------------------------------ 抢占 ------------------------------ */

struct arb_st {
	struct task *hi;
	struct task *lo;
};

/*
 * 高优先级任务只要还在跑，低优先级这一拍就完全不 tick。
 *
 * 注意低优先级是被"冻结"而不是被"重置"：避障结束以后，前进接着从它
 * 自己记住的目标点往下走，不需要谁告诉它"你刚才走到哪了"。
 */
static int arb_tick(struct task *t, struct world *w)
{
	struct arb_st *s = t->st;
	int r = task_tick(s->hi, w);

	if (r == TASK_RUNNING)
		return TASK_RUNNING;
	if (r == TASK_FAILURE) {
		task_reset(s->hi);
		return TASK_RUNNING;	/* 避障失败了也不许撞，下一拍再试 */
	}
	return task_tick(s->lo, w);
}

static void arb_reset(struct task *t)
{
	struct arb_st *s = t->st;

	task_reset(s->hi);
	task_reset(s->lo);
}

struct task *arb(struct task *hi, struct task *lo)
{
	struct task *t = calloc(1, sizeof(*t));
	struct arb_st *s = calloc(1, sizeof(*s));

	s->hi = hi;
	s->lo = lo;

	t->name = "抢占";
	t->tick = arb_tick;
	t->reset = arb_reset;
	t->st = s;
	return t;
}
