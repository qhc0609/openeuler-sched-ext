/*
 * first-bot.c —— 第一个机器人控制程序
 *
 * 干的事很简单：每 1 毫秒算一次两个关节角，通过 /dev/robotmotor 写下去。
 * 但它用到了 Linux 给机器人程序的全部四样东西：
 *
 *   用户态应用   这个程序本身
 *   系统调用     clock_nanosleep 定周期，write 下发电机指令
 *   线程         控制线程跑 1 kHz，主线程跑 30 Hz 做感知
 *   驱动         /dev/robotmotor，write 进去的指令由 motor.ko 接收
 *
 * 三种结构可以对比（--mode）：
 *   thread   控制线程 1 kHz，感知在主线程（默认）
 *   single   一个线程全干完，感知写在控制循环里
 *   process  fork 一个子进程跑控制，感知在父进程
 *
 * --sensor-us 是"一次感知要多久"的替身。相机检测、点云配准、
 * 模型推理都在这个量级：几毫秒。
 *
 * 编译：make
 * 跑法：sudo ./first-bot --mode thread --sensor-us 5000
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "motor_abi.h"

#define DEV		"/dev/robotmotor"
#define NSAMP		40000
#define SECS		5
#define SENSOR_HZ	30

/*
 * 统计区。--mode process 时这块内存要在 fork 之前 mmap 成共享的，
 * 父子进程才看得到同一份数。
 */
struct stats {
	unsigned long long ticks;
	unsigned long long missed;
	unsigned long long write_ns;
	long long per[NSAMP];
	int stop;
};

struct ctl_arg {
	struct stats *st;
	long sensor_us;		/* 只有 single 模式用得上 */
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(long long t)
{
	struct timespec ts = { t / 1000000000LL, t % 1000000000LL };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* 忙等，代表一次真正的计算：视觉检测、点云配准、模型推理 */
static void busy_us(long us)
{
	long long until;

	if (us <= 0)
		return;
	until = now_ns() + us * 1000LL;
	while (now_ns() < until)
		;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

/* 把当前执行体钉在某个核上，并给它实时优先级 */
static int make_rt(int cpu, int prio)
{
	cpu_set_t set;
	struct sched_param sp;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		return -1;

	if (prio > 0) {
		sp.sched_priority = prio;
		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
			return -1;
	}
	return 0;
}

/*
 * 控制循环本体。
 *
 * 四件事，顺序不能换：等这一拍 → 算指令 → 写下去 → 记数。
 * 写下去那一步是系统调用，进内核，花的时间算在这一拍里。
 */
static void control_loop(int fd, struct stats *st, int hz, long secs,
			 int cpu, int prio, long sensor_us)
{
	long long period = 1000000000LL / hz;
	long long next, prev, t0, t1;
	unsigned long long n = 0, miss = 0, wsum = 0;
	unsigned seq = 0;

	if (make_rt(cpu, prio) < 0)
		fprintf(stderr, "  （绑核或实时优先级失败，继续跑）\n");

	prev = now_ns();
	next = prev;

	while (n < (unsigned long long)secs * hz) {
		struct motor_cmd cmd;
		long long per;

		next += period;
		sleep_until(next);
		t0 = now_ns();

		/*
		 * 感知。sensor_us 大于 0 时每 33 拍做一次——相当于 30 Hz 的相机，
		 * 一次检测要 sensor_us 微秒。放在控制循环里，它就占这一拍。
		 */
		if (sensor_us > 0 && n % 33 == 0)
			busy_us(sensor_us);

		/* 算这一拍的关节角：绕着一个正弦走，只为让数据在动 */
		cmd.seq = seq++;
		cmd.q1_mrad = (int)(sinf((float)n * 0.001f) * 800.0f);
		cmd.q2_mrad = (int)(cosf((float)n * 0.001f) * 500.0f);

		/* 这一行是系统调用：用户态 → 内核 → 驱动 */
		if (write(fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
			break;

		t1 = now_ns();
		wsum += (unsigned long long)(t1 - t0);

		per = t1 - prev;
		if (n < NSAMP)
			st->per[n] = per;
		if (per > period * 3 / 2)
			miss++;
		prev = t1;
		n++;
	}

	st->ticks = n;
	st->missed = miss;
	st->write_ns = wsum;
}

static void *control_thread(void *arg)
{
	struct ctl_arg *a = arg;
	int fd = open(DEV, O_WRONLY);

	if (fd < 0) {
		perror("open " DEV);
		return NULL;
	}
	control_loop(fd, a->st, 1000, SECS, 2, 50, a->sensor_us);
	close(fd);
	return NULL;
}

/* 另一个执行体专门做感知：主线程这么干，父进程也这么干 */
static void sensor_loop(struct stats *st, long sensor_us, int cpu)
{
	if (cpu >= 0)
		make_rt(cpu, 0);

	for (int i = 0; i < SECS * SENSOR_HZ; i++) {
		usleep(1000000 / SENSOR_HZ);
		busy_us(sensor_us);
		if (st->ticks >= (unsigned long long)SECS * 1000)
			break;
	}
}

static void report(struct stats *st, const char *tag)
{
	unsigned long long n = st->ticks;
	int m = (int)(n < NSAMP ? n : NSAMP);

	if (!m) {
		printf("  没有采到样本\n");
		return;
	}
	qsort(st->per, (size_t)m, sizeof(st->per[0]), cmp_ll);

	printf("\n== %s ==\n", tag);
	printf("  拍了 %llu 次，超过 1.5 倍周期 %llu 次（%.2f%%）\n",
	       n, st->missed, 100.0 * (double)st->missed / (double)n);
	printf("  周期 p50 %.1f us，p99 %.1f us，最大 %.1f us\n",
	       st->per[m / 2] / 1000.0, st->per[m * 99 / 100] / 1000.0,
	       st->per[m - 1] / 1000.0);
	printf("  每拍 write() 平均 %.2f us（占 1 ms 预算的 %.2f%%）\n",
	       (double)st->write_ns / (double)n / 1000.0,
	       100.0 * (double)st->write_ns / (double)n / 1e6);
}

int main(int argc, char **argv)
{
	const char *mode = "thread";
	long sensor_us = 0;
	struct stats *st;
	struct ctl_arg arg;
	int fd;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mode") && i + 1 < argc)
			mode = argv[++i];
		else if (!strcmp(argv[i], "--sensor-us") && i + 1 < argc)
			sensor_us = atol(argv[++i]);
		else {
			fprintf(stderr, "用法: first-bot [--mode thread|single|"
				"process] [--sensor-us N]\n");
			return 2;
		}
	}

	/* 共享映射：fork 之后父子进程看到同一块 */
	st = mmap(NULL, sizeof(*st), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (st == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(st, 0, sizeof(*st));
	arg.st = st;

	if (!strcmp(mode, "thread")) {
		pthread_t th;

		arg.sensor_us = 0;	/* 感知不在控制线程里 */
		if (pthread_create(&th, NULL, control_thread, &arg) != 0) {
			perror("pthread_create");
			return 1;
		}
		sensor_loop(st, sensor_us, 1);
		pthread_join(th, NULL);
		report(st, "线程：控制线程 1 kHz，感知在主线程");

	} else if (!strcmp(mode, "single")) {
		fd = open(DEV, O_WRONLY);
		if (fd < 0) {
			perror("open " DEV);
			return 1;
		}
		/* 感知写在控制循环里，占的是同一拍 */
		control_loop(fd, st, 1000, SECS, 2, 50, sensor_us);
		close(fd);
		report(st, "单线程：感知写在控制循环里");

	} else if (!strcmp(mode, "process")) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			/* 子进程：只跑控制 */
			fd = open(DEV, O_WRONLY);
			if (fd < 0)
				_exit(1);
			control_loop(fd, st, 1000, SECS, 2, 50, 0);
			close(fd);
			_exit(0);
		}
		sensor_loop(st, sensor_us, 1);
		waitpid(pid, NULL, 0);
		report(st, "进程：控制子进程 1 kHz，感知在父进程");
	} else {
		fprintf(stderr, "未知模式 %s\n", mode);
		return 2;
	}

	munmap(st, sizeof(*st));
	return 0;
}
