/*
 * motor_abi.h —— 用户态和驱动之间唯一的约定
 *
 * 只有两边都按同一个布局解释这块内存，write() 和 read() 才对得上。
 * 这也是驱动和用户态程序必须一起改的原因：改一个字段，两边都得重编。
 */
#ifndef RB_MOTOR_ABI_H
#define RB_MOTOR_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 rb_u32;
typedef __s32 rb_s32;
#else
#include <stdint.h>
typedef uint32_t rb_u32;
typedef int32_t rb_s32;
#endif

/* 一条指令：第几拍 + 两个关节角，单位毫弧度（整数，跨内核边界不传浮点） */
struct motor_cmd {
	rb_u32 seq;
	rb_s32 q1_mrad;
	rb_s32 q2_mrad;
};

/* 驱动的状态：收了多少条、被打开过几次、最后一条是什么 */
struct motor_stat {
	rb_u32 writes;
	rb_u32 opens;
	rb_u32 pad;
	struct motor_cmd last;
};

#endif /* RB_MOTOR_ABI_H */
