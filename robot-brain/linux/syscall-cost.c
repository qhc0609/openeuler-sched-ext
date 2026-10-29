/*
 * syscall-cost.c —— 一次"进内核"到底要多久
 *
 * 机器人程序里每一拍都要调几个函数，但不是每个函数都要进内核。
 * 这个工具量五个：
 *
 *   clock_gettime      glibc 走 vDSO，用户态直接读共享页，不进内核
 *   syscall(getpid)    真系统调用，但什么活都不干——这就是"过门槛"的底价
 *   clock_nanosleep    真系统调用，睡 0 纳秒，立刻返回
 *   write(/dev/null)   真系统调用，走文件系统
 *   write(/dev/robotmotor) 真系统调用，还要进我们自己的驱动
 *
 * 编译：make
 * 跑法：sudo ./syscall-cost [每次调用次数]
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "motor_abi.h"

static volatile long g_sink;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define BENCH(label, n, code)						\
	do {								\
		long long _t0 = now_ns(), _t1;				\
		for (long _i = 0; _i < (n); _i++) { code; }		\
		_t1 = now_ns();						\
		printf("  %-28s %8.1f ns/次\n", label,			\
		       (double)(_t1 - _t0) / (double)(n));		\
	} while (0)

int main(int argc, char **argv)
{
	long n = 200000;
	int devnull, devmotor;
	struct timespec zero = { 0, 0 };
	struct motor_cmd cmd = { 0, 0, 0 };

	if (argc > 1)
		n = atol(argv[1]);
	if (n <= 0)
		n = 200000;

	devnull = open("/dev/null", O_WRONLY);
	devmotor = open("/dev/robotmotor", O_WRONLY);
	if (devmotor < 0) {
		fprintf(stderr, "打不开 /dev/robotmotor，驱动装了吗？"
			"（先 make && sudo insmod motor.ko）\n");
		return 1;
	}

	printf("== 每种调用 %ld 次 ==\n", n);

	BENCH("clock_gettime（vDSO）", n, {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		g_sink += ts.tv_nsec;
	});

	BENCH("syscall(getpid)（进内核）", n,
	      g_sink += syscall(SYS_getpid));

	BENCH("clock_nanosleep(0)（进内核）", n,
	      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &zero, NULL));

	BENCH("write(/dev/null)（进内核）", n,
	      g_sink += write(devnull, &cmd, sizeof(cmd)));

	BENCH("write(/dev/robotmotor)", n,
	      g_sink += write(devmotor, &cmd, sizeof(cmd)));

	close(devnull);
	close(devmotor);
	return 0;
}
