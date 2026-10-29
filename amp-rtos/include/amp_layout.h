/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * amp_layout.h —— Linux 与 RTOS 共同遵守的内存布局
 *
 * 一块 4 MB 的共享内存，由内核模块 amp_shm.ko 用 alloc_pages_exact 预留，
 * 两侧各自 mmap 同一段物理页，因此看到的是同一份字节。
 *
 * 布局是固定的，不靠协商：
 *
 *   0x000000  4 KB    控制块（状态、特性、vring 位置、门铃计数）
 *   0x001000  64 KB   vring 0 —— Linux 发、RTOS 收
 *   0x011000  64 KB   vring 1 —— RTOS 发、Linux 收
 *   0x021000  余下    rpmsg 数据缓冲池
 *
 * 真实板卡上这些地址来自设备树，这里来自内核模块，地址一样是常量。
 */

#ifndef AMP_LAYOUT_H
#define AMP_LAYOUT_H

/*
 * 这个头文件两边都要用：用户态（Linux 侧程序、RTOS 侧程序）和内核态（模块）。
 * 内核里没有 stdint.h，所以宽度靠条件编译各取一套，保证结构体布局一致。
 */
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 amp_u32;
typedef u64 amp_u64;
#else
#include <stdint.h>
typedef uint32_t amp_u32;
typedef uint64_t amp_u64;
#endif

#define AMP_SHM_SIZE		(4u * 1024u * 1024u)

#define AMP_CTRL_OFF		0x000000u
#define AMP_CTRL_SIZE		0x001000u

#define AMP_VRING0_OFF		0x001000u
#define AMP_VRING1_OFF		0x011000u
#define AMP_VRING_SIZE		0x010000u

#define AMP_BUF_OFF		0x021000u
#define AMP_BUF_SIZE		(AMP_SHM_SIZE - AMP_BUF_OFF)

/* 描述符数量。256 是 OpenAMP 的惯用值，一个描述符 16 字节。 */
#define AMP_NUM_DESCS		256u
/* 每个 rpmsg 消息最多装多少字节。512 是 OpenAMP 的默认值。 */
#define AMP_BUF_SIZE_EACH	512u

/* 控制块里 magic 的值，用来判断对侧有没有初始化过。 */
#define AMP_MAGIC		0x414D5043u	/* "AMPC" */
#define AMP_VERSION		1u

/* 角色。0 固定给 Linux，1 固定给 RTOS。 */
#define AMP_ROLE_HOST		0
#define AMP_ROLE_RTOS		1
#define AMP_ROLE_NONE		(-1)

/*
 * 控制块。放在共享内存最前面，两侧都直接读写。
 *
 * status / features 是 virtio 的语义：host 写 status 表示"我准备好了"，
 * RTOS 轮询到 DRIVER_OK 才开始收消息。真实硬件上这两个字段在 MMIO 寄存器里，
 * 这里在内存里，行为完全一样。
 */
struct amp_ctrl {
	amp_u32 magic;			/* AMP_MAGIC */
	amp_u32 version;		/* AMP_VERSION */
	amp_u32 shm_size;		/* 共享内存总大小 */
	amp_u32 num_descs;		/* 每个 vring 的描述符数 */
	amp_u32 buf_size;		/* 每条消息的缓冲大小 */

	amp_u32 status;		/* virtio device status */
	amp_u32 features;		/* 协商后的特性位 */
	amp_u32 rtos_ready;		/* RTOS 侧初始化完成 */

	/* 门铃计数，由内核模块维护，用户态只读 */
	amp_u32 host_kicks;		/* Linux -> RTOS 次数 */
	amp_u32 rtos_kicks;		/* RTOS -> Linux 次数 */

	amp_u32 rtos_tick_hz;		/* RTOS 的 tick 频率，Linux 侧读来显示 */
	amp_u32 host_pid;
	amp_u32 rtos_pid;
	amp_u32 pad[2];
};

/*
 * 门铃 ioctl。
 *
 * 真实板卡上是写 mailbox 寄存器触发对侧中断；这里是 ioctl 进内核，
 * 内核唤醒对侧正在 poll() 的进程。语义一样：投递一次"有事了"。
 */
#define AMP_IOC_MAGIC		'A'
#define AMP_IOC_BIND		_IOW(AMP_IOC_MAGIC, 1, int)
#define AMP_IOC_KICK		_IOW(AMP_IOC_MAGIC, 2, int)
#define AMP_IOC_INFO		_IOR(AMP_IOC_MAGIC, 3, struct amp_info)

struct amp_info {
	amp_u64 phys;			/* 共享内存物理起始地址 */
	amp_u64 size;			/* 大小 */
	amp_u32 role;			/* 本端角色 */
	amp_u32 peer_bound;		/* 对侧是否已绑定 */
	amp_u32 host_kicks;
	amp_u32 rtos_kicks;
};

/*
 * RTOS 侧往这里写运行状态，Linux 侧直接读 —— 不经过 rpmsg。
 *
 * 这一小块是"控制面"，用来演示大脑随时能看见小脑的心跳和抖动，
 * 不必为每次查询付一次消息往返。
 */
#define AMP_RT_STATE_OFF	(AMP_CTRL_OFF + 0x100u)

/*
 * 抖动直方图的分档，单位纳秒。
 * 只看最大值没用 —— 一次 14 ms 的毛刺可能只是虚拟机被宿主机挂起了一下，
 * 说明不了常态。要看的是"多少拍落在 50 us 以内"。
 */
#define AMP_JIT_BUCKETS		6
#ifndef __KERNEL__
static const amp_u32 amp_jit_edge_ns[AMP_JIT_BUCKETS] = {
	50000u, 100000u, 200000u, 500000u, 1000000u, 0u
};
#endif

struct amp_rt_state {
	amp_u32 tick_count;		/* 控制任务跑了多少拍 */
	amp_u32 tick_late;		/* 有多少拍晚于 1.5 个周期 */
	amp_u32 last_jitter_ns;		/* 最近一拍的偏差 */
	amp_u32 max_jitter_ns;		/* 最大偏差 */
	amp_u32 echo_count;		/* 回显了多少条 */
	amp_u32 seq;			/* 写入序号，读侧用来判断有没有更新 */
	float setpoint;			/* 小脑算出来的设定值 */
	float feedback;			/* 上一拍的实际值 */
	amp_u32 hist[AMP_JIT_BUCKETS];	/* 抖动落在各档的拍数 */
	amp_u32 pad[2];
};

/* 把偏移换算成指针 */
#define AMP_PTR(base, off)	((void *)((char *)(base) + (off)))

#endif /* AMP_LAYOUT_H */
