/* 只测一件事：clock_nanosleep 的绝对睡眠到底晚了多少微秒 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20;
	long long step = 150000000LL, t0 = now_ns();
	long long *d = malloc(sizeof(long long) * n);

	for (int i = 0; i < n; i++) {
		long long target = t0 + (long long)(i + 1) * step;
		struct timespec next = {
			.tv_sec = target / 1000000000LL,
			.tv_nsec = target % 1000000000LL,
		};

		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
		d[i] = now_ns() - target;
	}
	for (int i = 0; i < n; i++)
		printf("%lld%s", d[i] / 1000, i + 1 == n ? "\n" : " ");
	return 0;
}
