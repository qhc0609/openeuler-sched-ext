/*
 * 第 5 篇的被观察对象：一个三段式的程序，其中一段明显最热。
 *
 * 三段都加了 noinline，否则编译器会把它们内联进 main，热点分析时
 * 只能看到一团 main，分不出谁占了多少。
 *
 * 编译：gcc -O2 -mfma -fno-tree-vectorize -o hotloop hotloop.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../common/bench.h"

#define N 8192

static float ga[N], gb[N];
static uint8_t gv[N];

/* 轻量段：约占总时间的 10% */
__attribute__((noinline)) static float phase_light(void)
{
    float s = 0.0f;

    for (int i = 0; i < N; i++)
        s += ga[i];
    return s;
}

/* 重量段：纯计算，约占总时间的 70% */
__attribute__((noinline)) static float phase_heavy(void)
{
    float s = 0.0f;

    for (int r = 0; r < 24; r++)
        for (int i = 0; i < N; i++)
            s += ga[i] * gb[i];
    return s;
}

/* 分支段：数据随机，预测器学不会，约占总时间的 20% */
__attribute__((noinline)) static uint64_t phase_branchy(void)
{
    uint64_t s = 0;

    for (int i = 0; i < N * 24; i++) {
        uint8_t x = gv[i & (N - 1)];

        if (x >= 128)
            s += x;
    }
    return s;
}

int main(int argc, char **argv)
{
    uint64_t budget_ms = (argc > 1) ? (uint64_t)atoi(argv[1]) : 5000ull;
    uint64_t t0;
    uint32_t seed = 1234u;
    double sink = 0.0;
    int iters = 0;

    for (int i = 0; i < N; i++) {
        ga[i] = (float)((i % 13) - 6) * 0.25f;
        gb[i] = (float)((i % 11) - 5) * 0.125f;
        seed = seed * 1103515245u + 12345u;
        gv[i] = (uint8_t)(seed >> 16);
    }

    t0 = bench_ns();
    while ((bench_ns() - t0) / 1000000ull < budget_ms) {
        sink += phase_light();
        sink += phase_heavy();
        sink += (double)phase_branchy();
        iters++;
    }

    printf("iters=%d sink=%.3f\n", iters, sink);
    return 0;
}
