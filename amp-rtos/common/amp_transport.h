/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * amp_transport.h —— 把"一块共享内存"包装成 virtio 设备
 *
 * OpenAMP 的 rpmsg 层不认识"共享内存"这件事，它只认 virtio 设备：
 * 需要 virtqueue、需要 status/features 寄存器、需要一条 notify 通道。
 * 真实板卡上这些由 virtio-mmio 或 remoteproc 提供，由设备树描述地址。
 *
 * 这里我们自己在共享内存里实现同样的东西：
 *
 *   vring 0/1      固定偏移，两侧用同样的常量算地址
 *   status/features 共享内存头部的控制块，等价于 MMIO 寄存器
 *   notify()       ioctl 进内核敲对侧的门铃
 *
 * 所以这个文件两侧共用 —— 就像 virtio-mmio 的驱动代码两侧共用一样。
 */

#ifndef AMP_TRANSPORT_H
#define AMP_TRANSPORT_H

#include <openamp/virtio.h>
#include <openamp/virtqueue.h>
#include <openamp/virtio_ring.h>
#include <metal/io.h>

#include "amp_layout.h"

#define AMP_NUM_VRINGS		2
#define AMP_VRING_ALIGN		0x1000

struct amp_transport;

/* 敲对侧门铃。两侧实现不同：Linux 侧 ioctl，RTOS 侧也是 ioctl，但可以换成别的 */
typedef int (*amp_kick_fn)(struct amp_transport *t);

struct amp_transport {
	struct virtio_device vdev;			/* 必须放第一个 */
	struct virtio_vring_info vrings[AMP_NUM_VRINGS];
	/*
	 * 注意：这里存的是指针，不是数组。
	 *
	 * struct virtqueue 末尾有个零长数组 vq_descx[0]，上游的约定是调用方
	 * 额外分配 num_descs 个 struct vq_desc_extra 跟在后面。当成普通成员
	 * 声明的话，virtqueue_add_buffer 写 cookie 时会越界踩到后面的字段。
	 */
	struct virtqueue *vqs[AMP_NUM_VRINGS];
	struct metal_io_region io;			/* 覆盖整块共享内存 */
	struct amp_ctrl *ctrl;
	void *shm;
	int role;
	amp_kick_fn kick;
	unsigned long kicks;				/* 本端敲了多少次 */
};

/* 一个 virtqueue 连带它的 desc 附加数组要多少字节 */
#define AMP_VQ_BYTES \
	(sizeof(struct virtqueue) + \
	 (size_t)AMP_NUM_DESCS * sizeof(struct vq_desc_extra))


/*
 * 初始化。shm 是 mmap 得到的共享内存首地址，role 是 AMP_ROLE_HOST / AMP_ROLE_RTOS。
 * kick 为空表示不敲门铃（调试用）。
 */
int amp_transport_init(struct amp_transport *t, void *shm, int role,
		       amp_kick_fn kick);

#endif /* AMP_TRANSPORT_H */
