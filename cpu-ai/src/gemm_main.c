/*
 * 第 3 篇的驱动程序：同一道矩阵乘，四个版本，逐个量。
 *
 * 四个版本刻意只差一个因素，这样"快多少"能归因到具体哪一步：
 *   1 → 2  只换循环顺序
 *   2 → 3  让编译器自动向量化
 *   3 → 4  手写 AVX2
 *
 * 编译：gcc -O2 -o gemm gemm_main.c gemm_ijk.c gemm_ikj_scalar.c gemm_ikj_auto.c gemm_avx2.c -lm
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/bench.h"
#include "gemm.h"

typedef void (*gemm_fn)(const float *, const float *, float *, int);

static double time_once(gemm_fn fn, const float *a, const float *b, float *c, int n)
{
    uint64_t t0 = bench_ns();

    fn(a, b, c, n);
    return (double)(bench_ns() - t0) / 1e9;
}

static double max_err(const float *x, const float *y, size_t cnt)
{
    double e = 0.0;

    for (size_t i = 0; i < cnt; i++) {
        double d = fabs((double)x[i] - (double)y[i]);

        if (d > e)
            e = d;
    }
    return e;
}

/* 三次测量取中间值：最小那次可能踩到空载，最大那次可能被宿主打断 */
static double med3(double x, double y, double z)
{
    if (x > y) {
        double t = x;

        x = y;
        y = t;
    }
    if (y > z)
        y = z;

    return (x > y) ? x : y;
}

int main(void)
{
    static const int sizes[] = { 64, 128, 256, 512 };
    static const struct {
        const char *name;
        gemm_fn fn;
    } variants[] = {
        { "i,j,k 教科书顺序",        gemm_ijk },
        { "i,k,j 只换顺序",          gemm_ikj_scalar },
        { "i,k,j + 自动向量化",      gemm_ikj_auto },
        { "i,j,k + 手写 AVX2",       gemm_avx2 },
        { "i,k,j + 手写 AVX2",       gemm_avx2_ikj },
    };
    const int nv = (int)(sizeof(variants) / sizeof(variants[0]));
    uint32_t seed = 20261027u;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int n = sizes[s];
        size_t cnt = (size_t)n * (size_t)n;
        float *a = malloc(cnt * sizeof(float));
        float *b = malloc(cnt * sizeof(float));
        float *ref = malloc(cnt * sizeof(float));
        float *c = malloc(cnt * sizeof(float));
        double base = 0.0;

        if (!a || !b || !ref || !c) {
            perror("malloc");
            return 1;
        }

        for (size_t i = 0; i < cnt; i++) {
            seed = seed * 1103515245u + 12345u;
            a[i] = (float)((double)(seed >> 8) / 8388608.0 - 1.0);
            seed = seed * 1103515245u + 12345u;
            b[i] = (float)((double)(seed >> 8) / 8388608.0 - 1.0);
        }

        printf("\n=== n = %d （%zu 个元素，一个矩阵 %.0f KB） ===\n",
               n, cnt, (double)cnt * 4.0 / 1024.0);
        printf("%-24s %10s %10s %12s %12s\n", "版本", "GFLOPS", "相对", "最大误差", "耗时(s)");

        /* 参考值用最朴素那个版本算，后面三个都跟它比 */
        gemm_ijk(a, b, ref, n);

        for (int v = 0; v < nv; v++) {
            double t[3], med, gflops, err;

            for (int r = 0; r < 3; r++)
                t[r] = time_once(variants[v].fn, a, b, c, n);
            med = med3(t[0], t[1], t[2]);

            gflops = 2.0 * (double)n * n * n / med / 1e9;
            err = max_err(c, ref, cnt);
            if (v == 0)
                base = gflops;

            printf("%-24s %10.2f %9.1fx %12.3e %12.4f\n",
                   variants[v].name, gflops, gflops / base, err, med);
        }

        free(a);
        free(b);
        free(ref);
        free(c);
    }

    printf("\n说明：GFLOPS 按 2*n^3 次浮点运算算（一次乘加记两次）。\n");
    printf("      \"相对\"是相对同一尺寸下第一行的倍数。\n");
    printf("      误差是相对教科书版本的，三个优化版本都应该是 0 或者 1e-6 量级。\n");

    return 0;
}
