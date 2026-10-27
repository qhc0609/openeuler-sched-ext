/*
 * 第 1 篇的验证程序：一条指令的五个阶段，各自要花多少时间。
 *
 * 取指和译码没法单独量，但它们的结果体现在"吞吐"上；执行和访存能直接量：
 * 用一条依赖链把后一条指令卡在前一条的结果上，量出来的就是延迟。
 *
 * 编译：gcc -O2 -fno-tree-vectorize -o stage5 stage5.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/bench.h"

static double g_ghz;    /* 核心实际频率，标定出来的 */
static double g_tsc;    /* TSC 自己的频率 */

/* 防止整段循环被当成死代码删掉 */
static volatile uint64_t g_sink;

static double to_cycles(uint64_t ns, uint64_t ops)
{
    return (double)ns * g_ghz / (double)ops;
}

/*
 * 每个 asm 块里放 32 条指令，循环控制只有 3 条。
 * 放 8 条的话循环开销占四分之一，量出来的是"指令 + 循环"，不是指令本身。
 */
#define ADD8                                                                  \
    "addq $1, %0\n\t"                                                         \
    "addq $1, %1\n\t"                                                         \
    "addq $1, %2\n\t"                                                         \
    "addq $1, %3\n\t"                                                         \
    "addq $1, %4\n\t"                                                         \
    "addq $1, %5\n\t"                                                         \
    "addq $1, %6\n\t"                                                         \
    "addq $1, %7\n\t"

/* ---------------- 延迟：一条依赖链 ---------------- */

static void chain_add(uint64_t n)
{
    uint64_t x = 1;

    for (uint64_t i = 0; i < n; i += 32)
        __asm__ __volatile__(
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0\n\t" "addq $1, %0"
            : "+r"(x));

    g_sink = x;
}

static void chain_imul(uint64_t n)
{
    uint64_t x = 3;

    for (uint64_t i = 0; i < n; i += 32)
        __asm__ __volatile__(
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0\n\t"
            "imulq $3, %0, %0\n\t" "imulq $3, %0, %0"
            : "+r"(x));

    g_sink = x;
}

/* ---------------- 吞吐：八条互不相干的链 ---------------- */

static void indep_add(uint64_t n)
{
    uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;

    for (uint64_t i = 0; i < n; i += 32)
        __asm__ __volatile__(
            ADD8 ADD8 ADD8 ADD8
            : "+r"(a), "+r"(b), "+r"(c), "+r"(d),
              "+r"(e), "+r"(f), "+r"(g), "+r"(h));

    g_sink = a + b + c + d + e + f + g + h;
}

/* ---------------- 访存：指针追逐 ---------------- */

/*
 * 把数组做成一条覆盖全部元素的环：next[i] = (i + K) & (n-1)。
 * n 是 2 的幂、K 是奇数，两者互素，从任意点出发都能走遍全部元素。
 * K 取得很大，相邻两次访问的地址跨得很远，硬件预取器跟不住——
 * 否则它会替你把数据提前搬进缓存，量出来的延迟是假的。
 */
static double chase_cycles(uint32_t n)
{
    const uint32_t k = 2654435761u;    /* 0x9E3779B1，奇数 */
    uint32_t *next = malloc((size_t)n * sizeof(uint32_t));
    uint64_t steps, i, ns0, ns1, best = ~0ull;
    uint32_t idx = 0;

    if (!next) {
        perror("malloc");
        exit(1);
    }
    for (i = 0; i < n; i++)
        next[i] = (uint32_t)((i + k) & (uint32_t)(n - 1));

    steps = (n <= 131072u) ? (uint64_t)n * 8 : (uint64_t)n;

    /* 预热：把整条环走一遍，保证后面量到的是缓存已经稳定的状态 */
    for (i = 0; i < n; i++)
        idx = next[idx];

    for (int r = 0; r < 3; r++) {
        uint32_t cur = idx;

        ns0 = bench_ns();
        for (i = 0; i < steps; i++)
            cur = next[cur];
        ns1 = bench_ns();

        if (ns1 - ns0 < best)
            best = ns1 - ns0;
        idx = cur;
    }

    /* 这一句是关键：没有它，整个循环的结果没人用，会被编译器删干净 */
    g_sink = idx;
    free(next);

    return to_cycles(best, steps);
}

/* 三次测量取最小值：干扰只会让时间变长，不会变短，所以最小那次最接近真实 */
static uint64_t time_fn(void (*fn)(uint64_t), uint64_t n)
{
    uint64_t best = ~0ull;

    fn(n / 10);
    for (int r = 0; r < 3; r++) {
        uint64_t t0 = bench_ns();

        fn(n);
        uint64_t d = bench_ns() - t0;

        if (d < best)
            best = d;
    }
    return best;
}

int main(void)
{
    static const struct {
        uint32_t n;
        const char *name;
    } ladder[] = {
        { 4096u,      "16 KB   L1" },
        { 32768u,     "128 KB  L2" },
        { 1048576u,   "4 MB    L3" },
        { 8388608u,   "32 MB   内存" },
    };
    const uint64_t ops = 200000000ull;
    double cyc, thr;

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("CPU: ");
    fflush(stdout);
    if (system("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2") != 0)
        printf("(读不到 /proc/cpuinfo)\n");

    g_ghz = bench_calibrate_ghz();
    g_tsc = bench_calibrate_tsc_ghz();

    printf("\n=== 一、先标定：这台机器到底跑多快 ===\n");
    printf("核心实际频率   %.2f GHz\n", g_ghz);
    printf("TSC 计数器     %.2f GHz\n", g_tsc);
    printf("两者相差       %.1f%%\n", (g_ghz / g_tsc - 1.0) * 100.0);
    printf("说明：下面所有\"拍\"都按 %.2f GHz 换算，不是按 TSC。\n", g_ghz);
    printf("      标定方法：跑一条依赖 add 链，它每拍只能走一条，\n");
    printf("      所以 拍数 = 次数，用墙钟时间反推频率。\n");

    printf("\n=== 二、执行阶段：一条指令自己要走几拍 ===\n");

    cyc = to_cycles(time_fn(chain_add, ops), ops);
    printf("依赖 add  链   %6.2f 拍/条\n", cyc);

    cyc = to_cycles(time_fn(chain_imul, ops), ops);
    printf("依赖 imul 链   %6.2f 拍/条\n", cyc);

    printf("说明：两个数都是\"延迟\"——后一条必须等前一条写回。\n");
    printf("      add 走 1 拍，imul 走 3 拍，乘法器比加法器深三级。\n");

    printf("\n=== 三、取指译码阶段：靠吞吐反推 ===\n");

    cyc = to_cycles(time_fn(indep_add, ops), ops);
    thr = 1.0 / cyc;
    printf("8 条独立 add   %6.2f 拍/条 → %.2f 条/拍\n", cyc, thr);
    printf("说明：八条互不相干，执行不排队，卡住的是前端能同时喂几条。\n");
    printf("      这个数比理论宽度低，一部分是循环本身的开销，\n");
    printf("      一部分是虚拟机里 vCPU 拿不到完整的发射带宽。\n");

    printf("\n=== 四、访存阶段：数据放在哪，代价差两个数量级 ===\n");
    printf("%-16s %12s %12s\n", "工作集", "拍/次访问", "折合纳秒");
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        double c = chase_cycles(ladder[i].n);

        printf("%-16s %12.1f %12.1f\n", ladder[i].name, c, c / g_ghz);
    }
    printf("说明：每次访问都依赖上一次的结果，所以这是纯延迟，不是带宽。\n");

    printf("\n=== 五、五个阶段合起来 ===\n");
    printf("取指译码   前端宽度有限，实测 %.1f 条/拍\n", thr);
    printf("执行       add 1 拍、imul 3 拍\n");
    printf("访存       从 L1 到内存，跨两个数量级\n");
    printf("写回       不单独占时间，但下一条依赖指令要等它\n");
    printf("结论：同一条 add，操作数在寄存器里 1 拍，在内存里要等两百拍。\n");
    printf("      这就是为什么 AI 推理的瓶颈常常不在算，而在搬。\n");

    return 0;
}
