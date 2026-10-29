/*
 * bench.c —— 解析到底要多少钱
 *
 * 直觉上"四路传感器数据"听起来是很重的活。量一下就知道：解析一帧
 * 激光雷达不到两微秒，而等它扫完要一百毫秒。
 *
 * 传感器的成本不在解析，在等待。
 */
#include <stdio.h>
#include <time.h>

#include "sensor.h"

#define ITER	200000

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile float sink;

int main(void)
{
	static uint8_t buf[16384];
	struct sim s;
	struct enc_reading enc;
	struct imu_reading imu;
	struct lidar_reading lid;
	struct cam_reading cam;
	size_t n_enc, n_imu, n_lid, n_cam;
	uint64_t t0, t1;
	double e_enc, e_imu, e_lid, e_cam, e_chk;
	int k;

	sim_init(&s);
	sim_add_obs(&s, 1.20f, 0.17f, 0.10f);
	s.v = 0.30f;
	for (k = 0; k < 2000; k++)
		sim_step(&s, 0.001f);

	n_enc = sim_enc(&s, buf, 1000000000ull);
	n_imu = sim_imu(&s, buf, 1000000000ull);
	n_lid = sim_lidar(&s, buf, 1000000000ull);
	n_cam = sim_cam(&s, buf, 1000000000ull);

	printf("=== 一帧多少字节 ===\n\n");
	printf("  编码器    %5zu 字节（头 %zu + 载荷 %zu）\n", n_enc,
	       sizeof(struct sensor_hdr), sizeof(struct enc_frame));
	printf("  IMU       %5zu 字节\n", n_imu);
	printf("  激光雷达  %5zu 字节（360 束 × 2）\n", n_lid);
	printf("  摄像头    %5zu 字节（64 × 48）\n\n", n_cam);

	/* 校验：只验头，不碰载荷 */
	{
		sim_enc(&s, buf, 1000000000ull);
		t0 = now_ns();
		for (k = 0; k < ITER; k++)
			sink = sens_check(buf, n_enc) ? 1.0f : 0.0f;
		t1 = now_ns();
		e_chk = (double)(t1 - t0) / ITER;
	}

	sim_enc(&s, buf, 1000000000ull);
	t0 = now_ns();
	for (k = 0; k < ITER; k++) {
		enc_parse(buf, n_enc, &enc);
		sink = enc.ds_m + enc.dth_rad;
	}
	t1 = now_ns();
	e_enc = (double)(t1 - t0) / ITER;

	sim_imu(&s, buf, 1000000000ull);
	t0 = now_ns();
	for (k = 0; k < ITER; k++) {
		imu_parse(buf, n_imu, &imu);
		sink = imu.gz_rad_s;
	}
	t1 = now_ns();
	e_imu = (double)(t1 - t0) / ITER;

	sim_lidar(&s, buf, 1000000000ull);
	t0 = now_ns();
	for (k = 0; k < ITER; k++) {
		lidar_parse(buf, n_lid, &lid);
		sink = lid.nearest_m;
	}
	t1 = now_ns();
	e_lid = (double)(t1 - t0) / ITER;

	sim_cam(&s, buf, 1000000000ull);
	t0 = now_ns();
	for (k = 0; k < ITER; k++) {
		cam_parse(buf, n_cam, &cam);
		sink = cam.bear_rad;
	}
	t1 = now_ns();
	e_cam = (double)(t1 - t0) / ITER;

	printf("=== 解析一帧要多久（%d 次平均）===\n\n", ITER);
	printf("%-10s %-12s %-14s %s\n", "传感器", "一帧", "出数周期", "占周期");
	printf("──────────────────────────────────────────────────────\n");
	printf("%-10s %8.0f ns   %6.0f ms       %.4f%%\n", "编码器", e_enc,
	       1000.0 / ENC_HZ, e_enc / (1e9 / ENC_HZ) * 100.0);
	printf("%-10s %8.0f ns   %6.0f ms       %.4f%%\n", "IMU", e_imu,
	       1000.0 / IMU_HZ, e_imu / (1e9 / IMU_HZ) * 100.0);
	printf("%-10s %8.0f ns   %6.0f ms       %.4f%%\n", "激光雷达", e_lid,
	       1000.0 / LIDAR_HZ, e_lid / (1e9 / LIDAR_HZ) * 100.0);
	printf("%-10s %8.0f ns   %6.1f ms       %.4f%%\n", "摄像头", e_cam,
	       1000.0 / CAM_FPS, e_cam / (1e9 / CAM_FPS) * 100.0);
	printf("\n只验头不碰载荷：%.0f ns\n", e_chk);

	{
		double per_sec = e_enc * ENC_HZ + e_imu * IMU_HZ +
				 e_lid * LIDAR_HZ + e_cam * CAM_FPS;

		printf("\n四路全开，每秒花在解析上的时间：%.1f 微秒（%.3f%% 的一个核）\n",
		       per_sec / 1e3, per_sec / 1e9 * 100.0);
		printf("等一帧激光雷达扫完要 %.0f 毫秒，是解析的 %.0f 倍\n",
		       1000.0 / LIDAR_HZ,
		       (1e9 / LIDAR_HZ) / e_lid);
	}

	printf("\n每秒进程序的数据量：\n");
	printf("  编码器   %d 帧 × %zu B = %.0f B/s\n", ENC_HZ, n_enc,
	       (double)ENC_HZ * (double)n_enc);
	printf("  IMU      %d 帧 × %zu B = %.0f B/s\n", IMU_HZ, n_imu,
	       (double)IMU_HZ * (double)n_imu);
	printf("  激光雷达 %d 帧 × %zu B = %.0f B/s\n", LIDAR_HZ, n_lid,
	       (double)LIDAR_HZ * (double)n_lid);
	printf("  摄像头   %d 帧 × %zu B = %.0f B/s\n", CAM_FPS, n_cam,
	       (double)CAM_FPS * (double)n_cam);
	return 0;
}
