/*
 * mission.c —— 把四个动作拼成一个任务，跑一遍，看它停在哪
 *
 * 场景：底盘从 (0, 0) 出发，要去 (1.00, 0.55)。
 * 中间 (0.80, 0.10) 有个半径 10 厘米的柱子，正好卡在路上。
 *
 * 任务树：
 *
 *   抢占
 *   ├── 避障                    ← 高优先级，挡路就抢
 *   └── 顺序
 *       ├── 前进→(0.45, 0.00)
 *       ├── 前进→(1.00, 0.00)
 *       ├── 前进→(1.00, 0.55)
 *       └── 停车
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "task.h"
#include "world.h"

#define GOAL_X	1.00f
#define GOAL_Y	0.55f
#define MAX_TICK 3000		/* 60 秒，跑不完就算失败 */

int main(void)
{
	struct world w;
	struct task *kids[4];
	struct task *root;
	const char *prev = "";
	int ticks = 0, r = TASK_RUNNING;

	world_init(&w);
	world_add_obs(&w, 0.80f, 0.10f, 0.10f);

	kids[0] = task_goto(0.45f, 0.00f);
	kids[1] = task_goto(1.00f, 0.00f);
	kids[2] = task_goto(1.00f, 0.55f);
	kids[3] = task_stop();

	root = arb(task_avoid(), seq_n(kids, 4));

	printf("起点 (0.00, 0.00)，朝向 0 度；目标 (%.2f, %.2f)\n", GOAL_X, GOAL_Y);
	printf("障碍 (0.80, 0.10)，半径 10 cm\n\n");

	while (ticks < MAX_TICK) {
		r = task_tick(root, &w);
		world_step(&w);
		ticks++;

		if (strcmp(prev, g_active)) {
			printf("[%6.2f s] %-18s 位置 (%5.2f, %5.2f)\n",
			       w.t, g_active, w.x, w.y);
			prev = g_active;
		}
		if (r != TASK_RUNNING)
			break;
	}

	printf("\n任务%s，用了 %.2f 秒（%d 拍）\n",
	       r == TASK_SUCCESS ? "完成" : "没完成", w.t, ticks);
	printf("停在 (%.3f, %.3f)，朝向 %.1f 度\n",
	       w.x, w.y, w.th * 57.2957795f);
	printf("离目标还剩 %.1f mm\n",
	       1000.0f * hypotf(GOAL_X - w.x, GOAL_Y - w.y));
	printf("路上撞了吗：%s\n", w.collided ? "撞了" : "没有");
	return 0;
}
