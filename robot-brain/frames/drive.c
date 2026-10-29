/*
 * drive.c —— 从"去那个点"到"两个轮子各转多快"
 *
 * 一条链路四步：
 *
 *   世界系目标 (tx, ty)
 *        ↓ 减掉车头朝向、折进 ±π        ← 第三节
 *   车体系方位 err
 *        ↓ 比例控制 + 限幅
 *   车体系速度 (v, w)
 *        ↓ 打包，带时间戳和有效期        ← 第五、六节
 *   一条 motion_cmd
 *
 * 每一步都能搞错，这一篇把四个最常见的错各跑一遍，看差多少。
 */
#include <math.h>
#include <stdio.h>

#include "cmd.h"
#include "pose.h"

#define DT		0.001f
#define VMAX		0.30f
#define WMAX		1.50f
#define GOAL_X		1.00f
#define GOAL_Y		0.55f
#define TOL		0.012f
#define START_TH	(120.0f * DEG2RAD)

static float clampf_(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/* ---------- 控制器：位姿 + 目标 → (v, w) ---------- */

static void nav_ok(const struct pose *p, float tx, float ty, float *v, float *w)
{
	float err = bearing_to(p, tx, ty);	/* 减了 θ，也折进 ±π */
	float d = hypotf(tx - p->x, ty - p->y);

	if (d < TOL) {
		*v = 0.0f;
		*w = 0.0f;
		return;
	}
	*v = fminf(VMAX, 1.2f * d);
	*w = clampf_(4.0f * err, -WMAX, WMAX);
}

/* 同一个控制器，忘了减车头朝向 */
static void nav_noth(const struct pose *p, float tx, float ty, float *v, float *w)
{
	float err = atan2f(ty - p->y, tx - p->x);	/* ← 少了 - p->th */
	float d = hypotf(tx - p->x, ty - p->y);

	if (d < TOL) {
		*v = 0.0f;
		*w = 0.0f;
		return;
	}
	*v = fminf(VMAX, 1.2f * d);
	*w = clampf_(4.0f * err, -WMAX, WMAX);
}

struct result {
	struct pose end;
	double dist, secs;
	int reached;
};

static struct result mission(void (*nav)(const struct pose *, float, float,
					 float *, float *))
{
	struct pose p = { 0.0f, 0.0f, START_TH };
	struct result r;

	r.dist = 0.0;
	r.secs = 0.0;
	r.reached = 0;

	while (r.secs < 20.0) {
		float v, w, ox = p.x, oy = p.y;

		nav(&p, GOAL_X, GOAL_Y, &v, &w);

		p.x += v * cosf(p.th) * DT;
		p.y += v * sinf(p.th) * DT;
		p.th = wrap_pi(p.th + w * DT);

		r.dist += hypotf(p.x - ox, p.y - oy);
		r.secs += (double)DT;

		if (hypotf(GOAL_X - p.x, GOAL_Y - p.y) < TOL) {
			r.reached = 1;
			break;
		}
	}
	r.end = p;
	return r;
}

/* ---------- 对准要多久 ---------- */

static void align_cost(float err0, float *turn_deg, double *secs)
{
	float err = err0;
	double t = 0.0, turned = 0.0;

	while (fabsf(err) >= 0.12f && t < 60.0) {
		float w = clampf_(4.0f * err, -WMAX, WMAX);

		err -= w * DT;
		turned += w * DT;
		t += (double)DT;
	}
	*turn_deg = turned * RAD2DEG;
	*secs = t;
}

/* ---------- 一条指令晚到多久，偏多少 ---------- */

static float late_miss(float delay_s, float w_turn, float step)
{
	struct pose now = { 0.0f, 0.0f, 0.0f };
	struct pose late = { 0.0f, 0.0f, w_turn * delay_s };
	struct vec2 a = body_to_world(&now, (struct vec2){ step, 0.0f });
	struct vec2 b = body_to_world(&late, (struct vec2){ step, 0.0f });

	return hypotf(b.x - a.x, b.y - a.y);
}

/* ---------- 两种限幅，各跑一秒 ---------- */

struct track {
	float x, y, th;
};

static struct track run_limit(struct motion_cmd c, double secs)
{
	struct track t = { 0.0f, 0.0f, 0.0f };

	for (double s = 0.0; s < secs; s += (double)DT) {
		t.x += c.v * cosf(t.th) * DT;
		t.y += c.v * sinf(t.th) * DT;
		t.th += c.w * DT;
	}
	return t;
}

int main(void)
{
	struct pose start = { 0.0f, 0.0f, START_TH };
	struct vec2 wv = { GOAL_X, GOAL_Y };
	struct vec2 bv = pose_to_body(&start, wv);
	struct result ok, bad;
	float t1, t2;
	double s1, s2;

	printf("=== 一、同一个点，两种说法 ===\n\n");
	printf("车在 (%.2f, %.2f)，车头朝 %+.1f°\n", (double)start.x,
	       (double)start.y, (double)(START_TH * RAD2DEG));
	printf("目标点 (%.2f, %.2f)\n\n", (double)GOAL_X, (double)GOAL_Y);
	printf("  世界系坐标   (%+.2f, %+.2f)   离 +x 轴 %+.2f°\n",
	       (double)wv.x, (double)wv.y,
	       (double)(atan2f(wv.y, wv.x) * RAD2DEG));
	printf("  车体系坐标   (%+.2f, %+.2f)   离车头 %+.2f°\n",
	       (double)bv.x, (double)bv.y,
	       (double)(atan2f(bv.y, bv.x) * RAD2DEG));
	printf("  方位角       %+.2f°\n",
	       (double)(bearing_to(&start, GOAL_X, GOAL_Y) * RAD2DEG));
	printf("  距离         %.3f m\n\n",
	       (double)hypotf(bv.x, bv.y));
	printf("世界系说\"右前方 %.0f 度\"，车体系说\"正右方 %.0f 度\"。\n",
	       (double)(atan2f(wv.y, wv.x) * RAD2DEG),
	       (double)fabs(bearing_to(&start, GOAL_X, GOAL_Y) * RAD2DEG));
	printf("差 %.1f 度，正好是车头朝向。\n\n",
	       (double)(START_TH * RAD2DEG));

	printf("=== 二、忘了减车头朝向 ===\n\n");
	ok = mission(nav_ok);
	bad = mission(nav_noth);

	printf("%-18s %-12s %-12s %-10s %s\n", "", "终点", "离目标", "走的路", "用时");
	printf("──────────────────────────────────────────────────────────\n");
	printf("%-18s (%+.2f,%+.2f)  %6.1f mm  %6.2f m  %.2f s\n", "正确",
	       (double)ok.end.x, (double)ok.end.y,
	       (double)(hypotf(GOAL_X - ok.end.x, GOAL_Y - ok.end.y) * 1000.0f),
	       ok.dist, ok.secs);
	printf("%-18s (%+.2f,%+.2f)  %6.1f mm  %6.2f m  %.2f s\n", "不减 θ",
	       (double)bad.end.x, (double)bad.end.y,
	       (double)(hypotf(GOAL_X - bad.end.x, GOAL_Y - bad.end.y) * 1000.0f),
	       bad.dist, bad.secs);
	printf("\n终点离目标 %s\n\n",
	       ok.reached ? "12 毫米以内" : "还在外面");

	printf("=== 三、角度不归一化 ===\n\n");
	printf("同一个 20 度的误差，两种写法：\n\n");
	align_cost(wrap_pi(-340.0f * DEG2RAD), &t1, &s1);
	align_cost(-340.0f * DEG2RAD, &t2, &s2);
	printf("%-26s %-12s %-12s %s\n", "", "初始误差", "转过", "用时");
	printf("──────────────────────────────────────────────────────────\n");
	printf("%-26s %+8.1f°    %+8.1f°   %.3f s\n", "wrap_pi（折进 ±π）",
	       (double)(wrap_pi(-340.0f * DEG2RAD) * RAD2DEG), (double)t1, s1);
	printf("%-26s %+8.1f°    %+8.1f°   %.3f s\n", "不折", -340.0,
	       (double)t2, s2);
	printf("\n差 %.1f 倍。车还是对准了，只是白转了一圈半。\n\n",
	       s2 / s1);

	printf("=== 四、指令晚到 ===\n\n");
	printf("车在以 1.0 rad/s 左转，队列里躺着一条 (v=0.30, w=0.00)。\n");
	printf("它说的是\"沿当时车头走 0.30 米\"。车头一直在转，\n");
	printf("晚取出来的每一毫秒，这句话的意思都在变。\n\n");
	printf("%8s   %10s   %10s   %s\n", "晚了", "车头已转", "终点偏",
	       "ttl=100ms");
	printf("──────────────────────────────────────────────────────────\n");
	{
		float ds[] = { 0.100f, 0.200f, 0.400f, 0.800f };

		for (int i = 0; i < 4; i++) {
			float m = late_miss(ds[i], 1.0f, 0.30f);

			printf("%6.0f ms   %8.1f°   %8.1f cm   %s\n",
			       (double)(ds[i] * 1e3f),
			       (double)(ds[i] * 1.0f * RAD2DEG),
			       (double)(m * 100.0f),
			       ds[i] <= 0.100f ? "收" : "扔");
		}
	}
	printf("\n");

	printf("=== 五、超速了怎么截 ===\n\n");
	{
		struct motion_cmd each, ratio;
		struct track te, tr;

		cmd_make(&each, 1, 0, 100, 0.50f, 2.00f);
		cmd_make(&ratio, 1, 0, 100, 0.50f, 2.00f);
		cmd_limit_each(&each, VMAX, WMAX);
		cmd_limit_ratio(&ratio, VMAX, WMAX);

		printf("上层要的是 v=0.50 m/s, w=2.00 rad/s");
		printf("（转弯半径 %.3f m）\n", 0.50 / 2.00);
		printf("底盘上限 v=%.2f, w=%.2f\n\n", (double)VMAX, (double)WMAX);

		te = run_limit(each, 1.0);
		tr = run_limit(ratio, 1.0);

		printf("%-14s %-8s %-8s %-10s %-18s %s\n", "", "v", "w",
		       "转弯半径", "1 秒后", "朝向");
		printf("──────────────────────────────────────────────────────────\n");
		printf("%-14s %-8.3f %-8.3f %-10.3f (%+.3f,%+.3f)   %+.1f°\n",
		       "分别截断", (double)each.v, (double)each.w,
		       (double)(each.v / each.w),
		       (double)te.x, (double)te.y,
		       (double)(te.th * RAD2DEG));
		printf("%-14s %-8.3f %-8.3f %-10.3f (%+.3f,%+.3f)   %+.1f°\n",
		       "等比缩放", (double)ratio.v, (double)ratio.w,
		       (double)(ratio.v / ratio.w),
		       (double)tr.x, (double)tr.y,
		       (double)(tr.th * RAD2DEG));
		printf("\n两点相距 %.1f cm，朝向差 %.1f°\n",
		       (double)(hypotf(te.x - tr.x, te.y - tr.y) * 100.0f),
		       (double)(fabsf(te.th - tr.th) * RAD2DEG));
	}
	return 0;
}
