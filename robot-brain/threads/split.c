/*
 * split.c —— 拆成四个线程，中间放一个队列
 *
 * 接收、决策、日志各一个线程，控制还是 1 kHz。队列有两种实现、三种满的策略：
 *
 *   --q mutex --drop none   无界链表。满了不丢，一直涨
 *   --q ring  --drop old    满了把最旧的挤掉
 *   --q ring  --drop new    满了不收新的
 *
 * 另外两个旋钮：
 *   --cpus N    把这四个线程摊到 N 个核上（0 = 不绑）
 *   --batch N   日志线程攒 N 行才 fsync 一次（0 = 每行都 fsync）
 *
 * 决策每 100 ms 跑一次 8 ms，跑完顺手把 200 行轨迹倒进日志。
 * 于是日志的产生速度是 1000 + 2000 = 3000 行/秒，而 fsync 每行一次
 * 只吃得下 1287 行/秒——差 2.3 倍，队列一定涨。
 *
 * 跑法：./bin/split --q ring --drop new --cpus 4 --batch 0 --log /root/rb.log
 */
#define _GNU_SOURCE
#include "common.h"
#include "ring.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#define MAXN		10000
#define CTL_NS		50000ull
#define RX_NS		5000ull
#define DECIDE_NS	8000000ull
#define DECIDE_EVERY	100
#define BURST		200
#define RX_CAP		64
#define LOG_CAP		4096
#define POOL		65536

struct sample {
	uint64_t t_ns;
	uint32_t seq;
	float ax, gyro;
};

struct rec {
	uint64_t t_ns;
	uint32_t seq;
	int kind;		/* 0 控制，1 决策 */
	float v[3];
};

/* ---- 无界链表队列（只用来演示"一直涨"） ---- */
struct node {
	struct node *next;
	struct rec r;
};

static struct node g_pool[POOL];
static struct node *g_free;
static struct node *g_head, *g_tail;
static size_t g_qn;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void pool_init(void)
{
	int i;

	for (i = 0; i < POOL - 1; i++)
		g_pool[i].next = &g_pool[i + 1];
	g_pool[POOL - 1].next = NULL;
	g_free = &g_pool[0];
}

/* ---- 队列抽象 ---- */
static struct ring g_rx, g_log;
static struct sample g_rxbuf[RX_CAP];
static struct rec g_logbuf[LOG_CAP];

enum { Q_MUTEX, Q_RING };
static int g_q = Q_RING;
static int g_drop_old = 1;	/* ring 专用 */
static int g_batch;		/* 0 = 每行 fsync */
static int g_rate = 2;		/* rx 每拍送几帧（传感器比控制快） */
static int g_drain;		/* 控制每拍是否排空队列、只留最新一帧 */
static int g_cap = LOG_CAP;	/* 日志队列的深度 */

static size_t g_log_dropped, g_rx_dropped, g_log_peak;
static _Atomic uint32_t g_next_id;	/* 每条日志的全局编号 */
static uint32_t g_last_id;		/* 真正落盘的最后一条编号 */

static void q_rx_init(void)
{
	ring_init(&g_rx, g_rxbuf, RX_CAP, sizeof(struct sample));
	ring_init(&g_log, g_logbuf, (size_t)g_cap, sizeof(struct rec));
	pool_init();
}

static void log_push(struct rec *r)
{
	size_t d;

	r->seq = atomic_fetch_add(&g_next_id, 1);

	if (g_q == Q_MUTEX) {
		struct node *n;

		pthread_mutex_lock(&g_mu);
		n = g_free;
		if (!n) {		/* 池子也空了，只能丢 */
			g_log_dropped++;
			pthread_mutex_unlock(&g_mu);
			return;
		}
		g_free = n->next;
		n->r = *r;
		n->next = NULL;
		if (g_tail)
			g_tail->next = n;
		else
			g_head = n;
		g_tail = n;
		d = ++g_qn;
		if (d > g_log_peak)
			g_log_peak = d;
		pthread_mutex_unlock(&g_mu);
		return;
	}

	/* 同一个环形队列，只换"满了丢谁" */
	if (g_drop_old) {
		if (ring_push_drop_old(&g_log, r))
			g_log_dropped++;
	} else {
		if (!ring_push_drop_new(&g_log, r))
			g_log_dropped++;
	}
	d = ring_count(&g_log);
	if (d > g_log_peak)
		g_log_peak = d;
}

static int log_pop(struct rec *r)
{
	if (g_q == Q_MUTEX) {
		struct node *n;
		int ok = 0;

		pthread_mutex_lock(&g_mu);
		n = g_head;
		if (n) {
			g_head = n->next;
			if (!g_head)
				g_tail = NULL;
			g_qn--;
			*r = n->r;
			n->next = g_free;
			g_free = n;
			ok = 1;
		}
		pthread_mutex_unlock(&g_mu);
		return ok;
	}
	return ring_pop(&g_log, r);
}

/* ---- 线程 ---- */
static uint64_t g_t0;
static volatile int g_stop;
static int g_n;			/* 实际拍数 = 秒数 * 1000 */
static uint64_t g_lat[MAXN], g_age[MAXN], g_lag[MAXN * 4];
static int g_nage, g_nlag, g_starve, g_written;
static FILE *g_lf;

static void pin(int cpu, int cpus)
{
	cpu_set_t set;

	if (cpus <= 0)
		return;
	CPU_ZERO(&set);
	CPU_SET(cpu % cpus, &set);
	pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static void *rx_thread(void *arg)
{
	uint64_t next = g_t0;
	struct sample s = { 0, 0, 0.01f, 0.002f };
	int i, k;

	pin(1, *(int *)arg);
	for (i = 0; i < g_n; i++) {
		next += PERIOD_NS;
		wait_until(next);
		burn_ns(RX_NS);
		s.ax += 0.0001f;
		for (k = 0; k < g_rate; k++) {
			s.t_ns = now_ns();
			s.seq = (uint32_t)(i * g_rate + k);
			if (ring_push_drop_old(&g_rx, &s))
				g_rx_dropped++;
		}
	}
	return NULL;
}

static void *decide_thread(void *arg)
{
	uint64_t next = g_t0;
	struct rec r;
	int i, k;

	pin(2, *(int *)arg);
	for (i = 0; i < g_n / DECIDE_EVERY; i++) {
		next += DECIDE_EVERY * PERIOD_NS;
		wait_until(next);
		burn_ns(DECIDE_NS);
		r.t_ns = now_ns();
		r.kind = 1;
		for (k = 0; k < BURST; k++) {
			r.seq = (uint32_t)(i * BURST + k);
			r.v[0] = (float)k * 0.01f;
			r.v[1] = 0.0f;
			r.v[2] = 0.0f;
			log_push(&r);
		}
	}
	return NULL;
}

static void *log_thread(void *arg)
{
	struct rec r;
	int pending = 0;

	pin(3, *(int *)arg);
	while (!g_stop) {
		if (!log_pop(&r)) {
			struct timespec ts = { 0, 50000 };

			nanosleep(&ts, NULL);
			continue;
		}
		if (g_nlag < MAXN * 4)
			g_lag[g_nlag++] = now_ns() - r.t_ns;
		g_written++;
		g_last_id = r.seq;
		fprintf(g_lf, "%06u %d %.4f %.4f %.4f\n", r.seq, r.kind,
			r.v[0], r.v[1], r.v[2]);
		if (g_batch) {
			if (++pending >= g_batch) {
				fflush(g_lf);
				fsync(fileno(g_lf));
				pending = 0;
			}
		} else {
			fflush(g_lf);
			fsync(fileno(g_lf));
		}
	}
	fflush(g_lf);
	fsync(fileno(g_lf));
	return NULL;
}

static void *ctl_thread(void *arg)
{
	uint64_t next = g_t0;
	struct rec r;
	int i, miss = 0;

	pin(0, *(int *)arg);
	for (i = 0; i < g_n; i++) {
		struct sample s;
		uint64_t t;
		int got;

		next += PERIOD_NS;
		wait_until(next);
		t = now_ns();
		g_lat[i] = t - next;
		if (g_lat[i] >= PERIOD_NS)
			miss++;

		if (g_drain) {
			/* 排空：只留最新的一帧，中间的全扔掉 */
			struct sample last;

			got = 0;
			while (ring_pop(&g_rx, &s)) {
				last = s;
				got = 1;
			}
			s = last;
		} else {
			got = ring_pop(&g_rx, &s);
		}
		if (got) {
			long long age = (long long)t - (long long)s.t_ns;

			if (age < 0)
				age = 0;
			if (g_nage < MAXN)
				g_age[g_nage++] = (uint64_t)age;
		} else {
			g_starve++;
		}

		burn_ns(CTL_NS);

		r.t_ns = now_ns();
		r.seq = (uint32_t)i;
		r.kind = 0;
		r.v[0] = 1.0f;
		r.v[1] = 0.5f;
		r.v[2] = 0.25f;
		log_push(&r);
	}
	g_stop = 1;
	return NULL;
}

static void stat1(const char *label, uint64_t *v, int n)
{
	qsort(v, n, sizeof v[0], cmp_u64);
	printf("   %s p50 %.1f us   p99 %.1f us   max %.1f us\n", label,
	       (double)pct(v, n, 50) / 1e3,
	       (double)pct(v, n, 99) / 1e3,
	       (double)v[n - 1] / 1e3);
}

int main(int argc, char **argv)
{
	const char *logpath = "/root/rb.log";
	pthread_t th[4];
	int cpus = 0, secs = 5, i, miss = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--q") && i + 1 < argc) {
			g_q = strcmp(argv[++i], "mutex") ? Q_RING : Q_MUTEX;
		} else if (!strcmp(argv[i], "--drop") && i + 1 < argc) {
			g_drop_old = !strcmp(argv[++i], "old");
		} else if (!strcmp(argv[i], "--cpus") && i + 1 < argc) {
			cpus = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--cap") && i + 1 < argc) {
			g_cap = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			g_rate = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--drain") && i + 1 < argc) {
			g_drain = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--batch") && i + 1 < argc) {
			g_batch = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--secs") && i + 1 < argc) {
			secs = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
			logpath = argv[++i];
		} else {
			fprintf(stderr, "用法: split [--q mutex|ring] "
				"[--drop old|new] [--cpus N] [--batch N] "
				"[--log PATH]\n");
			return 2;
		}
	}

	g_lf = fopen(logpath, "w");
	if (!g_lf) {
		perror(logpath);
		return 1;
	}
	setvbuf(g_lf, NULL, _IOFBF, 1 << 16);
	if (secs < 1)
		secs = 1;
	if (secs * 1000 > MAXN)
		secs = MAXN / 1000;
	g_n = secs * 1000;
	q_rx_init();

	printf("== 四个线程 / 队列：%s，满时%s / %s / 日志%s ==\n",
	       g_q == Q_MUTEX ? "无界链表" : "环形队列",
	       g_q == Q_MUTEX ? "不丢" : (g_drop_old ? "丢最旧" : "丢最新"),
	       cpus > 0 ? "绑到固定核" : "不绑核",
	       g_batch ? "攒够了再 fsync" : "每行 fsync");
	if (g_q != Q_MUTEX)
		printf("   日志队列 %d 条\n", g_cap);
	if (g_batch)
		printf("   日志线程每 %d 行 fsync 一次\n", g_batch);
	printf("   传感器每拍送 %d 帧（%.0f Hz），控制每拍%s\n",
	       g_rate, (double)g_rate * 1000.0,
	       g_drain ? "排空队列只留最新一帧" : "只取一帧");

	g_t0 = now_ns() + 50 * PERIOD_NS;
	pthread_create(&th[0], NULL, ctl_thread, &cpus);
	pthread_create(&th[1], NULL, rx_thread, &cpus);
	pthread_create(&th[2], NULL, decide_thread, &cpus);
	pthread_create(&th[3], NULL, log_thread, &cpus);
	for (i = 0; i < 4; i++)
		pthread_join(th[i], NULL);

	for (i = 0; i < g_n; i++)
		if (g_lat[i] >= PERIOD_NS)
			miss++;

	qsort(g_lat, g_n, sizeof g_lat[0], cmp_u64);
	printf("   控制 %d 次，迟到 p50 %.1f us   p99 %.1f us   max %.1f us，"
	       "丢拍 %d\n",
	       g_n, (double)pct(g_lat, g_n, 50) / 1e3,
	       (double)pct(g_lat, g_n, 99) / 1e3,
	       (double)g_lat[g_n - 1] / 1e3, miss);
	stat1("数据龄", g_age, g_nage > 0 ? g_nage : 1);
	printf("   传感器队列丢 %zu 条，控制没拿到数据 %d 次\n",
	       g_rx_dropped, g_starve);
	stat1("日志落后", g_lag, g_nlag > 0 ? g_nlag : 1);
	printf("   日志写出 %d 条，丢 %zu 条，队列最深 %zu 条\n",
	       g_written, g_log_dropped, g_log_peak);
	printf("   落盘的最后一条编号 %u，一共产生 %u 条\n\n",
	       g_last_id, atomic_load(&g_next_id));

	fclose(g_lf);
	return 0;
}
