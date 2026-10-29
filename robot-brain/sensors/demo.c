/*
 * demo.c —— 读一帧四路数据，把字节变成物理量
 *
 * 这是文章 1.4 的主程序。场景：一台差速底盘以 0.30 m/s 直行，
 * 左前方 1.2 米处有一根半径 10 厘米的柱子。
 *
 * 程序做三件事：
 *   1. 按各自的节奏产出四路字节（仿真器代替驱动）
 *   2. 用同一个入口解析成四个结构体
 *   3. 把四个数摆在一起，和真值对一遍
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensor.h"

#define T_END		0.95
#define DT		0.001
#define RUN_SPEED	0.30f

struct truth {
	double t;
	float x, y, th;
};

static struct truth hist[256];
static int nhist;

static void push_truth(const struct sim *s, double t)
{
	if (nhist < (int)(sizeof(hist) / sizeof(hist[0]))) {
		hist[nhist].t = t;
		hist[nhist].x = s->x;
		hist[nhist].y = s->y;
		hist[nhist].th = s->th;
		nhist++;
	}
}

static const struct truth *at(double t)
{
	const struct truth *best = NULL;

	for (int i = 0; i < nhist; i++)
		if (hist[i].t <= t + 1e-6)
			best = &hist[i];
	return best;
}

/* 从某个位姿看 0 号障碍，方位角（左正）和距离 */
static void look(const struct truth *p, float ox, float oy, float *bear, float *dist)
{
	float dx = ox - p->x, dy = oy - p->y;

	*dist = sqrtf(dx * dx + dy * dy);
	*bear = atan2f(dy, dx) - p->th;
	while (*bear > 3.14159265f)
		*bear -= 6.2831853f;
	while (*bear < -3.14159265f)
		*bear += 6.2831853f;
}

int main(void)
{
	static uint8_t buf[16384];
	struct sim s;
	struct enc_reading enc;
	struct imu_reading imu;
	struct lidar_reading lid;
	struct cam_reading cam;
	double t_enc = 0, t_lidar = 0, t_cam = 0;
	float gz_ring[40];
	double gz_sum = 0.0;
	int gz_n = 0;
	int step = 0;

	sim_init(&s);
	sim_add_obs(&s, 1.20f, 0.17f, 0.10f);
	s.v = RUN_SPEED;
	s.w = 0.0f;

	printf("车从 (0.00, 0.00) 朝 0 度出发，%.2f m/s 直行 %.2f 秒\n",
	       (double)RUN_SPEED, (double)T_END);
	printf("左前方 (1.20, 0.17) 有根半径 0.10 米的柱子\n\n");

	while (s.t < T_END - 1e-9) {
		uint64_t t_ns;

		sim_step(&s, DT);
		step++;
		t_ns = (uint64_t)(s.t * 1e9);

		/* 50 Hz：编码器 + IMU，两路同频 */
		if (step % (1000 / ENC_HZ) == 0) {
			sim_enc(&s, buf, t_ns);
			enc_parse(buf, sizeof(buf), &enc);
			sim_imu(&s, buf, t_ns);
			imu_parse(buf, sizeof(buf), &imu);
			/* 留最近 40 帧，够算一个一秒的平均 */
			{
				int slot = gz_n % 40;

				if (gz_n < 40)
					gz_sum += imu.gz_rad_s;
				else
					gz_sum += imu.gz_rad_s - gz_ring[slot];
				gz_ring[slot] = imu.gz_rad_s;
				gz_n++;
			}
			t_enc = s.t;
			push_truth(&s, s.t);
		}

		/* 10 Hz：激光雷达，扫完一圈才算一帧 */
		if (step % (1000 / LIDAR_HZ) == 0) {
			sim_lidar(&s, buf, t_ns);
			lidar_parse(buf, sizeof(buf), &lid);
			t_lidar = s.t;
			push_truth(&s, s.t);
		}

		/* 30 Hz：摄像头 */
		if (step % 33 == 0) {
			sim_cam(&s, buf, t_ns);
			cam_parse(buf, sizeof(buf), &cam);
			t_cam = s.t;
			push_truth(&s, s.t);
		}
	}

	printf("=== 此刻手上有的四帧 ===\n");
	printf("编码器    seq %-3u t=%.3f s  年龄 %5.1f ms   左 %+d 右 %+d 计数\n",
	       enc.seq, t_enc, (T_END - t_enc) * 1e3, enc.cnt_l, enc.cnt_r);
	printf("         → 前进 %.2f mm，转角 %.4f rad\n",
	       (double)(enc.ds_m * 1000.0f), (double)enc.dth_rad);
	printf("IMU       seq %-3u t=%.3f s  年龄 %5.1f ms   gz %+d LSB\n",
	       imu.seq, t_enc, (T_END - t_enc) * 1e3, imu.gz_lsb);
	printf("         → 角速度 %+.4f rad/s\n", (double)imu.gz_rad_s);
	printf("激光雷达  seq %-3u t=%.3f s  年龄 %5.1f ms   %d 束命中\n",
	       lid.seq, t_lidar, (T_END - t_lidar) * 1e3, lid.n);
	printf("         这 360 束打在 %.3f–%.3f s 之间，跨度 100.0 ms\n",
	       t_lidar - 0.1, t_lidar);
	printf("         → 最近 %.3f m，方位 %+.2f 度\n",
	       (double)lid.nearest_m, (double)(lid.nearest_bear_rad / DEG2RAD_F));
	printf("摄像头    seq %-3u t=%.3f s  年龄 %5.1f ms   亮块 %d 像素\n",
	       cam.seq, t_cam, (T_END - t_cam) * 1e3, cam.n_px);
	printf("         → 重心 u=%.2f，方位 %+.2f 度\n\n",
	       (double)cam.u, (double)(cam.bear_rad / DEG2RAD_F));

	printf("=== 和真值比 ===\n\n");
	printf("%-10s %-10s %-12s %-12s %s\n",
	       "来源", "帧时刻", "测到方位", "该时刻真值", "差");
	{
		const struct truth *p;
		float tb, td;

		p = at(t_lidar);
		look(p, 1.20f, 0.17f, &tb, &td);
		printf("%-10s %.3f s   %+10.2f°  %+10.2f°  %+.2f°\n", "激光雷达",
		       t_lidar, (double)(lid.nearest_bear_rad / DEG2RAD_F),
		       (double)(tb / DEG2RAD_F),
		       (double)((lid.nearest_bear_rad - tb) / DEG2RAD_F));

		p = at(t_cam);
		look(p, 1.20f, 0.17f, &tb, &td);
		printf("%-10s %.3f s   %+10.2f°  %+10.2f°  %+.2f°\n", "摄像头",
		       t_cam, (double)(cam.bear_rad / DEG2RAD_F),
		       (double)(tb / DEG2RAD_F),
		       (double)((cam.bear_rad - tb) / DEG2RAD_F));
	}

	printf("\n=== 编码器和 IMU 这一帧说了什么 ===\n\n");
	printf("编码器  转角 %+.4f rad  ← 左右 %+d / %+d 个计数，相等就是没转\n",
	       (double)enc.dth_rad, enc.cnt_l, enc.cnt_r);
	printf("        前进 %.2f mm（%d 个计数 × %.4f mm）\n",
	       (double)(enc.ds_m * 1000.0f), enc.cnt_l,
	       (double)(ENC_M_PER_CNT * 1000.0f));
	printf("IMU     单帧 %+.4f rad/s（%+d LSB），车其实没转\n",
	       (double)imu.gz_rad_s, imu.gz_lsb);
	if (gz_n >= 40)
		printf("        最近 40 帧平均 %+.4f rad/s ← 噪声从 0.030 压到 0.005\n",
		       gz_sum / 40.0);
	printf("\n换算：1 个计数 = %.4f mm，1 个 LSB = %.6f rad/s\n",
	       (double)(ENC_M_PER_CNT * 1000.0f), (double)GYRO_RAD_S_PER_LSB);
	return 0;
}
