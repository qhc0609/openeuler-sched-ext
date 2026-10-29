/*
 * sensor.h —— 四路传感器的帧格式
 *
 * 摄像头、激光雷达、IMU、编码器，硬件上毫无关系，可送到程序里的东西
 * 长得一模一样：一段字节，前面一个头，头里有 id、长度、时间戳、序号。
 *
 * 这一篇要讲的就是"把这段字节变成带单位的物理量"这件事。
 * 换算常数全部写在下面，一个都不许散落在代码里——单位错了，后面
 * 算得再对也是错的。
 */
#ifndef SENSOR_H
#define SENSOR_H

#include <stddef.h>
#include <stdint.h>

#define SENS_MAGIC	0x53454E53u	/* "SENS" */

enum sensor_id {
	SENS_ENC	= 1,
	SENS_IMU	= 2,
	SENS_LIDAR	= 3,
	SENS_CAM	= 4,
};

/* 各路传感器的出数节奏，四个都不一样 */
#define ENC_HZ		50
#define IMU_HZ		50
#define LIDAR_BEAMS	360
#define LIDAR_HZ	10
#define CAM_W		64
#define CAM_H		48
#define CAM_FPS		30

struct sensor_hdr {
	uint32_t magic;
	uint16_t id;
	uint16_t len;		/* 后面跟多少字节载荷 */
	uint64_t t_ns;		/* 采样时刻，单调钟 */
	uint32_t seq;
	uint32_t sum;		/* 载荷累加和，够用了 */
};

struct enc_frame {
	int32_t dl;		/* 本帧左轮增量，单位：计数 */
	int32_t dr;
};

struct imu_frame {
	int16_t ax, ay, az;	/* 加速度计原始 LSB */
	int16_t gx, gy, gz;	/* 陀螺原始 LSB */
};

struct lidar_frame {
	uint16_t mm[LIDAR_BEAMS];	/* 0 表示这一束没打中东西 */
};

struct cam_frame {
	uint8_t px[CAM_W * CAM_H];	/* 灰度，一行一行存 */
};

/*
 * 换算常数。真机的值在数据手册里，这里取一台小车的典型值。
 * 这四个宏就是"字节 → 物理量"的全部秘密。
 */
#define ENC_CPR		4096		/* 1024 线，四倍频 */
#define WHEEL_R_M	0.065f
#define TRACK_M		0.32f		/* 轮距 */
#define ENC_M_PER_CNT	(2.0f * 3.14159265f * WHEEL_R_M / (float)ENC_CPR)

/*
 * 轮距不是宏，是变量——因为它要标定。
 * 出厂值 0.32 米，装完车量出来可能是 0.33，这一个数错了，转角就跟着错。
 */
extern float enc_track_m;

#define DEG2RAD_F	0.017453292519943295f
#define GYRO_FS_DPS	2000.0f		/* 陀螺量程 ±2000 °/s */
#define ACC_FS_G	4.0f		/* 加速度计量程 ±4 g */
#define GYRO_RAD_S_PER_LSB	(GYRO_FS_DPS * DEG2RAD_F / 32768.0f)
#define ACC_G_PER_LSB		(ACC_FS_G / 32768.0f)

#define LIDAR_RANGE_MAX_MM	12000
#define LIDAR_DEG_PER_BEAM	(360.0f / (float)LIDAR_BEAMS)
#define CAM_DEG_PER_PX		(60.0f / (float)(CAM_W - 1))

/*
 * 解析出来的东西。左边是字节，右边是物理量——这一篇全部内容就是
 * 从左边走到右边，顺便记住每个数是什么时候的。
 */
struct enc_reading {
	uint64_t t_ns;
	uint32_t seq;
	int32_t cnt_l, cnt_r;	/* 字节里的原始值：本帧增量计数 */
	float dl_m, dr_m;	/* 两个轮子各走了多少米 */
	float ds_m;		/* 车体前进多少米 */
	float dth_rad;		/* 车体转了多少弧度 */
};

struct imu_reading {
	uint64_t t_ns;
	uint32_t seq;
	int16_t gz_lsb;		/* 字节里的原始值 */
	float gz_rad_s;
	float ax_g, ay_g, az_g;
};

struct lidar_reading {
	uint64_t t_ns;
	uint32_t seq;
	int n;			/* 打中东西的束数 */
	float range_m[LIDAR_BEAMS];
	float bear_rad[LIDAR_BEAMS];	/* 左正右负，0 是车头正前 */
	float nearest_m;
	float nearest_bear_rad;
	int nearest_i;
};

struct cam_reading {
	uint64_t t_ns;
	uint32_t seq;
	int n_px;		/* 超过阈值的像素数 */
	float u;		/* 亮块重心，像素列 */
	float bear_rad;		/* 换算成方位角 */
};

/* 打包 / 校验 / 解析 */
size_t sens_pack(uint8_t *buf, uint16_t id, uint64_t t_ns, uint32_t seq,
		 const void *payload, uint16_t len);
const struct sensor_hdr *sens_check(const uint8_t *buf, size_t n);

int enc_parse(const uint8_t *buf, size_t n, struct enc_reading *r);
int imu_parse(const uint8_t *buf, size_t n, struct imu_reading *r);
int lidar_parse(const uint8_t *buf, size_t n, struct lidar_reading *r);
int cam_parse(const uint8_t *buf, size_t n, struct cam_reading *r);

/*
 * 仿真器。真机上这一层是驱动，这里是"造硬件"：
 * 车在哪儿、障碍在哪儿是已知的，四路传感器的字节由它推出来。
 */
#define SIM_MAX_OBS	8

struct sim {
	/* 真值，只有仿真器自己知道 */
	float x, y, th;
	float v, w;
	double t;

	/* 编码器累计计数，用来算帧间增量 */
	double acc_l, acc_r;
	int32_t last_l, last_r;

	/* 每一路自己数自己的帧号，不共用一个 */
	uint32_t seq_enc, seq_imu, seq_lidar, seq_cam;

	struct {
		float x, y, r;
	} obs[SIM_MAX_OBS];
	int nobs;
};

void sim_init(struct sim *s);
void sim_seed(uint32_t seed);
void sim_add_obs(struct sim *s, float x, float y, float r);
void sim_step(struct sim *s, float dt);

size_t sim_enc(struct sim *s, uint8_t *buf, uint64_t t_ns);
size_t sim_imu(struct sim *s, uint8_t *buf, uint64_t t_ns);
size_t sim_lidar(struct sim *s, uint8_t *buf, uint64_t t_ns);
size_t sim_cam(struct sim *s, uint8_t *buf, uint64_t t_ns);

#endif
