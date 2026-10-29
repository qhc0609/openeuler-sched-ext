/*
 * sim.c —— 四路传感器的"造硬件"层
 *
 * 真机上这一段是驱动：中断、DMA、寄存器。这里换成几何和随机数，
 * 但产出的字节格式和真机一模一样——所以上面的解析代码一行都不用改。
 *
 * 车在哪儿、障碍在哪儿是已知的，这叫真值。仿真器的价值就在于它有真值，
 * 不然你没法说"解析对了没有"。
 */
#include <math.h>
#include <string.h>

#include "sensor.h"

/* 这台"陀螺"的零偏和噪声。0.002 rad/s 是很便宜的那一档。 */
#define IMU_BIAS_RAD_S	0.002f
#define IMU_NOISE_RAD_S	0.030f
#define ENC_NOISE_CNT	0.0f
#define LIDAR_NOISE_MM	2.0f
#define CAM_NOISE_LSB	3.0f

/* xorshift32，够用，且可重复——同一个种子每次跑出同一批数 */
static uint32_t rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static float urand(void)
{
	return (float)(rnd() >> 8) * (1.0f / 16777216.0f);
}

/* 四个均匀数相加，方差为 1 的近似高斯 */
static float grand(void)
{
	return (urand() + urand() + urand() + urand() - 2.0f) * 1.7320508f;
}

static int16_t clamp16(long v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

/* 换个种子，就能让两组实验吃到一模一样的噪声——对照实验的前提 */
void sim_seed(uint32_t seed)
{
	rng = seed ? seed : 1u;
}

void sim_init(struct sim *s)
{
	memset(s, 0, sizeof(*s));
	s->x = 0.0f;
	s->y = 0.0f;
	s->th = 0.0f;
}

void sim_add_obs(struct sim *s, float x, float y, float r)
{
	if (s->nobs < SIM_MAX_OBS) {
		s->obs[s->nobs].x = x;
		s->obs[s->nobs].y = y;
		s->obs[s->nobs].r = r;
		s->nobs++;
	}
}

void sim_step(struct sim *s, float dt)
{
	float vl = s->v - s->w * TRACK_M * 0.5f;
	float vr = s->v + s->w * TRACK_M * 0.5f;

	s->x += s->v * cosf(s->th) * dt;
	s->y += s->v * sinf(s->th) * dt;
	s->th += s->w * dt;
	if (s->th > 3.14159265f)
		s->th -= 6.2831853f;
	if (s->th < -3.14159265f)
		s->th += 6.2831853f;

	s->acc_l += (double)(vl * dt) / (double)ENC_M_PER_CNT;
	s->acc_r += (double)(vr * dt) / (double)ENC_M_PER_CNT;
	s->t += dt;
}

/*
 * 编码器：只报"这一帧两个轮子各转了几个计数"。
 * 注意它是增量，不是位置——一开机就是 0，和世界没有任何关系。
 */
size_t sim_enc(struct sim *s, uint8_t *buf, uint64_t t_ns)
{
	struct enc_frame f;
	int32_t cl = (int32_t)lround(s->acc_l);
	int32_t cr = (int32_t)lround(s->acc_r);

	f.dl = cl - s->last_l;
	f.dr = cr - s->last_r;
	s->last_l = cl;
	s->last_r = cr;

	return sens_pack(buf, SENS_ENC, t_ns, s->seq_enc++, &f, sizeof(f));
}

/*
 * IMU：车体角速度 + 零偏 + 噪声，量化成 int16。
 * 真机上你拿到的就是这个 int16，零偏在数据里，不在手册里。
 */
size_t sim_imu(struct sim *s, uint8_t *buf, uint64_t t_ns)
{
	struct imu_frame f;
	float gz = s->w + IMU_BIAS_RAD_S + IMU_NOISE_RAD_S * grand();

	f.gx = clamp16(lroundf((IMU_NOISE_RAD_S * grand()) / GYRO_RAD_S_PER_LSB));
	f.gy = clamp16(lroundf((IMU_NOISE_RAD_S * grand()) / GYRO_RAD_S_PER_LSB));
	f.gz = clamp16(lroundf(gz / GYRO_RAD_S_PER_LSB));

	f.ax = clamp16(lroundf((IMU_NOISE_RAD_S * grand()) / ACC_G_PER_LSB));
	f.ay = clamp16(lroundf((IMU_NOISE_RAD_S * grand()) / ACC_G_PER_LSB));
	/* 静止时重力压在 z 上，约 1 g */
	f.az = clamp16(lroundf((1.0f + 0.02f * grand()) / ACC_G_PER_LSB));

	return sens_pack(buf, SENS_IMU, t_ns, s->seq_imu++, &f, sizeof(f));
}

/* 射线和圆求交，返回最近距离；没打中返回 0 */
static float cast_ray(const struct sim *s, float px, float py, float ang)
{
	float dx = cosf(ang), dy = sinf(ang);
	float best = 0.0f;

	for (int i = 0; i < s->nobs; i++) {
		float ox = px - s->obs[i].x;
		float oy = py - s->obs[i].y;
		float b = ox * dx + oy * dy;
		float c = ox * ox + oy * oy - s->obs[i].r * s->obs[i].r;
		float disc = b * b - c;

		if (disc < 0.0f)
			continue;
		disc = sqrtf(disc);
		if (-b - disc > 0.02f && (best <= 0.0f || -b - disc < best))
			best = -b - disc;
	}
	return best;
}

/*
 * 激光雷达：360 束，10 Hz，扫一圈 100 ms。
 *
 * 关键在这一句：**每一束不是同一时刻打的**。第 0 束在 100 ms 前，
 * 第 359 束在刚才。帧时间戳只能标一个数，所以标的是"扫完那一刻"。
 */
size_t sim_lidar(struct sim *s, uint8_t *buf, uint64_t t_ns)
{
	static struct lidar_frame f;
	float period = 1.0f / (float)LIDAR_HZ;
	float per = period / (float)LIDAR_BEAMS;

	for (int i = 0; i < LIDAR_BEAMS; i++) {
		float back = period - (float)i * per;	/* 这一束是多久以前打的 */
		float th = s->th - s->w * back;
		float px = s->x - s->v * cosf(s->th) * back;
		float py = s->y - s->v * sinf(s->th) * back;
		float ang = th + (float)i * LIDAR_DEG_PER_BEAM * DEG2RAD_F;
		float d = cast_ray(s, px, py, ang);
		long mm;

		if (d <= 0.0f) {
			f.mm[i] = 0;		/* 没打中 */
			continue;
		}
		mm = lroundf(d * 1000.0f + LIDAR_NOISE_MM * grand());
		if (mm < 1)
			mm = 1;
		if (mm >= LIDAR_RANGE_MAX_MM)
			mm = 0;			/* 超量程也当没打中 */
		f.mm[i] = (uint16_t)mm;
	}

	return sens_pack(buf, SENS_LIDAR, t_ns, s->seq_lidar++, &f, sizeof(f));
}

/*
 * 摄像头：64×48 灰度，60 度水平视场。
 * 每一列也是一条射线，只是它带回来的是"多亮"，不是"多远"。
 * 这里让亮度随距离衰减，于是近的障碍在图上就是一块亮斑。
 */
size_t sim_cam(struct sim *s, uint8_t *buf, uint64_t t_ns)
{
	static struct cam_frame f;

	memset(&f, 0, sizeof(f));

	for (int u = 0; u < CAM_W; u++) {
		float bear = -((float)u - 0.5f * (float)(CAM_W - 1)) *
			     CAM_DEG_PER_PX * DEG2RAD_F;
		float d = cast_ray(s, s->x, s->y, s->th + bear);
		int v;

		if (d <= 0.0f)
			continue;
		v = (int)lroundf(255.0f * (1.0f - d / 12.0f) +
				 CAM_NOISE_LSB * grand());
		if (v < 0)
			v = 0;
		if (v > 255)
			v = 255;
		for (int y = 0; y < CAM_H; y++)
			f.px[y * CAM_W + u] = (uint8_t)v;
	}

	return sens_pack(buf, SENS_CAM, t_ns, s->seq_cam++, &f, sizeof(f));
}
