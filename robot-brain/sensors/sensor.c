/*
 * sensor.c —— 打包、校验、解析
 *
 * 四个 parse 函数各十几行，结构完全一样：先验头，再取载荷，最后乘一个
 * 常数。这一篇想让人记住的不是这十几行，而是"先验头"这一步不能省。
 */
#include <math.h>
#include <string.h>

#include "sensor.h"

float enc_track_m = TRACK_M;

/* 载荷累加和。不做 CRC 的数学保证，只求便宜，能挡住撕开的半帧就行。 */
static uint32_t sum32(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t s = 0;

	for (size_t i = 0; i < n; i++)
		s = s * 31u + b[i];
	return s;
}

size_t sens_pack(uint8_t *buf, uint16_t id, uint64_t t_ns, uint32_t seq,
		 const void *payload, uint16_t len)
{
	struct sensor_hdr h;

	h.magic = SENS_MAGIC;
	h.id = id;
	h.len = len;
	h.t_ns = t_ns;
	h.seq = seq;
	h.sum = sum32(payload, len);

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), payload, len);
	return sizeof(h) + (size_t)len;
}

/*
 * 校验。返回头指针，或者 NULL。
 * 三件事：魔数对不对、长度对不对、累加和对不对。少一样都可能把
 * 一段别的什么字节当成传感器数据去乘常数。
 */
const struct sensor_hdr *sens_check(const uint8_t *buf, size_t n)
{
	const struct sensor_hdr *h = (const void *)buf;

	if (!buf || n < sizeof(*h))
		return NULL;
	if (h->magic != SENS_MAGIC)
		return NULL;
	if (h->len > n - sizeof(*h))
		return NULL;
	if (sum32(buf + sizeof(*h), h->len) != h->sum)
		return NULL;
	return h;
}

int enc_parse(const uint8_t *buf, size_t n, struct enc_reading *r)
{
	const struct sensor_hdr *h = sens_check(buf, n);
	const struct enc_frame *f;

	if (!h || h->id != SENS_ENC || h->len != sizeof(*f))
		return -1;
	f = (const void *)(buf + sizeof(*h));

	r->t_ns = h->t_ns;
	r->seq = h->seq;
	r->cnt_l = f->dl;
	r->cnt_r = f->dr;
	r->dl_m = (float)f->dl * ENC_M_PER_CNT;
	r->dr_m = (float)f->dr * ENC_M_PER_CNT;
	r->ds_m = 0.5f * (r->dl_m + r->dr_m);
	r->dth_rad = (r->dr_m - r->dl_m) / enc_track_m;
	return 0;
}

int imu_parse(const uint8_t *buf, size_t n, struct imu_reading *r)
{
	const struct sensor_hdr *h = sens_check(buf, n);
	const struct imu_frame *f;

	if (!h || h->id != SENS_IMU || h->len != sizeof(*f))
		return -1;
	f = (const void *)(buf + sizeof(*h));

	r->t_ns = h->t_ns;
	r->seq = h->seq;
	r->gz_lsb = f->gz;
	r->gz_rad_s = (float)f->gz * GYRO_RAD_S_PER_LSB;
	r->ax_g = (float)f->ax * ACC_G_PER_LSB;
	r->ay_g = (float)f->ay * ACC_G_PER_LSB;
	r->az_g = (float)f->az * ACC_G_PER_LSB;
	return 0;
}

int lidar_parse(const uint8_t *buf, size_t n, struct lidar_reading *r)
{
	const struct sensor_hdr *h = sens_check(buf, n);
	const struct lidar_frame *f;

	if (!h || h->id != SENS_LIDAR || h->len != sizeof(*f))
		return -1;
	f = (const void *)(buf + sizeof(*h));

	r->t_ns = h->t_ns;
	r->seq = h->seq;
	r->n = 0;
	r->nearest_m = 1e9f;
	r->nearest_bear_rad = 0.0f;
	r->nearest_i = -1;

	for (int i = 0; i < LIDAR_BEAMS; i++) {
		/* 0 是"这一束没回来"，不是"贴着 0 毫米" */
		if (f->mm[i] == 0) {
			r->range_m[i] = 0.0f;
			r->bear_rad[i] = 0.0f;
			continue;
		}
		r->range_m[i] = (float)f->mm[i] * 0.001f;
		r->bear_rad[i] = (float)i * LIDAR_DEG_PER_BEAM * DEG2RAD_F;
		r->n++;

		if (r->range_m[i] < r->nearest_m) {
			r->nearest_m = r->range_m[i];
			r->nearest_bear_rad = r->bear_rad[i];
			r->nearest_i = i;
		}
	}
	return 0;
}

/*
 * 摄像头：灰度图 → 亮块重心 → 方位角。
 * 阈值以下当背景，以上的按亮度加权求列重心。这一步和激光雷达求
 * 最近束是一回事——都是把"一堆数"压成"一个方向"。
 */
int cam_parse(const uint8_t *buf, size_t n, struct cam_reading *r)
{
	const struct sensor_hdr *h = sens_check(buf, n);
	const struct cam_frame *f;
	float wsum = 0.0f, usum = 0.0f;
	int cnt = 0;

	if (!h || h->id != SENS_CAM || h->len != sizeof(*f))
		return -1;
	f = (const void *)(buf + sizeof(*h));

	for (int y = 0; y < CAM_H; y++) {
		for (int x = 0; x < CAM_W; x++) {
			int v = f->px[y * CAM_W + x];

			if (v < 96)
				continue;
			usum += (float)x * (float)(v - 96);
			wsum += (float)(v - 96);
			cnt++;
		}
	}

	r->t_ns = h->t_ns;
	r->seq = h->seq;
	r->n_px = cnt;
	if (wsum <= 0.0f) {
		r->u = -1.0f;
		r->bear_rad = 0.0f;
		return 0;
	}
	r->u = usum / wsum;
	/* 像素往右走，方位角往负走——图像坐标和车体坐标是反的 */
	r->bear_rad = -(r->u - 0.5f * (float)(CAM_W - 1)) * CAM_DEG_PER_PX * DEG2RAD_F;
	return 0;
}
