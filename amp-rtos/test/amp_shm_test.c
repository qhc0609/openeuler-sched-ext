/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * amp_shm_test.c —— 只测内核模块本身：两个进程能不能看到同一份内存，
 * 门铃能不能互相叫醒。
 *
 * 这一步和 OpenAMP 无关。先证明"共享内存 + 门铃"这两样硬件替身是好的，
 * 再往上搭 rpmsg。否则后面出问题，分不清是协议错还是通道错。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "amp_layout.h"

/* 往返多少次。100 次足够看出分布，又不至于跑太久 */
#define ROUNDS	100

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *amp_map(int *fdp, int role)
{
	struct amp_info info;
	void *p;
	int fd;

	fd = open("/dev/amp0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/amp0");
		exit(1);
	}

	if (ioctl(fd, AMP_IOC_BIND, &role) < 0) {
		perror("ioctl BIND");
		exit(1);
	}

	p = mmap(NULL, AMP_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	if (ioctl(fd, AMP_IOC_INFO, &info) == 0)
		printf("  [role %d] phys=0x%llx size=%llu\n", role,
		       (unsigned long long)info.phys,
		       (unsigned long long)info.size);

	*fdp = fd;
	return p;
}

/* 等门铃，最多等 timeout_ms */
static int wait_kick(int fd, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	return poll(&pfd, 1, timeout_ms);
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(void)
{
	struct amp_ctrl *ctrl;
	void *shm;
	int fd, i, ret;
	pid_t pid;

	/*
	 * 重定向到文件时 stdout 默认全缓冲。fork() 会把缓冲区一起复制，
	 * 子进程 exit 时再刷一遍，日志里就会出现两份一样的内容。
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("== 一、父进程（Linux 侧）先绑定，拿共享内存 ==\n");
	shm = amp_map(&fd, AMP_ROLE_HOST);
	ctrl = (struct amp_ctrl *)shm;

	/* 父进程负责初始化控制块 */
	memset(shm, 0, AMP_CTRL_SIZE);
	ctrl->magic = AMP_MAGIC;
	ctrl->version = AMP_VERSION;
	ctrl->shm_size = AMP_SHM_SIZE;
	ctrl->num_descs = AMP_NUM_DESCS;
	ctrl->buf_size = AMP_BUF_SIZE_EACH;

	printf("  magic=0x%08x version=%u descs=%u buf=%u\n",
	       ctrl->magic, ctrl->version, ctrl->num_descs, ctrl->buf_size);

	printf("\n== 二、fork 出 RTOS 侧，绑另一个角色 ==\n");
	pid = fork();
	if (pid == 0) {
		struct amp_ctrl *c;
		void *s;
		int cfd;

		s = amp_map(&cfd, AMP_ROLE_RTOS);
		c = (struct amp_ctrl *)s;

		if (c->magic != AMP_MAGIC) {
			printf("  [rtos] magic 不对：0x%08x\n", c->magic);
			exit(1);
		}
		printf("  [rtos] 看到的 magic=0x%08x，和父进程写的一样\n", c->magic);

		c->rtos_ready = 1;

		/* 等 ROUNDS 次门铃，每次回一次 */
		for (i = 0; i < ROUNDS; i++) {
			if (wait_kick(cfd, 2000) <= 0) {
				printf("  [rtos] 第 %d 次等门铃超时\n", i);
				exit(1);
			}
			c->rtos_kicks += 0;	/* 读一下，确认能看见 */
			ioctl(cfd, AMP_IOC_KICK, (int[]){ AMP_ROLE_HOST });
		}

		printf("  [rtos] %d 次门铃全部收到并已回铃\n", ROUNDS);
		exit(0);
	}

	printf("\n== 三、双向门铃：父进程敲 %d 次，子进程每次回一次 ==\n",
	       ROUNDS);
	usleep(200000);		/* 等子进程绑定好 */

	{
		static long long rtt[ROUNDS];
		long long t0, t1;
		int n = 0;

		for (i = 0; i < ROUNDS; i++) {
			t0 = now_ns();
			ioctl(fd, AMP_IOC_KICK, (int[]){ AMP_ROLE_RTOS });
			ret = wait_kick(fd, 2000);
			t1 = now_ns();

			if (ret <= 0) {
				printf("  第 %d 次没等到回铃（ret=%d）\n", i, ret);
				break;
			}
			rtt[n++] = t1 - t0;
		}

		if (n > 0) {
			qsort(rtt, (size_t)n, sizeof(rtt[0]), cmp_ll);
			printf("  %d 次往返：最小 %lld ns，中位 %lld ns，"
			       "p99 %lld ns，最大 %lld ns\n",
			       n, rtt[0], rtt[n / 2],
			       rtt[(int)((long long)n * 99 / 100)], rtt[n - 1]);
		}
	}

	waitpid(pid, NULL, 0);

	printf("\n== 四、门铃计数（内核维护） ==\n");
	{
		struct amp_info info;

		ioctl(fd, AMP_IOC_INFO, &info);
		printf("  Linux->RTOS %u 次，RTOS->Linux %u 次\n",
		       info.host_kicks, info.rtos_kicks);
		printf("  控制块里的计数：%u / %u\n",
		       ctrl->host_kicks, ctrl->rtos_kicks);
	}

	munmap(shm, AMP_SHM_SIZE);
	close(fd);
	return 0;
}
