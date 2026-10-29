#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch-freertos-tick.py —— 修 FreeRTOS 官方 POSIX 端口的一拍

官方端口用 usleep() 产生 tick：

    while( xTimerTickThreadShouldRun )
    {
        pthread_kill( thread->pthread, SIGALRM );
        usleep( portTICK_RATE_MICROSECONDS );   /* 相对睡眠 */
    }

两个毛病，都在同一行上：

  1. usleep 是"相对"睡眠，每次多睡的那部分会累积。这台机器上 1 kHz 实测只有 685 Hz。
  2. 更麻烦的是唤醒本身不准。这台虚拟机上空载的 CPU 会被挂起，定时器到期后
     要把 vCPU 叫回来，实测一次唤醒晚 0.5~1 ms，而且越睡越晚。光靠 sleep，
     1 kHz 这一拍永远落不到点上。

所以改两处：绝对时刻的 clock_nanosleep 消掉累积漂移；再提前 AMP_SPIN_NS
醒来，用自旋跨过最后一段 —— 这是嵌入式里对付"OS 定时器粒度太粗"的老办法，
代价是 tick 线程要占一点核。

只改这一个函数，别的一个字节不动。改完的代码里带 AMP_TICK_PATCH 标记，
重复执行不会改第二遍。
"""

import os
import sys

MARK = "AMP_TICK_PATCH"

OLD = """    while( xTimerTickThreadShouldRun )
    {
        /*
         * signal to the active task to cause tick handling or
         * preemption (if enabled)
         */
        Thread_t * thread = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
        pthread_kill( thread->pthread, SIGALRM );
        usleep( portTICK_RATE_MICROSECONDS );
    }
"""

NEW = """    /* AMP_TICK_PATCH: 绝对时刻 + 提前醒来自旋 */
    clock_gettime( CLOCK_MONOTONIC, &xTickGrid );

    while( xTimerTickThreadShouldRun )
    {
        struct timespec xWake;
        uint64_t ullTarget;
        long lSpinNs;
        Thread_t * thread;

        xTickGrid.tv_nsec += ( long ) portTICK_RATE_MICROSECONDS * 1000L;

        while( xTickGrid.tv_nsec >= 1000000000L )
        {
            xTickGrid.tv_nsec -= 1000000000L;
            xTickGrid.tv_sec += 1;
        }

        /*
         * 先睡到"还差 AMP_SPIN_NS 到期"，剩下的用自旋等。
         * 直接睡到点上的话，这台虚拟机的唤醒延迟有半毫秒，
         * 拍子会一直晚。
         */
        xWake = xTickGrid;
        lSpinNs = prvAmpSpinNs();

        if( xWake.tv_nsec >= lSpinNs )
        {
            xWake.tv_nsec -= lSpinNs;
        }
        else
        {
            xWake.tv_sec -= 1;
            xWake.tv_nsec += 1000000000L - lSpinNs;
        }

        clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &xWake, NULL );

        ullTarget = ( uint64_t ) xTickGrid.tv_sec * 1000000000ULL +
                    ( uint64_t ) xTickGrid.tv_nsec;
        while( prvGetTimeNs() < ullTarget )
        {
            /* 自旋窗口，正常只有几十微秒 */
        }

        /*
         * signal to the active task to cause tick handling or
         * preemption (if enabled)
         */
        thread = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
        pthread_kill( thread->pthread, SIGALRM );
    }
"""

DECL_OLD = """static void * prvTimerTickHandler( void * arg )
{
    ( void ) arg;
"""

DECL_NEW = """#include <stdlib.h>

/*
 * 提前醒来的窗口：比一个 tick 小，比唤醒延迟大。
 * 环境变量 AMP_SPIN_US 可以在运行时改（单位 us），方便扫参数：
 * 窗口越大拍子越准，但占核越多、门铃响应越慢。
 */
#ifndef AMP_SPIN_NS
    #define AMP_SPIN_NS    700000L
#endif

static long prvAmpSpinNs( void )
{
    static long lSpin = -1;

    if( lSpin < 0 )
    {
        const char * pcSpin = getenv( "AMP_SPIN_US" );

        lSpin = ( pcSpin != NULL ) ? atol( pcSpin ) * 1000L : AMP_SPIN_NS;
        if( ( lSpin < 0 ) || ( lSpin > 950000L ) )
        {
            lSpin = AMP_SPIN_NS;
        }
    }

    return lSpin;
}

static void * prvTimerTickHandler( void * arg )
{
    struct timespec xTickGrid;

    ( void ) arg;
"""


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/root/amp/FreeRTOS-Kernel"
    path = os.path.join(root, "portable/ThirdParty/GCC/Posix/port.c")

    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    if MARK in src:
        print("port.c 已打过补丁，跳过")
        return 0

    if OLD not in src:
        print("没找到预期的 prvTimerTickHandler 循环，端口版本变了？")
        return 1
    if DECL_OLD not in src:
        print("没找到 prvTimerTickHandler 的函数头")
        return 1

    src = src.replace(DECL_OLD, DECL_NEW, 1)
    src = src.replace(OLD, NEW, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(src)

    print("port.c 已打补丁：tick 改为绝对时刻 + 自旋窗口")
    return 0


if __name__ == "__main__":
    sys.exit(main())
