/*
 * delay.c —— 一帧激光雷达里，最老的那一束有多老
 *
 * 360 束不是同时打的，是一束一束扫出来的。10 Hz 扫一圈，所以第 0 束
 * 打完到第 359 束打完，中间隔了 100 毫秒。帧上只有一个时间戳，标的是
 * "扫完那一刻"——于是前面 359 束的数据，都比你手上的时刻要老。
 *
 * 车在转的时候，这个"老"就变成了角度误差。
 */
#include <math.h>
#include <stdio.h>

#include "sensor.h"

#define W_SPIN		1.0f		/* 原地转的角速度 rad/s */
#define T_END		0.10		/* 正好扫完一圈 */
#define OBST_X		1.13f
#define OBST_Y		0.41f
#define OBST_R		0.10f

int main(void)
{
	static uint8_t buf[16384];
	struct sim s;
	struct lidar_reading lid;
	float period = 1.0f / (float)LIDAR_HZ;
	float per = period / (float)LIDAR_BEAMS;
	float bear_true, bear_naive = 0.0f, bear_fix = 0.0f;
	float sum_naive = 0.0f, sum_fix = 0.0f, mean_range = 0.0f;
	int nhit = 0, first = -1, last = -1;

	sim_init(&s);
	sim_add_obs(&s, OBST_X, OBST_Y, OBST_R);
	s.w = W_SPIN;

	while (s.t < T_END - 1e-9)
		sim_step(&s, 0.0005f);

	sim_lidar(&s, buf, (uint64_t)(s.t * 1e9));
	lidar_parse(buf, sizeof(buf), &lid);

	/* 真值：t=T_END 这一刻，障碍在车体坐标系的哪个方向 */
	bear_true = atan2f(OBST_Y - s.y, OBST_X - s.x) - s.th;

	printf("车在 (0.00, 0.00) 原地以 %.1f rad/s 逆时针转\n", (double)W_SPIN);
	printf("障碍在 (%.2f, %.2f)，半径 %.2f 米\n", (double)OBST_X,
	       (double)OBST_Y, (double)OBST_R);
	printf("一帧扫 %.0f ms，360 束，每束间隔 %.3f ms\n\n",
	       (double)(period * 1e3), (double)(per * 1e3));

	printf("帧时间戳 t=%.3f s（扫完那一刻），此刻车头已转过 %.2f 度\n",
	       (double)s.t, (double)(s.th / DEG2RAD_F));
	printf("这一帧里最老的一束打了 %.1f ms，最新的一束打了 %.1f ms\n\n",
	       (double)(period * 1e3), (double)(per * 1e3));

	/*
	 * 方位取"命中那一段弧的中心"。
	 * 一束只有 1 度的量化，一段弧的中心能到零点几度——和摄像头求亮块
	 * 重心是一回事，都是用"一块"换"一个点"的精度。
	 */
	for (int i = 0; i < LIDAR_BEAMS; i++) {
		float back, bear;

		if (lid.range_m[i] <= 0.0f)
			continue;

		back = period - (float)i * per;
		bear = lid.bear_rad[i] - W_SPIN * back;	/* 减掉这段时间转过的角 */

		sum_naive += lid.bear_rad[i];
		sum_fix += bear;
		mean_range += lid.range_m[i];
		if (first < 0)
			first = i;
		last = i;
		nhit++;
	}
	if (nhit > 0) {
		bear_naive = sum_naive / (float)nhit;
		bear_fix = sum_fix / (float)nhit;
		mean_range /= (float)nhit;
	}

	printf("命中 %d 束，第 %d 束到第 %d 束，平均距离 %.3f 米\n\n", nhit,
	       first, last, (double)mean_range);

	printf("%-24s %-10s %-10s %s\n", "", "方位", "差", "1 米处横向偏");
	printf("──────────────────────────────────────────────────────\n");
	printf("%-24s %+8.2f°  %8s   %s\n", "真值（t=0.100 s）",
	       (double)(bear_true / DEG2RAD_F), "—", "—");
	printf("%-24s %+8.2f°  %+7.2f°   %+.1f cm\n", "直接读整帧",
	       (double)(bear_naive / DEG2RAD_F),
	       (double)((bear_naive - bear_true) / DEG2RAD_F),
	       (double)((bear_naive - bear_true) * mean_range * 100.0f));
	printf("%-24s %+8.2f°  %+7.2f°   %+.1f cm\n", "按每束自己的时刻校正",
	       (double)(bear_fix / DEG2RAD_F),
	       (double)((bear_fix - bear_true) / DEG2RAD_F),
	       (double)((bear_fix - bear_true) * mean_range * 100.0f));
	return 0;
}
