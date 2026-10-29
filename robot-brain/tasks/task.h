/*
 * task.h —— 一个任务长什么样，以及怎么把任务拼起来
 *
 * 拆解的全部内容就在这个结构体里：一个任务 = 一个名字 + 一个 tick()。
 * tick 每 20 毫秒被调一次，返回三种结果之一：
 *
 *   TASK_RUNNING   还没干完，下一拍继续
 *   TASK_SUCCESS   干完了
 *   TASK_FAILURE   干不成
 *
 * 有了这三种返回值，任务之间才能组合——顺序、抢占，都只依赖返回值，
 * 不依赖任务内部怎么实现。
 */
#ifndef TASK_H
#define TASK_H

struct world;

enum task_ret {
	TASK_RUNNING = 0,
	TASK_SUCCESS = 1,
	TASK_FAILURE = -1,
};

struct task {
	const char *name;
	int  (*tick)(struct task *t, struct world *w);
	void (*reset)(struct task *t);	/* 被重新启用时清自己的状态 */
	void *st;			/* 这个任务自己的状态 */
};

/* 本拍正在执行的任务名，mission.c 用它打轨迹 */
extern const char *g_active;

int  task_tick(struct task *t, struct world *w);
void task_reset(struct task *t);

/* 组合器 */
struct task *seq_n(struct task **kids, int n);	/* 顺序：一个一个来 */
struct task *arb(struct task *hi, struct task *lo);	/* 抢占：hi 忙时 lo 停 */

/* 叶子任务，都在 behaviors.c 里 */
struct task *task_turn(float target_th);
struct task *task_goto(float x, float y);
struct task *task_avoid(void);
struct task *task_stop(void);

#endif
