/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * main.c —— RTOS 侧（OpenAMP 里的 remote / virtio device）
 *
 * 这是 FreeRTOS，不是 Linux 线程池套了个壳：任务、优先级、1 kHz tick、
 * 抢占式调度，都是 FreeRTOS 自己的。跑在 Linux 上只是因为这台机器没有
 * 第二颗能跑裸机的核，用的是 FreeRTOS 官方的 POSIX 端口。
 *
 * 三个任务：
 *
 *   amp_task      等门铃 → 处理 vring → 回显。对应真板卡上的 mailbox 中断服务
 *   control_task  1 kHz 控制环，算设定值，同时记录自己每一拍的抖动
 *   stats_task    每秒打一行状态
 *
 * 上半部分（main 里）是 OpenAMP 的初始化，和 Linux 侧用的是同一份
 * amp_transport.c，区别只有 role。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "FreeRTOS.h"
#include "task.h"

#include <openamp/rpmsg_virtio.h>

#include "amp_layout.h"
#include "amp_transport.h"

#define CONTROL_HZ	1000
#define TICK_NS		(1000000000LL / CONTROL_HZ)

static int g_fd = -1;
static void *g_shm;
static struct amp_ctrl *g_ctrl;
static struct amp_rt_state *g_rt;

static struct amp_transport g_t;
static struct rpmsg_virtio_device g_rvdev;
static struct rpmsg_endpoint g_echo;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* notify：敲 Linux 的门铃 */
static int rtos_kick(struct amp_transport *t)
{
	int target = AMP_ROLE_HOST;

	(void)t;
	return ioctl(g_fd, AMP_IOC_KICK, &target);
}

/*
 * 回显端点。收到什么原样发回去 —— 这是 OpenAMP 的 hello world，
 * 也是量往返时间最干净的办法：中间没有任何业务逻辑。
 */
static int echo_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		   uint32_t src, void *priv)
{
	(void)src;
	(void)priv;

	rpmsg_send(ept, data, len);
	g_rt->echo_count++;
	return 0;
}

/*
 * 等门铃。真板卡上这里是中断服务程序，唤醒一个任务去处理 vring；
 * 这里 poll() 等内核把门铃投过来，然后调 virtqueue_notification()。
 */
static void amp_task(void *arg)
{
	struct pollfd pfd;

	(void)arg;
	pfd.fd = g_fd;
	pfd.events = POLLIN;

	for (;;) {
		if (poll(&pfd, 1, 20) > 0)
			virtqueue_notification(g_rvdev.rvq);
	}
}

/*
 * 1 kHz 控制环。这就是"小脑"该干的活：定周期算设定值。
 *
 * 每一拍记一次实际唤醒时刻，减掉 1 毫秒就是这一拍的抖动。
 * 抖动小说明这个核真的归我；抖动大说明有人在抢。
 */
static void control_task(void *arg)
{
	TickType_t last = xTaskGetTickCount();
	long long prev = 0;
	unsigned int n = 0;
	float phase = 0.0f;

	(void)arg;

	for (;;) {
		long long now, dev, absdev;
		int b;

		vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / CONTROL_HZ));

		now = now_ns();
		if (prev) {
			dev = now - prev - TICK_NS;
			absdev = (dev < 0) ? -dev : dev;

			g_rt->last_jitter_ns = (amp_u32)absdev;
			if (absdev > (long long)g_rt->max_jitter_ns)
				g_rt->max_jitter_ns = (amp_u32)absdev;
			if (absdev > TICK_NS / 2)
				g_rt->tick_late++;

			/* 直方图：第一个边界比它小的档 */
			for (b = 0; b < AMP_JIT_BUCKETS - 1; b++) {
				if ((amp_u64)absdev < amp_jit_edge_ns[b])
					break;
			}
			g_rt->hist[b]++;
		}
		prev = now;

		/* 一条 1 Hz 的正弦轨迹，代表机械臂关节的目标位置 */
		phase += 6.2831853f / CONTROL_HZ;
		if (phase > 6.2831853f)
			phase -= 6.2831853f;
		g_rt->setpoint = sinf(phase);
		g_rt->feedback = g_rt->setpoint * 0.98f;

		g_rt->tick_count = ++n;
		g_rt->seq++;
	}
}

static void stats_task(void *arg)
{
	(void)arg;

	printf("[rtos] 统计任务已启动，每秒一行\n");
	fflush(stdout);

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		printf("[rtos] tick=%u late=%u max_jitter=%.1f us echo=%u "
		       "kicks_out=%u\n",
		       g_rt->tick_count, g_rt->tick_late,
		       g_rt->max_jitter_ns / 1000.0f,
		       g_rt->echo_count, g_ctrl->rtos_kicks);
		fflush(stdout);
	}
}

int main(void)
{
	struct rpmsg_virtio_shm_pool shpool;
	int role = AMP_ROLE_RTOS;
	int ret;

	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("== RTOS 侧启动（FreeRTOS %s，tick %d Hz）==\n",
	       tskKERNEL_VERSION_NUMBER, CONTROL_HZ);

	g_fd = open("/dev/amp0", O_RDWR);
	if (g_fd < 0) {
		perror("open /dev/amp0");
		return 1;
	}

	if (ioctl(g_fd, AMP_IOC_BIND, &role) < 0) {
		perror("ioctl BIND");
		return 1;
	}

	g_shm = mmap(NULL, AMP_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     g_fd, 0);
	if (g_shm == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	g_ctrl = (struct amp_ctrl *)g_shm;
	g_rt = (struct amp_rt_state *)AMP_PTR(g_shm, AMP_RT_STATE_OFF);
	memset(g_rt, 0, sizeof(*g_rt));

	if (g_ctrl->magic != AMP_MAGIC) {
		printf("  控制块 magic 不对（0x%08x），Linux 侧还没起来？\n",
		       g_ctrl->magic);
		return 1;
	}

	g_ctrl->rtos_tick_hz = CONTROL_HZ;
	g_ctrl->rtos_ready = 1;

	printf("  共享内存已映射，控制块 magic=0x%08x\n", g_ctrl->magic);

	amp_transport_init(&g_t, g_shm, AMP_ROLE_RTOS, rtos_kick);
	rpmsg_virtio_init_shm_pool(&shpool, AMP_PTR(g_shm, AMP_BUF_OFF),
				   AMP_BUF_SIZE);

	/*
	 * 设备角色：这个调用会一直等，直到 host 把 status 推到 DRIVER_OK。
	 * 所以要先起 Linux 侧，再起 RTOS 侧。
	 */
	printf("  等 host 把 virtio status 推到 DRIVER_OK ...\n");
	fflush(stdout);

	ret = rpmsg_init_vdev(&g_rvdev, &g_t.vdev, NULL, &g_t.io, &shpool);
	if (ret) {
		printf("  rpmsg_init_vdev 失败 %d\n", ret);
		return 1;
	}
	printf("  status=0x%02x，两个 vring 已就绪\n", g_ctrl->status);

	ret = rpmsg_create_ept(&g_echo, &g_rvdev.rdev, "rpmsg-echo",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, echo_cb, NULL);
	if (ret) {
		printf("  建回显端点失败 %d\n", ret);
		return 1;
	}
	printf("  回显端点已建立 addr=%u，名字服务已广播\n", g_echo.addr);
	fflush(stdout);

	/*
	 * 优先级是有讲究的，不能随便排。
	 *
	 * 这个端口里，任务阻塞在 poll() 上时，FreeRTOS 并不知道它"在等"——
	 * 在调度器眼里它就是在正常运行。所以如果等门铃的任务优先级最高，
	 * 它会把 CPU 一直占住，控制环一次都轮不到（第一版就是这样，
	 * 两秒内 tick_count 是 0）。
	 *
	 * 把它放到最低：每来一个 tick，控制环（3）和统计（2）就绪，
	 * vTaskSwitchContext 选中它们，端口用信号把正卡在 poll() 里的
	 * amp_task 挂起。门铃来了再把它唤醒，延迟还是几十微秒。
	 */
	xTaskCreate(amp_task, "amp", 2048, NULL, 1, NULL);
	xTaskCreate(control_task, "ctrl", 2048, NULL, 3, NULL);
	xTaskCreate(stats_task, "stats", 2048, NULL, 2, NULL);

	vTaskStartScheduler();

	/* 调度器正常不会返回 */
	printf("调度器退出了\n");
	return 1;
}
