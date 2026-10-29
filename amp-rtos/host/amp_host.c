/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * amp_host.c —— Linux 侧（OpenAMP 里的 host / virtio driver）
 *
 * 这个程序干的事和真实板卡上 Linux 侧一模一样：
 *
 *   1. 打开 /dev/amp0，声明自己是 host，mmap 共享内存
 *   2. 初始化控制块，把 virtio 的 status 推到 DRIVER_OK
 *   3. 调 rpmsg_init_vdev() —— 上游 open-amp 的原样代码
 *   4. 收到名字服务广播后建端点，然后发消息、收回复、算往返时间
 *
 * 唯一"非标准"的地方是 notify 和等待：真实硬件写 mailbox 寄存器并等中断，
 * 这里 ioctl 敲对侧门铃、poll 等门铃。
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

#include <openamp/rpmsg_virtio.h>

#include "amp_layout.h"
#include "amp_transport.h"

#define MSG_COUNT	2000

static int g_fd = -1;
static struct amp_ctrl *g_ctrl;
static void *g_shm;

static struct amp_transport g_t;
static struct rpmsg_virtio_device g_rvdev;
static struct rpmsg_endpoint g_ept;
static int g_ept_ready;

/* 往返计时 */
static long long g_t_send;
static long long g_rtt[MSG_COUNT];
static int g_sent, g_recv;
static int g_in_flight;
static int g_lost;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		  uint32_t src, void *priv);

/* notify：敲 RTOS 的门铃 */
static int host_kick(struct amp_transport *t)
{
	int target = AMP_ROLE_RTOS;

	(void)t;
	return ioctl(g_fd, AMP_IOC_KICK, &target);
}

/*
 * 名字服务回调。
 *
 * RTOS 侧建端点时会广播 "我这儿有个叫 rpmsg-echo 的端点，地址是 X"。
 * open-amp 解析出这条广播后调到这里，我们就在本地建一个对端连过去。
 */
static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
	printf("  [host] 名字服务广播：name=\"%s\" addr=%u\n", name, dest);

	if (strcmp(name, "rpmsg-echo") != 0)
		return;

	if (rpmsg_create_ept(&g_ept, rdev, name, RPMSG_ADDR_ANY, dest,
			     ept_cb, NULL) != 0) {
		printf("  [host] 建端点失败\n");
		return;
	}

	g_ept_ready = 1;
	printf("  [host] 端点已建立：本地 addr=%u，对端 addr=%u\n",
	       g_ept.addr, g_ept.dest_addr);
}

/* 收到 RTOS 回的消息 */
static int ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		  uint32_t src, void *priv)
{
	long long t1 = now_ns();

	(void)ept;
	(void)data;
	(void)len;
	(void)src;
	(void)priv;

	if (g_in_flight) {
		g_rtt[g_recv] = t1 - g_t_send;
		g_recv++;
		g_in_flight = 0;
	} else {
		g_lost++;
	}

	return 0;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	struct rpmsg_virtio_shm_pool shpool;
	struct pollfd pfd;
	int count = (argc > 1) ? atoi(argv[1]) : MSG_COUNT;
	int i, timeout_ms = 0;
	long long sum = 0, t_start = 0, t_end = 0;

	if (count <= 0 || count > MSG_COUNT)
		count = MSG_COUNT;

	/* 输出重定向到文件时默认是全缓冲，进程被杀就什么都看不到 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("== 一、打开共享内存设备，声明自己是 host ==\n");
	g_fd = open("/dev/amp0", O_RDWR);
	if (g_fd < 0) {
		perror("open /dev/amp0");
		return 1;
	}

	i = AMP_ROLE_HOST;
	if (ioctl(g_fd, AMP_IOC_BIND, &i) < 0) {
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

	/* 控制块由 host 初始化，相当于真实硬件上 host 复位 virtio 设备 */
	memset(g_ctrl, 0, sizeof(*g_ctrl));
	g_ctrl->magic = AMP_MAGIC;
	g_ctrl->version = AMP_VERSION;
	g_ctrl->shm_size = AMP_SHM_SIZE;
	g_ctrl->num_descs = AMP_NUM_DESCS;
	g_ctrl->buf_size = AMP_BUF_SIZE_EACH;
	printf("  共享内存 %u 字节，%u 个描述符，每条消息 %u 字节\n",
	       AMP_SHM_SIZE, AMP_NUM_DESCS, AMP_BUF_SIZE_EACH);

	printf("\n== 二、把共享内存包装成 virtio 设备 ==\n");
	amp_transport_init(&g_t, g_shm, AMP_ROLE_HOST, host_kick);

	printf("\n== 三、rpmsg_init_vdev()：上游 open-amp 原样调用 ==\n");
	rpmsg_virtio_init_shm_pool(&shpool, AMP_PTR(g_shm, AMP_BUF_OFF),
				   AMP_BUF_SIZE);

	if (rpmsg_init_vdev(&g_rvdev, &g_t.vdev, ns_bind_cb, &g_t.io,
			    &shpool) != 0) {
		printf("  rpmsg_init_vdev 失败\n");
		return 1;
	}
	printf("  status=0x%02x（DRIVER_OK），RX 队列已投递 %u 个空缓冲\n",
	       g_ctrl->status, g_rvdev.rvq->vq_nentries);

	printf("\n== 四、等 RTOS 广播端点，然后发 %d 条消息 ==\n", count);

	pfd.fd = g_fd;
	pfd.events = POLLIN;

	t_start = now_ns();

	while (g_recv < count) {
		/* 有端点且上一条已回，就发下一条 */
		if (g_ept_ready && !g_in_flight && g_sent < count) {
			char buf[64];
			int n = snprintf(buf, sizeof(buf), "ping %d", g_sent);
			int ret;

			g_t_send = now_ns();
			/*
			 * 注意：rpmsg_send() 成功时返回的是发出的字节数，不是 0。
			 * 上游 rpmsg_virtio_send_offchannel_nocopy() 最后一行是
			 *     return len;
			 * 所以这里必须判 > 0。判 == 0 会把"发成功了"当成失败。
			 */
			ret = rpmsg_send(&g_ept, buf, (size_t)n + 1);
			if (ret > 0) {
				g_in_flight = 1;
				g_sent++;
			} else {
				printf("  rpmsg_send 失败 %d\n", ret);
				break;
			}
		}

		/*
		 * 等门铃。真实硬件上是等中断，这里是 poll。
		 * 门铃响了就调 virtqueue_notification()，等价于中断处理里
		 * 读 vring 的 used 环 —— 这也是上游 open-amp 的入口。
		 */
		if (poll(&pfd, 1, 20) > 0)
			virtqueue_notification(g_rvdev.rvq);

		if (g_ept_ready && g_in_flight) {
			if (++timeout_ms > 500) {
				printf("  第 %d 条等了 10 秒没回，停\n", g_sent);
				break;
			}
		} else {
			timeout_ms = 0;
		}

		/* 10 秒还没有任何进展就退出，免得脚本卡死 */
		if (now_ns() - t_start > 10000000000LL) {
			printf("  10 秒无进展，退出（端点%s就绪）\n",
			       g_ept_ready ? "已" : "未");
			printf("  magic=0x%08x rtos_ready=%u | rvq used.idx=%u "
			       "used_cons=%u | rtos_kicks=%u\n",
			       g_ctrl->magic, g_ctrl->rtos_ready,
			       g_rvdev.rvq->vq_ring.used->idx,
			       g_rvdev.rvq->vq_used_cons_idx,
			       g_ctrl->rtos_kicks);
			break;
		}
	}

	t_end = now_ns();

	if (g_recv == 0) {
		printf("  一条都没回来，退出\n");
		return 1;
	}

	printf("\n== 五、往返时间 ==\n");
	qsort(g_rtt, (size_t)g_recv, sizeof(long long), cmp_ll);
	for (i = 0; i < g_recv; i++)
		sum += g_rtt[i];

	printf("  发送 %d 条，收回 %d 条\n", g_sent, g_recv);
	printf("  最小 %lld ns\n", g_rtt[0]);
	printf("  中位 %lld ns\n", g_rtt[g_recv / 2]);
	printf("  p90  %lld ns\n", g_rtt[(int)((long long)g_recv * 90 / 100)]);
	printf("  p99  %lld ns\n", g_rtt[(int)((long long)g_recv * 99 / 100)]);
	printf("  最大 %lld ns\n", g_rtt[g_recv - 1]);
	printf("  平均 %lld ns\n", sum / g_recv);
	printf("  单向估算 %lld ns（往返减半，含两次门铃和两次调度）\n",
	       g_rtt[g_recv / 2] / 2);
	printf("  一问一答 %.0f 条/秒（严格的发一条等一条，不是流水线）\n",
	       (double)g_recv * 1e9 / (double)(t_end - t_start));

	printf("\n== 六、门铃计数 ==\n");
	printf("  Linux->RTOS %u 次，RTOS->Linux %u 次\n",
	       g_ctrl->host_kicks, g_ctrl->rtos_kicks);
	printf("  本端 notify 被调 %lu 次\n", g_t.kicks);
	printf("  RTOS tick 频率 %u Hz\n", g_ctrl->rtos_tick_hz);

	/*
	 * RTOS 侧的 1 kHz 控制环还在跑。隔两秒读两次它的 tick 计数，
	 * 用墙钟算实际频率 —— 这个数说明"消息收发"有没有把控制环带慢。
	 */
	{
		struct amp_rt_state *rt =
			(struct amp_rt_state *)AMP_PTR(g_shm, AMP_RT_STATE_OFF);
		amp_u32 t0, t1;
		amp_u32 h0[AMP_JIT_BUCKETS];
		long long w0, w1;
		amp_u32 total = 0;
		int b;

		sleep(2);
		t0 = rt->tick_count;
		w0 = now_ns();
		for (b = 0; b < AMP_JIT_BUCKETS; b++)
			h0[b] = rt->hist[b];
		sleep(2);
		t1 = rt->tick_count;
		w1 = now_ns();

		printf("\n== 七、RTOS 侧控制环（从共享内存里读）==\n");
		printf("  实测 %.1f Hz（设定 %u Hz），两秒内走了 %u 拍\n",
		       (double)(t1 - t0) * 1e9 / (double)(w1 - w0),
		       g_ctrl->rtos_tick_hz, t1 - t0);
		printf("  累计 %u 拍，迟到超过半拍 %u 次（%.1f%%）\n",
		       t1, rt->tick_late,
		       100.0 * (double)rt->tick_late / (double)t1);

		for (b = 0; b < AMP_JIT_BUCKETS; b++)
			total += rt->hist[b] - h0[b];

		printf("  最近两秒 %u 拍的抖动分布：\n", total);
		for (b = 0; b < AMP_JIT_BUCKETS; b++) {
			amp_u32 n = rt->hist[b] - h0[b];

			if (b < AMP_JIT_BUCKETS - 1)
				printf("    < %6.0f us  %6u  %5.1f%%\n",
				       amp_jit_edge_ns[b] / 1000.0, n,
				       total ? 100.0 * (double)n / (double)total : 0.0);
			else
				printf("    >= %5.0f us  %6u  %5.1f%%\n",
				       amp_jit_edge_ns[b - 1] / 1000.0, n,
				       total ? 100.0 * (double)n / (double)total : 0.0);
		}
		printf("  历史最大偏差 %.1f us，最近一拍 %.1f us\n",
		       rt->max_jitter_ns / 1000.0f,
		       rt->last_jitter_ns / 1000.0f);
		printf("  RTOS 回显 %u 条\n", rt->echo_count);
	}

	munmap(g_shm, AMP_SHM_SIZE);
	close(g_fd);
	return 0;
}
