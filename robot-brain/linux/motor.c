/*
 * motor.c —— 一个最小的电机驱动，只为把"用户态 → 系统调用 → 驱动"这条链
 * 摆到台面上，不控制任何真实硬件。
 *
 * 它做三件事：
 *   1. 注册一个字符设备 /dev/robotmotor
 *   2. write() 收一条指令，存下来，记数
 *   3. read() 把"收到了几条、最后一条是什么"交回用户态
 *
 * 为什么不直接在用户态写寄存器：
 *   x86 上用户态碰不到设备的 MMIO 和 IO 端口（ring 3 没有那个权限），
 *   必须有一次从 ring 3 到 ring 0 的切换。这一步就是系统调用。
 *
 * 编译：make -C /lib/modules/$(uname -r)/build M=$PWD modules
 * 加载：insmod motor.ko
 */
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "motor_abi.h"

static dev_t g_dev;
static struct cdev g_cdev;
static struct class *g_cls;

static struct motor_cmd g_last;
static atomic_t g_writes = ATOMIC_INIT(0);
static atomic_t g_opens = ATOMIC_INIT(0);

static int motor_open(struct inode *inode, struct file *filp)
{
	atomic_inc(&g_opens);
	return 0;
}

/*
 * 用户态每毫秒调一次这个函数。它只做两件事：把指令拷进来、记数。
 * 真实驱动在这里还会去写寄存器、或者塞进 DMA 队列。
 */
static ssize_t motor_write(struct file *filp, const char __user *buf,
			   size_t len, loff_t *off)
{
	struct motor_cmd cmd;

	if (len != sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	g_last = cmd;
	atomic_inc(&g_writes);

	return len;
}

static ssize_t motor_read(struct file *filp, char __user *buf,
			  size_t len, loff_t *off)
{
	struct motor_stat st;

	if (*off > 0)
		return 0;

	st.writes = atomic_read(&g_writes);
	st.opens = atomic_read(&g_opens);
	st.last = g_last;

	if (len < sizeof(st))
		return -EINVAL;
	if (copy_to_user(buf, &st, sizeof(st)))
		return -EFAULT;

	*off += sizeof(st);
	return sizeof(st);
}

static const struct file_operations motor_fops = {
	.owner = THIS_MODULE,
	.open = motor_open,
	.read = motor_read,
	.write = motor_write,
};

static int __init motor_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&g_dev, 0, 1, "robotmotor");
	if (ret)
		return ret;

	cdev_init(&g_cdev, &motor_fops);
	g_cdev.owner = THIS_MODULE;
	ret = cdev_add(&g_cdev, g_dev, 1);
	if (ret)
		return ret;

	g_cls = class_create("robotmotor");
	if (IS_ERR(g_cls))
		return PTR_ERR(g_cls);

	if (IS_ERR(device_create(g_cls, NULL, g_dev, NULL, "robotmotor")))
		return -ENODEV;

	pr_info("robotmotor: /dev/robotmotor 就绪，主设备号 %u\n", MAJOR(g_dev));
	return 0;
}

static void __exit motor_exit(void)
{
	device_destroy(g_cls, g_dev);
	class_destroy(g_cls);
	cdev_del(&g_cdev);
	unregister_chrdev_region(g_dev, 1);
	pr_info("robotmotor: 卸载\n");
}

module_init(motor_init);
module_exit(motor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("robot-brain: minimal motor device for the Linux article");
