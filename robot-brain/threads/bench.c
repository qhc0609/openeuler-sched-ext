/*
 * bench.c —— 队列和日志各自的价钱
 *
 * 三个数：
 *   1. 环形队列进出一次多少纳秒，互斥锁进出一次多少纳秒
 *      —— 这两个数差多少，说明"用不用锁"值不值钱
 *   2. 写一行日志、fsync 一次多少微秒
 *      —— 这个数除以 1 ms，说明日志线程吃不吃得下 1 kHz 的产量
 *   3. 连续 fsync 的时候，一秒钟到底能落多少行
 *      —— 单次的 777 us 乘 1000 是 777 ms，但连续跑起来不是这么算的
 *
 * 跑法：./bin/bench /root/rb-bench.log
 */
#include "common.h"
#include "ring.h"

#include <pthread.h>
#include <unistd.h>

#define ITER		1000000

struct rec {
	uint64_t t_ns;
	uint32_t seq;
	int kind;
	float v[3];
};

static struct rec g_buf[4096];

static double per_iter_ns(uint64_t total, int n)
{
	return (double)total / (double)n;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/root/rb-bench.log";
	struct ring r;
	pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	struct rec e = { 0, 0, 0, { 1.0f, 0.5f, 0.25f } };
	uint64_t t0, t1;
	FILE *f;
	int i, ok = 0;

	/* ---- 1. 队列 vs 锁 ---- */
	ring_init(&r, g_buf, 4096, sizeof(struct rec));
	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		ring_push_drop_new(&r, &e);
		ring_pop(&r, &e);
	}
	t1 = now_ns();
	printf("%7.1f ns   环形队列：进一次 + 出一次\n",
	       per_iter_ns(t1 - t0, ITER));

	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		pthread_mutex_lock(&mu);
		pthread_mutex_unlock(&mu);
	}
	t1 = now_ns();
	printf("%7.1f ns   互斥锁：加锁 + 解锁（没有别人抢）\n",
	       per_iter_ns(t1 - t0, ITER));

	/* ---- 2. 写一行 ---- */
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return 1;
	}
	setvbuf(f, NULL, _IONBF, 0);

	t0 = now_ns();
	for (i = 0; i < 20000; i++)
		fprintf(f, "%06d %d %.4f %.4f %.4f\n", i, 0, 1.0f, 0.5f, 0.25f);
	t1 = now_ns();
	printf("%7.1f ns   fprintf 写一行（三个 float）\n",
	       per_iter_ns(t1 - t0, 20000));

	/* ---- 3. fsync 跟大小无关 ---- */
	{
		static const size_t sz[] = { 256, 4096, 65536 };
		static char blob[65536];

		memset(blob, 'x', sizeof blob);
		for (i = 0; i < 3; i++) {
			uint64_t sum = 0, mx = 0;
			int k, n = 100;

			for (k = 0; k < n; k++) {
				uint64_t a, b;

				fwrite(blob, 1, sz[i], f);
				a = now_ns();
				fsync(fileno(f));
				b = now_ns();
				sum += b - a;
				if (b - a > mx)
					mx = b - a;
			}
			printf("%7.1f us   fsync 一次（%5zu 字节，均值；最大 %.1f us）\n",
			       (double)sum / (double)n / 1e3, sz[i],
			       (double)mx / 1e3);
		}
	}

	/* ---- 4. 连续 fsync，一秒能落多少行 ---- */
	{
		int n = 2000, k;
		double wall, lines_per_s;

		t0 = now_ns();
		for (k = 0; k < n; k++) {
			fprintf(f, "%06d %d %.4f %.4f %.4f\n", k, 0, 1.0f,
				0.5f, 0.25f);
			fsync(fileno(f));
			ok++;
		}
		t1 = now_ns();
		wall = (double)(t1 - t0) / 1e9;
		lines_per_s = (double)n / wall;
		printf("%7.0f 行/秒   每行一次 fsync，连续 %d 行\n",
		       lines_per_s, n);
		printf("%7.2f       相对 1 kHz 控制回路的产量（1000 行/秒）\n",
		       lines_per_s / 1000.0);
	}

	fclose(f);
	printf("\n(%d 行都写了，%s)\n", ok, path);
	return 0;
}
