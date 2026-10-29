/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * amp_transport.c —— virtio 设备的内存实现
 *
 * 这个文件就是"没有板卡"这件事的全部答案。
 *
 * 一台真实的 AMP 机器上，virtio 的寄存器在 MMIO 地址上，vring 在保留内存里，
 * notify 是写 mailbox。这里：
 *
 *   寄存器 → 共享内存头部的 struct amp_ctrl 字段
 *   vring  → 共享内存里两个固定偏移的环形队列
 *   notify → ioctl 敲对侧门铃
 *
 * 对 open-amp 来说这三样没有区别，因为它只调函数指针。
 * 所以 rpmsg 的代码一行都不用改。
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <openamp/virtio.h>
#include <openamp/virtqueue.h>
#include <openamp/rpmsg_virtio.h>
#include <metal/io.h>
#include <metal/utilities.h>

#include "amp_transport.h"

static struct amp_transport *to_transport(struct virtio_device *vdev)
{
	return metal_container_of(vdev, struct amp_transport, vdev);
}

static struct amp_transport *vq_to_transport(struct virtqueue *vq)
{
	return to_transport(vq->vq_dev);
}

/*
 * 对侧有新消息了 —— 或者我们发完消息要告诉对侧。
 * 真实硬件上这里写 mailbox 寄存器，这里是 ioctl。
 */
static void amp_notify(struct virtqueue *vq)
{
	struct amp_transport *t = vq_to_transport(vq);

	t->kicks++;
	if (t->kick)
		t->kick(t);
}

/*
 * 建队列。virtio 的规矩：driver 侧负责把 vring 内存清零，
 * device 侧只读不写。这里照做，不然上一轮跑剩的描述符会串进来。
 */
static int amp_create_virtqueues(struct virtio_device *vdev, unsigned int flags,
				 unsigned int nvqs, const char *names[],
				 vq_callback callbacks[], void *callback_args[])
{
	struct amp_transport *t = to_transport(vdev);
	unsigned int i;
	int ret;

	(void)flags;
	(void)callback_args;

	if (nvqs > AMP_NUM_VRINGS)
		return -EINVAL;

	for (i = 0; i < nvqs; i++) {
		struct virtio_vring_info *vi = &t->vrings[i];

		if (vdev->role == VIRTIO_DEV_DRIVER) {
			size_t off = metal_io_virt_to_offset(&t->io, vi->info.vaddr);

			metal_io_block_set(&t->io, off, 0,
					   (size_t)vring_size(vi->info.num_descs,
							      vi->info.align));
		}

		memset(t->vqs[i], 0, AMP_VQ_BYTES);

		ret = virtqueue_create(vdev, (unsigned short)i, names[i],
				       &vi->info, callbacks[i], amp_notify,
				       t->vqs[i]);
		if (ret)
			return ret;

		vi->vq = t->vqs[i];
	}

	return 0;
}

static void amp_delete_virtqueues(struct virtio_device *vdev)
{
	(void)vdev;
}

static uint8_t amp_get_status(struct virtio_device *vdev)
{
	return to_transport(vdev)->ctrl->status;
}

static void amp_set_status(struct virtio_device *vdev, uint8_t status)
{
	to_transport(vdev)->ctrl->status = status;
}

/*
 * 只宣告一个特性：名字服务（NS）。
 * 有了它，RTOS 侧建端点时会广播自己的名字，Linux 侧才能自动绑定。
 */
static uint32_t amp_get_features(struct virtio_device *vdev)
{
	(void)vdev;
	return 1u << VIRTIO_RPMSG_F_NS;
}

static void amp_set_features(struct virtio_device *vdev, uint32_t features)
{
	to_transport(vdev)->ctrl->features = features;
}

static uint32_t amp_negotiate_features(struct virtio_device *vdev,
				       uint32_t features)
{
	return features & amp_get_features(vdev);
}

static void amp_read_config(struct virtio_device *vdev, uint32_t offset,
			    void *dst, int len)
{
	(void)vdev;
	(void)offset;
	memset(dst, 0, (size_t)len);
}

static void amp_write_config(struct virtio_device *vdev, uint32_t offset,
			     void *src, int len)
{
	(void)vdev;
	(void)offset;
	(void)src;
	(void)len;
}

static void amp_reset_device(struct virtio_device *vdev)
{
	to_transport(vdev)->ctrl->status = 0;
}

static const struct virtio_dispatch amp_virtio_ops = {
	.create_virtqueues	= amp_create_virtqueues,
	.delete_virtqueues	= amp_delete_virtqueues,
	.get_status		= amp_get_status,
	.set_status		= amp_set_status,
	.get_features		= amp_get_features,
	.set_features		= amp_set_features,
	.negotiate_features	= amp_negotiate_features,
	.read_config		= amp_read_config,
	.write_config		= amp_write_config,
	.reset_device		= amp_reset_device,
	.notify			= amp_notify,
};

/*
 * virtio 描述符里存的是"物理地址"，不是指针。真实板卡上这个地址就是共享内存的
 * 物理地址，两侧看到的是同一个数。
 *
 * 这里两个进程各自 mmap，虚拟地址不同，所以用一个单页表把"物理地址"定义成
 * 相对共享内存起点的偏移：physmap[0] = 0，页大小 4 MB（整块共享内存一页装下）。
 * 于是
 *     虚拟 → 物理：phys = 0 + offset
 *     物理 → 虚拟：virt = base + (phys - 0)
 * 两侧算出来的物理地址一致，指针各自翻译，互不干扰。
 *
 * 如果这里传 NULL，metal_io 无法做这个转换，描述符里的地址会是无效值，
 * 对侧拿到的缓冲指针会落在共享内存开头，把控制块踩掉。
 */
static metal_phys_addr_t g_physmap[1] = { 0 };
#define AMP_PAGE_SHIFT	22	/* 4 MB，正好一块共享内存 */

int amp_transport_init(struct amp_transport *t, void *shm, int role,
		       amp_kick_fn kick)
{
	unsigned int i;

	memset(t, 0, sizeof(*t));

	/*
	 * 队列要连同 desc 附加数组一起分配。上游把 vq_descx 声明成零长数组，
	 * 是因为描述符数量在运行期才知道；漏掉这块，cookie 会写到队列外面去。
	 */
	for (i = 0; i < AMP_NUM_VRINGS; i++) {
		t->vqs[i] = calloc(1, AMP_VQ_BYTES);
		if (!t->vqs[i])
			return -ENOMEM;
	}

	t->shm = shm;
	t->ctrl = (struct amp_ctrl *)shm;
	t->role = role;
	t->kick = kick;

	/*
	 * 一整块共享内存当成一个 I/O 区。page_shift 取 22，让整块内存落在
	 * physmap 的第 0 项里 —— 这样物理地址就等于相对偏移。
	 */
	metal_io_init(&t->io, shm, g_physmap, AMP_SHM_SIZE, AMP_PAGE_SHIFT, 0,
		      NULL);

	t->vdev.role = (role == AMP_ROLE_HOST) ? VIRTIO_DEV_DRIVER
					       : VIRTIO_DEV_DEVICE;
	t->vdev.id.device = VIRTIO_ID_RPMSG;
	t->vdev.id.vendor = (uint32_t)VIRTIO_DEV_ANY_ID;
	t->vdev.func = &amp_virtio_ops;
	t->vdev.vrings_info = t->vrings;
	t->vdev.vrings_num = AMP_NUM_VRINGS;

	t->vrings[0].info.vaddr = AMP_PTR(shm, AMP_VRING0_OFF);
	t->vrings[0].info.align = AMP_VRING_ALIGN;
	t->vrings[0].info.num_descs = AMP_NUM_DESCS;
	t->vrings[0].io = &t->io;

	t->vrings[1].info.vaddr = AMP_PTR(shm, AMP_VRING1_OFF);
	t->vrings[1].info.align = AMP_VRING_ALIGN;
	t->vrings[1].info.num_descs = AMP_NUM_DESCS;
	t->vrings[1].io = &t->io;

	return 0;
}
