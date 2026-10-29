// SPDX-License-Identifier: GPL-2.0
/*
 * amp_shm.c —— 用一个内核模块，替掉真实板卡上的 mailbox 和共享内存
 *
 * AMP（非对称多处理）在板卡上需要两样硬件：
 *
 *   1. 一块两侧都能访问的共享内存（通常是 SRAM 或 DDR 的保留区）
 *   2. 一个能互相打断的 mailbox，用来通知"对侧有新消息"
 *
 * 这两样在这台普通 PC 上都没有。但这个模块可以造出来：
 *
 *   共享内存  alloc_pages_exact 预留一段物理连续的页，两个进程各 mmap 一次，
 *             拿到的就是同一份物理页。CPU 缓存是硬件维护一致的，不需要刷 cache。
 *   门铃      ioctl 进内核，内核唤醒对侧正在 poll() 的进程。这就是"投递一次中断"，
 *             只不过中断服务程序换成了内核里的 wait_queue。
 *
 * 于是 Linux 侧看到的东西和真实板卡上完全一样：一段固定地址的内存 + 一个
 * 阻塞等待的门铃。OpenAMP 的代码一行都不用改。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/io.h>

#include "amp_layout.h"

#define AMP_DEV_NAME	"amp0"

static void *g_shm;			/* 共享内存的内核虚拟地址 */
static phys_addr_t g_phys;		/* 共享内存的物理起始地址 */
static struct amp_ctrl *g_ctrl;

/* 两个角色各占一个槽位，0 = Linux，1 = RTOS */
static struct amp_file *g_files[2];

struct amp_file {
	int role;
	wait_queue_head_t wq;
	atomic_t pending;
	pid_t pid;
};

/* 对侧还没绑定时先到的门铃，不能丢 */
static bool g_early[2];

static struct amp_file *peer_of(struct amp_file *f)
{
	int other;

	if (f->role != AMP_ROLE_HOST && f->role != AMP_ROLE_RTOS)
		return NULL;

	other = (f->role == AMP_ROLE_HOST) ? AMP_ROLE_RTOS : AMP_ROLE_HOST;
	return g_files[other];
}

static int amp_open(struct inode *inode, struct file *filp)
{
	struct amp_file *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	f->role = AMP_ROLE_NONE;
	init_waitqueue_head(&f->wq);
	atomic_set(&f->pending, 0);
	f->pid = current->pid;

	filp->private_data = f;
	return 0;
}

static int amp_release(struct inode *inode, struct file *filp)
{
	struct amp_file *f = filp->private_data;
	int r;

	if (!f)
		return 0;

	r = f->role;
	if (r == AMP_ROLE_HOST || r == AMP_ROLE_RTOS) {
		if (g_files[r] == f)
			g_files[r] = NULL;
	}

	/* 对侧可能正在等，叫醒它，让它发现对端没了 */
	for (r = 0; r < 2; r++) {
		if (g_files[r])
			wake_up_interruptible(&g_files[r]->wq);
	}

	kfree(f);
	filp->private_data = NULL;
	return 0;
}

/*
 * 绑定角色。谁先谁后都行，后绑定的那个负责把先来的叫醒。
 */
static int amp_bind(struct amp_file *f, int role)
{
	if (role != AMP_ROLE_HOST && role != AMP_ROLE_RTOS)
		return -EINVAL;

	if (g_files[role] && g_files[role] != f)
		return -EBUSY;

	f->role = role;
	g_files[role] = f;

	if (role == AMP_ROLE_HOST)
		g_ctrl->host_pid = f->pid;
	else
		g_ctrl->rtos_pid = f->pid;

	/* 对侧绑定时可能已经发过门铃，补上 */
	if (g_early[role]) {
		g_early[role] = false;
		atomic_set(&f->pending, 1);
	}

	/* 通知对侧：我这边的角色到位了 */
	{
		struct amp_file *peer = peer_of(f);

		if (peer)
			wake_up_interruptible(&peer->wq);
	}

	return 0;
}

/*
 * 门铃。target 是"要叫醒谁"。
 *
 * 计数器放在共享内存里，两侧都能看到投递了多少次 —— 这是排查
 * "消息发了但对面没动"这类问题的第一手材料。
 */
static int amp_kick(struct amp_file *f, int target)
{
	struct amp_file *peer;

	if (target != AMP_ROLE_HOST && target != AMP_ROLE_RTOS)
		return -EINVAL;

	if (target == f->role)
		return -EINVAL;		/* 不能叫醒自己 */

	peer = g_files[target];
	if (!peer) {
		/* 对侧还没来，记下来，等它绑定的时候补投 */
		g_early[target] = true;
	} else {
		atomic_set(&peer->pending, 1);
		wake_up_interruptible(&peer->wq);
	}

	if (f->role == AMP_ROLE_HOST)
		g_ctrl->host_kicks++;
	else
		g_ctrl->rtos_kicks++;

	return 0;
}

static long amp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct amp_file *f = filp->private_data;
	int val;

	if (!f)
		return -EINVAL;

	switch (cmd) {
	case AMP_IOC_BIND:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			return -EFAULT;
		return amp_bind(f, val);

	case AMP_IOC_KICK:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			return -EFAULT;
		return amp_kick(f, val);

	case AMP_IOC_INFO: {
		struct amp_info info;

		memset(&info, 0, sizeof(info));
		info.phys = (uint64_t)g_phys;
		info.size = AMP_SHM_SIZE;
		info.role = f->role;
		info.peer_bound = peer_of(f) != NULL;
		info.host_kicks = g_ctrl->host_kicks;
		info.rtos_kicks = g_ctrl->rtos_kicks;

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static __poll_t amp_poll(struct file *filp, poll_table *wait)
{
	struct amp_file *f = filp->private_data;
	__poll_t mask = 0;

	if (!f)
		return EPOLLERR;

	poll_wait(filp, &f->wq, wait);

	/* 取走就清掉，和读中断状态寄存器一个意思 */
	if (atomic_xchg(&f->pending, 0))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int amp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn = g_phys >> PAGE_SHIFT;
	int ret;

	if (vma->vm_pgoff != 0)
		return -EINVAL;

	if (size > AMP_SHM_SIZE)
		return -EINVAL;

	ret = remap_pfn_range(vma, vma->vm_start, pfn + vma->vm_pgoff,
			      size, vma->vm_page_prot);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
#endif

	return 0;
}

static const struct file_operations amp_fops = {
	.owner		= THIS_MODULE,
	.open		= amp_open,
	.release	= amp_release,
	.unlocked_ioctl	= amp_ioctl,
	.poll		= amp_poll,
	.mmap		= amp_mmap,
	.llseek		= noop_llseek,
};

static struct miscdevice amp_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= AMP_DEV_NAME,
	.fops	= &amp_fops,
	.mode	= 0666,
};

static int __init amp_init(void)
{
	int ret;

	/*
	 * 物理连续的 4 MB。共享内存必须是一段连续的物理页，否则两个进程
	 * 各自 mmap 时没法保证映射到同一组页。
	 */
	g_shm = alloc_pages_exact(AMP_SHM_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!g_shm) {
		pr_err("amp_shm: 分配 %u 字节连续内存失败\n", AMP_SHM_SIZE);
		return -ENOMEM;
	}

	g_phys = virt_to_phys(g_shm);
	g_ctrl = (struct amp_ctrl *)g_shm;

	/* 布局常量写进控制块，用户态读一次就知道自己拿到的对不对 */
	g_ctrl->shm_size = AMP_SHM_SIZE;
	g_ctrl->num_descs = AMP_NUM_DESCS;
	g_ctrl->buf_size = AMP_BUF_SIZE_EACH;
	g_ctrl->version = AMP_VERSION;

	ret = misc_register(&amp_misc);
	if (ret) {
		pr_err("amp_shm: 注册设备失败 %d\n", ret);
		free_pages_exact(g_shm, AMP_SHM_SIZE);
		return ret;
	}

	pr_info("amp_shm: /dev/%s 就绪，共享内存 %u 字节 @ phys 0x%llx\n",
		AMP_DEV_NAME, AMP_SHM_SIZE, (unsigned long long)g_phys);
	return 0;
}

static void __exit amp_exit(void)
{
	misc_deregister(&amp_misc);
	if (g_shm) {
		free_pages_exact(g_shm, AMP_SHM_SIZE);
		g_shm = NULL;
	}
	pr_info("amp_shm: 已卸载\n");
}

module_init(amp_init);
module_exit(amp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OpenAMP shared memory and doorbell for single-machine AMP");
MODULE_AUTHOR("qhc0609");
