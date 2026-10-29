/*
 * drift.c —— 三路"角度"各走各的
 *
 * 同一个转角，三个来源给三个答案：
 *   真值    仿真器自己知道，真机上你永远没有这个数
 *   编码器  两个轮子的计数差除以轮距，靠几何，不靠时间
 *   IMU     角速度对时间积分，靠时间，不靠几何
 *
 * 这一段就是把它们摆在一起，看谁在什么地方错。
 */
#include <math.h>
#include <stdio.h>

#include "sensor.h"

#define DT		0.001
#define FRAME		0.02		/* 50 Hz 出一帧 */
#define STILL_T		20.0		/* 停着不动多久 */
#define SPIN_W		1.0f		/* 原地转的角速度 rad/s */
#define SPIN_ANGLE	1.5707963f	/* 转 90 度 */

struct acc {
	double th_true;
	double th_enc, th_imu, th_imu_cal;
	int frames;
};

/* 跑一遍，返回三个角度 */
static void run(float spin_time, struct acc *a)
{
	static uint8_t buf[16384];
	struct sim s;
	struct enc_reading enc;
	struct imu_reading imu;
	int step = 0;

	sim_init(&s);
	sim_add_obs(&s, 3.0f, 0.0f, 0.10f);	/* 这个实验用不着障碍，摆远点 */

	a->th_enc = 0.0;
	a->th_imu = 0.0;
	a->th_imu_cal = 0.0;
	a->frames = 0;

	while (s.t < spin_time + STILL_T - 1e-9) {
		s.w = (s.t < spin_time) ? SPIN_W : 0.0f;
		sim_step(&s, DT);
		step++;

		if (step % (int)(FRAME / DT) != 0)
			continue;

		sim_enc(&s, buf, (uint64_t)(s.t * 1e9));
		enc_parse(buf, sizeof(buf), &enc);
		sim_imu(&s, buf, (uint64_t)(s.t * 1e9));
		imu_parse(buf, sizeof(buf), &imu);

		a->th_enc += enc.dth_rad;
		a->th_imu += (double)imu.gz_rad_s * FRAME;
		/* 扣掉出厂标定的零偏，剩下的才是噪声 */
		a->th_imu_cal += (double)(imu.gz_rad_s - 0.002f) * FRAME;
		a->frames++;
	}

	a->th_true = s.th;
}

int main(void)
{
	struct acc still, spin, spin_bad;
	float spin_time = SPIN_ANGLE / SPIN_W;

	printf("轮径 %.1f mm，%d 计数/转 → 1 个计数 = %.4f mm\n",
	       (double)(WHEEL_R_M * 1000.0f), ENC_CPR,
	       (double)(ENC_M_PER_CNT * 1000.0f));
	printf("陀螺 ±%.0f °/s → 1 个 LSB = %.6f rad/s\n",
	       (double)GYRO_FS_DPS, (double)GYRO_RAD_S_PER_LSB);
	printf("轮距 %.3f m，静止 %.0f 秒\n\n", (double)TRACK_M, STILL_T);

	enc_track_m = TRACK_M;
	sim_seed(20261029u);
	run(0.0f, &still);
	sim_seed(20261029u);
	run(spin_time, &spin);
	enc_track_m = 0.33f;		/* 标定错了 3% */
	sim_seed(20261029u);
	run(spin_time, &spin_bad);
	enc_track_m = TRACK_M;

	printf("%-20s %-10s %-10s %-10s %s\n",
	       "场景", "真值", "编码器", "IMU", "IMU 扣零偏");
	printf("──────────────────────────────────────────────────────────────\n");

	printf("%-20s %-10.5f %-10.5f %-10.5f %.5f\n",
	       "静止 20 秒", still.th_true, still.th_enc, still.th_imu,
	       still.th_imu_cal);
	printf("%-20s %-10.5f %-10.5f %-10.5f %.5f\n",
	       "原地转 90 度", spin.th_true, spin.th_enc, spin.th_imu,
	       spin.th_imu_cal);
	printf("%-20s %-10.5f %-10.5f %-10.5f %.5f\n",
	       "同上，轮距 0.33", spin_bad.th_true, spin_bad.th_enc,
	       spin_bad.th_imu, spin_bad.th_imu_cal);

	printf("\n误差，单位：度\n\n");
	printf("%-20s %-10s %-10s %s\n", "场景", "编码器", "IMU", "IMU 扣零偏");
	printf("──────────────────────────────────────────────────────\n");
	printf("%-20s %+9.2f %+9.2f %+9.2f\n", "静止 20 秒",
	       (double)((still.th_enc - still.th_true) / DEG2RAD_F),
	       (double)((still.th_imu - still.th_true) / DEG2RAD_F),
	       (double)((still.th_imu_cal - still.th_true) / DEG2RAD_F));
	printf("%-20s %+9.2f %+9.2f %+9.2f\n", "原地转 90 度",
	       (double)((spin.th_enc - spin.th_true) / DEG2RAD_F),
	       (double)((spin.th_imu - spin.th_true) / DEG2RAD_F),
	       (double)((spin.th_imu_cal - spin.th_true) / DEG2RAD_F));
	printf("%-20s %+9.2f %+9.2f %+9.2f\n", "同上，轮距 0.33",
	       (double)((spin_bad.th_enc - spin_bad.th_true) / DEG2RAD_F),
	       (double)((spin_bad.th_imu - spin_bad.th_true) / DEG2RAD_F),
	       (double)((spin_bad.th_imu_cal - spin_bad.th_true) / DEG2RAD_F));
	return 0;
}
