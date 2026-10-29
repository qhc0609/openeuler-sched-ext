/*
 * fsm.h —— 五个状态，一张表
 *
 * 待机、运行、避障、停止、故障。转换只写在 fsm.c 的 TBL 里，
 * 表以外的组合一律不许发生——这一句就是"状态机"和"一串 if"的全部差别。
 *
 * 另外两条工程约束也放在表里：
 *
 *   hold_ms    进入某个状态之后，至少待够这么久才能走。
 *              1 kHz 的回路里，没有这一条，状态会在阈值附近来回翻。
 *
 *   urgent     安全事件（急停、故障）不受 hold_ms 限制。
 *              否则一次急停可能被"至少待 200 ms"推迟 200 毫秒。
 */
#ifndef FSM_H
#define FSM_H

#include <stdint.h>

enum rb_state {
	ST_BOOT = 0,
	ST_IDLE,	/* 待机：上电了，还没收到开始指令 */
	ST_RUN,		/* 运行：正常执行任务 */
	ST_AVOID,	/* 避障：前方有东西，降速绕行 */
	ST_STOP,	/* 停止：任务正常结束，或者上层叫停 */
	ST_FAULT,	/* 故障：急停、驱动器报错、越界。只能复位退出 */
	ST_N
};

/* 事件层的两个阈值。0.40 进、0.55 出，中间 0.15 米是迟滞带 */
#define NEAR_M		0.40f
#define CLEAR_M		0.55f

enum rb_event {
	EV_NONE = 0,
	EV_CMD_START,
	EV_CMD_STOP,
	EV_OBSTACLE_NEAR,
	EV_OBSTACLE_CLEAR,
	EV_ESTOP,
	EV_RESET,
	EV_FAULT,
	EV_N
};

struct rb_fsm {
	uint8_t state;
	uint64_t t_enter;	/* 进入当前状态的时刻 */
	uint64_t hold_until;	/* 在这之前不许走（除非 urgent） */
	uint32_t n_trans;
	uint32_t n_reject;	/* 表里没有这条边，被拒了几次 */
	float v_cmd;		/* 进入动作算出来的速度指令 */
	uint8_t n_enter[ST_N];	/* 每个状态进了几次 */
	uint16_t edge[ST_N][EV_N];	/* 每条边走过了几次 */
};

const char *st_name(int s);
const char *ev_name(int e);
void fsm_init(struct rb_fsm *f, uint64_t now);
int fsm_step(struct rb_fsm *f, int ev, uint64_t now);
void fsm_set_hold(int s, uint32_t ms);	/* 覆盖表里某个状态的驻留时间 */
void fsm_set_urgent(int on);		/* 安全事件要不要绕过驻留 */
int rb_event_from(float d, int state, float near, float clear);
int fsm_edges_total(void);
int fsm_edges_seen(const struct rb_fsm *f);

#endif
