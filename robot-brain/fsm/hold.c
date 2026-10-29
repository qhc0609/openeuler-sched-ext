/*
 * hold.c —— 驻留期里按了急停
 *
 * 进入避障之后要"至少待 300 ms"。这句话保护的是状态别乱翻，
 * 但它也会保护住一条本来该立刻走的边：急停。
 *
 * 时间线：t=0 进运行（运行状态本身也有 100 ms 驻留），
 * t=100 ms 前方出现障碍，进避障（驻留到 400 ms），
 * t=101 ms 有人按下急停。
 *
 * 急停事件要不要受这 300 ms 限制？两个答案：
 *
 *   绕过   101 ms 就进了 FAULT
 *   不绕过 要等到 400 ms，驻留期满
 *
 * 差的 299 ms 里车还在走，速度 0.10 m/s 就是 3.0 cm，
 * 换成运行速度 0.30 m/s 就是 9.0 cm。
 */
#include "fsm.h"

#include <stdio.h>

#define T_NEAR_MS	100	/* 进运行 100 ms 后出现障碍 */
#define T_ESTOP_MS	101	/* 再 1 ms 后按下急停 */

/* 一直按着急停，返回进 FAULT 的时刻（毫秒）；走不到就返回 -1 */
static int until_fault(int urgent)
{
	struct rb_fsm f;
	int ms;

	fsm_set_hold(ST_AVOID, 300);
	fsm_set_urgent(urgent);
	fsm_init(&f, 0);
	fsm_step(&f, EV_RESET, 0);
	fsm_step(&f, EV_CMD_START, 0);
	fsm_step(&f, EV_OBSTACLE_NEAR, (uint64_t)T_NEAR_MS * 1000000ull);

	if (f.state != ST_AVOID) {
		printf("   进避障失败，状态是 %s\n", st_name(f.state));
		return -1;
	}

	for (ms = T_NEAR_MS; ms <= 1000; ms++) {
		if (ms >= T_ESTOP_MS)
			fsm_step(&f, EV_ESTOP, (uint64_t)ms * 1000000ull);
		if (f.state == ST_FAULT)
			return ms;
	}
	return -1;
}

int main(void)
{
	int a, b;

	printf("== t=%d ms 进避障（驻留 300 ms），t=%d ms 按下急停 ==\n\n",
	       T_NEAR_MS, T_ESTOP_MS);

	a = until_fault(1);
	printf("   安全事件绕过驻留   第 %d ms 进 FAULT，比按下晚 %d ms\n",
	       a, a - T_ESTOP_MS);

	b = until_fault(0);
	printf("   安全事件不绕过     第 %d ms 进 FAULT，比按下晚 %d ms\n",
	       b, b - T_ESTOP_MS);

	printf("\n   多等的 %d ms 里，车以 0.10 m/s 又走了 %.1f cm，"
	       "以 0.30 m/s 是 %.1f cm\n",
	       b - a, (double)(b - a) / 1000.0 * 0.10 * 100.0,
	       (double)(b - a) / 1000.0 * 0.30 * 100.0);

	fsm_set_urgent(1);
	fsm_set_hold(ST_AVOID, 0);
	return 0;
}
