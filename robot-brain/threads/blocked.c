/*
 * blocked.c —— 四件事挤在一个 1 kHz 的循环里，量控制那一拍晚了多少
 *
 * 一个机器人循环每一拍要干四件事：
 *
 *   1. 控制：伺服解算，50 us
 *   2. 接收：从串口/网口取一帧传感器数据，5 us
 *   3. 决策：每 100 ms 重新规划一次，8 ms（换成视觉或模型推理就是几十毫秒）
 *   4. 日志：写一行 + fsync，这台机器上 777 us
 *
 * 五档，每一档比上一档多塞一件事，看控制回路的"迟到"怎么变。
 * 迟到 = 控制计算真正开始的时刻 - 它该开始的时刻。
 * 丢拍 = 迟到超过一整个周期，也就是这一拍彻底错过了。
 *
 * 跑法：./bin/blocked /root/rb-naive.log
 */
#include "common.h"

#include <unistd.h>

#define NSAMP		3000
#define CTL_NS		50000ull
#define RX_NS		5000ull
#define DECIDE_NS	8000000ull
#define DECIDE_EVERY	100
#define BATCH		100

static FILE *g_log;
static int g_pending;

static void log_open(const char *p)
{
	g_log = fopen(p, "w");
	if (!g_log) {
		perror(p);
		exit(1);
	}
	setvbuf(g_log, NULL, _IOFBF, 1 << 16);
	g_pending = 0;
}

static void log_put(int i, float v, int sync_now)
{
	fprintf(g_log, "%06d %.4f %.4f %.4f\n", i, v, v * 0.5f, v * 0.25f);
	if (sync_now) {
		fflush(g_log);		/* 先把缓冲区里的字节交给内核 */
		fsync(fileno(g_log));	/* 再让内核把它们落到盘上 */
	}
}

static void run(int mode, const char *path)
{
	static uint64_t lat[NSAMP];
	static const char *name[] = {
		"控制",
		"控制 + 接收",
		"控制 + 接收 + 决策",
		"控制 + 接收 + 决策 + 日志（每拍 fsync）",
		"控制 + 接收 + 决策 + 日志（攒 100 行 fsync 一次）",
	};
	uint64_t next, busy = 0;
	double per;
	int i, miss = 0;

	log_open(path);
	next = now_ns();

	for (i = 0; i < NSAMP; i++) {
		uint64_t t_ctl;

		next += PERIOD_NS;
		wait_until(next);

		/* 从这里开始量：控制计算真正开始的时刻 */
		t_ctl = now_ns();
		lat[i] = t_ctl - next;
		if (lat[i] >= PERIOD_NS)
			miss++;

		/* 1. 控制：伺服一拍 */
		burn_ns(CTL_NS);

		/* 2. 接收：取一帧 */
		if (mode >= 1)
			burn_ns(RX_NS);

		/* 3. 决策：每 100 ms 重规划一次 */
		if (mode >= 2 && i % DECIDE_EVERY == 0)
			burn_ns(DECIDE_NS);

		/* 4. 日志 */
		if (mode == 3) {
			log_put(i, 1.0f, 1);
		} else if (mode == 4) {
			log_put(i, 1.0f, 0);
			if (++g_pending == BATCH) {
				fflush(g_log);
				fsync(fileno(g_log));
				g_pending = 0;
			}
		}

		busy += now_ns() - t_ctl;
	}

	fclose(g_log);
	g_log = NULL;

	per = (double)busy / (double)NSAMP;
	printf("模式 %d   每拍 %6.0f us   余量 %+6.0f us   占空比 %5.1f%%   "
	       "丢拍 %4d   %s\n",
	       mode, per / 1e3, (1e3 - per / 1e3), per / 1e4, miss,
	       name[mode]);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/root/rb-naive.log";
	int m;

	printf("== 一个循环干四件事 ==\n");
	printf("   控制 50 us，接收 5 us，决策 8 ms 每 100 ms 一次，"
	       "日志每拍一行 + fsync\n");
	printf("   周期 1000 us，共 3000 拍\n");
	printf("   余量 = 1000 us - 每拍工作量，负数表示这一拍根本做不完\n\n");
	for (m = 0; m <= 4; m++)
		run(m, path);
	return 0;
}
