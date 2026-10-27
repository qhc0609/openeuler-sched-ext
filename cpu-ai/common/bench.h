/*
 * 微基准的公共部分：读时间、读周期、标定主频、取中位数。
 *
 * 这台虚拟机上没有可用的 PMU（cycles / instructions / branches 全部返回
 * <not supported>），所以所有"周期"数都是这样得到的：先用一条已知延迟的
 * 指令链量出真实主频，再把墙钟时间换算成周期。
 *
 * 这一步不能省。TSC 走的是标称频率，核心在睿频时跑得更快，差一成很常见。
 * 直接把 TSC 差当成周期数，结论会系统性偏小。
 */
#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__x86_64__)
static inline uint64_t bench_ticks(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t bench_ticks(void)
{
    uint64_t v;

    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
#error "bench.h 只支持 x86_64 和 aarch64"
#endif

static inline uint64_t bench_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline int bench_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static inline int bench_cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x > y) - (x < y);
}

/* 取中位数：微基准里最小值太乐观（偶尔踩到空载），平均值被宿主抖动拉高 */
static inline uint64_t bench_median(uint64_t *v, int n)
{
    qsort(v, (size_t)n, sizeof(uint64_t), bench_cmp_u64);
    return v[n / 2];
}

/*
 * 依赖 add 链：后一条必须等前一条写回才能读同一个寄存器，所以每拍只走一条。
 * n 次 add 就是 n 拍，用它反推主频：GHz = 拍数 / 纳秒数。
 */
static inline void bench_chain_add(uint64_t n)
{
    uint64_t x = 1;

    for (uint64_t i = 0; i < n; i += 8)
        __asm__ __volatile__(
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0\n\t"
            "addq $1, %0"
            : "+r"(x));

    /* 让编译器相信 x 有被用到，否则整条链会被删掉 */
    if (x == 0)
        putchar('\n');
}

/* 返回核心实际频率（GHz）。TSC 频率另算，两者不是一回事。 */
static inline double bench_calibrate_ghz(void)
{
    const uint64_t n = 200000000ull;
    uint64_t ns0, ns1;

    bench_chain_add(n / 10);
    ns0 = bench_ns();
    bench_chain_add(n);
    ns1 = bench_ns();

    return (double)n / (double)(ns1 - ns0);
}

/* 返回 TSC 自己的频率（GHz），用来展示"周期计数器并不等于核心周期" */
static inline double bench_calibrate_tsc_ghz(void)
{
    uint64_t t0, t1, ns0, ns1;

    t0 = bench_ticks();
    ns0 = bench_ns();
    bench_chain_add(50000000ull);
    ns1 = bench_ns();
    t1 = bench_ticks();

    return (double)(t1 - t0) / (double)(ns1 - ns0);
}

#endif
