/*
 * 第 4 篇的验证程序：为什么主频很高，跑起来还是慢。
 *
 * 三个实验，对应三种让 CPU "等"的原因：
 *   一、分支预测错了 —— 流水线要清空重来
 *   二、指令之间有依赖 —— 计算单元空着没人用
 *   三、数据不在缓存里 —— 计算单元等着内存
 *
 * 注意：这个文件必须关掉 if-conversion（-fno-if-conversion），
 * 否则 GCC 会把 if 变成 cmov，没有分支可预测，第一个实验就没了。
 *
 * 编译：gcc -O2 -mfma -fno-tree-vectorize -fno-if-conversion -fno-if-conversion2 -o bottleneck bottleneck.c
 */
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/bench.h"

static double g_ghz;

static int cmp_u8(const void *a, const void *b)
{
    return (int)*(const uint8_t *)a - (int)*(const uint8_t *)b;
}

/* ================= 一、分支预测 ================= */

static uint64_t branch_sum(const uint8_t *v, int n, int *out_cnt)
{
    uint64_t s = 0;
    int c = 0;

    for (int i = 0; i < n; i++)
        if (v[i] >= 128) {
            s += v[i];
            c++;
        }
    *out_cnt = c;
    return s;
}

static void test_branch(void)
{
    const int n = 32768;            /* 32 KB，整个数组在 L1 里 */
    const int reps = 400;
    uint8_t *raw = malloc((size_t)n);
    uint8_t *sorted = malloc((size_t)n);
    uint32_t seed = 7u;
    int c1 = 0, c2 = 0;
    uint64_t ns0, ns1, total, sink = 0;
    uint64_t s1, s2;
    double cyc_rand, cyc_sorted;

    if (!raw || !sorted) {
        perror("malloc");
        exit(1);
    }
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        raw[i] = (uint8_t)(seed >> 16);
    }
    memcpy(sorted, raw, (size_t)n);
    qsort(sorted, (size_t)n, 1, cmp_u8);

    /* 两个版本必须算出同一个数，否则比的是两件事。先在计时前算好。 */
    s1 = branch_sum(raw, n, &c1);
    s2 = branch_sum(sorted, n, &c2);

    total = (uint64_t)reps * (uint64_t)n;

    branch_sum(raw, n, &c1);
    ns0 = bench_ns();
    for (int r = 0; r < reps; r++)
        sink += branch_sum(raw, n, &c1);
    ns1 = bench_ns();
    cyc_rand = (double)(ns1 - ns0) * g_ghz / (double)total;

    branch_sum(sorted, n, &c2);
    ns0 = bench_ns();
    for (int r = 0; r < reps; r++)
        sink += branch_sum(sorted, n, &c2);
    ns1 = bench_ns();
    cyc_sorted = (double)(ns1 - ns0) * g_ghz / (double)total;

    printf("=== 一、分支预测：同样的数据，只换顺序 ===\n");
    printf("数组 %d 字节（全在 L1），数据完全一样，只有排列不同\n", n);
    printf("校验：乱序和 %llu，顺序和 %llu，命中数 %d / %d\n",
           (unsigned long long)s1, (unsigned long long)s2, c1, c2);
    printf("%-16s %14s %14s\n", "数据顺序", "拍/元素", "相对");
    printf("%-16s %14.2f %13.1fx\n", "随机排列", cyc_rand, 1.0);
    printf("%-16s %14.2f %13.1fx\n", "升序排列", cyc_sorted, cyc_rand / cyc_sorted);
    printf("说明：升序之后分支几乎全部同向，预测器学会了；乱序时一半对一半，\n");
    printf("      每次猜错都要清空流水线。差出来的这些拍全是白等的。\n\n");

    if (sink == 0)
        putchar('\n');
    free(raw);
    free(sorted);
}

/* ================= 二、指令级并行 ================= */

/*
 * 同一个点积，累加器从 1 个加到 8 个。
 *
 * 每个累加器必须写成独立的变量。写成 float acc[8] 的话，GCC 在
 * -fno-tree-vectorize 下不会把这个数组展开到寄存器，而是留在栈上，
 * 每做一次加法都要读写内存，量出来的是内存延迟，不是 FMA 的延迟。
 */
static float dot1(const float *a, const float *b, int n)
{
    float s0 = 0.0f;

    for (int i = 0; i < n; i++)
        s0 += a[i] * b[i];
    return s0;
}

static float dot2(const float *a, const float *b, int n)
{
    float s0 = 0.0f, s1 = 0.0f;
    int i = 0;

    for (; i + 2 <= n; i += 2) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
    }
    for (; i < n; i++)
        s0 += a[i] * b[i];
    return s0 + s1;
}

static float dot4(const float *a, const float *b, int n)
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;

    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++)
        s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

static float dot8(const float *a, const float *b, int n)
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    int i = 0;

    for (; i + 8 <= n; i += 8) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
        s4 += a[i + 4] * b[i + 4];
        s5 += a[i + 5] * b[i + 5];
        s6 += a[i + 6] * b[i + 6];
        s7 += a[i + 7] * b[i + 7];
    }
    for (; i < n; i++)
        s0 += a[i] * b[i];
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}

static void test_ilp(void)
{
    const int n = 2048;             /* 8 KB 一个数组，两个共 16 KB，稳在 L1 */
    const int reps = 50000;
    static float a[2048], b[2048];
    const struct {
        const char *name;
        float (*fn)(const float *, const float *, int);
    } v[] = {
        { "1 个累加器", dot1 },
        { "2 个累加器", dot2 },
        { "4 个累加器", dot4 },
        { "8 个累加器", dot8 },
    };
    double base = 0.0;

    for (int i = 0; i < n; i++) {
        a[i] = (float)((i % 13) - 6) * 0.25f;
        b[i] = (float)((i % 11) - 5) * 0.125f;
    }

    printf("=== 二、指令级并行：同一个点积，只改累加器个数 ===\n");
    printf("数据 %d 个 float（两个数组共 16 KB，稳在 L1），纯计算\n\n", n);
    printf("%-16s %12s %12s %12s\n", "累加器", "GMAC/s", "MAC/拍", "相对");

    for (size_t k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
        double best = 1e9;
        volatile float ck = 0.0f;

        v[k].fn(a, b, n);
        for (int r = 0; r < 3; r++) {
            uint64_t ns0 = bench_ns();

            for (int t = 0; t < reps; t++)
                ck += v[k].fn(a, b, n);
            double s = (double)(bench_ns() - ns0) / 1e9;

            if (s < best)
                best = s;
        }
        double gmacs = (double)reps * n / best / 1e9;

        if (k == 0)
            base = gmacs;
        printf("%-16s %12.2f %12.2f %11.1fx\n", v[k].name, gmacs, gmacs / g_ghz,
               gmacs / base);
        if (ck == 1.0f)
            putchar('\n');
    }
    printf("\n说明：数据一样、指令一样，只是把一个长依赖拆成几个短的。\n");
    printf("      1 个累加器时每 4 拍才做完一次乘加，另外 3 拍计算单元是空的。\n");
    printf("      拆到 4 个就接近 1 拍 1 次，再往上加几乎不动——\n");
    printf("      瓶颈已经从\"等结果\"换成了\"等数据\"：每拍只有两次装载，\n");
    printf("      而一次乘加要读两个数。想再快，只能让一次装载喂给多次乘加\n");
    printf("      （矩阵乘里广播 A 就是这个道理）。\n\n");
}

/* ================= 三、缓存容量 ================= */

/*
 * 这里必须用向量指令，否则撞不到带宽墙。
 *
 * 标量版每个元素要读两个数（A 和 x），而每拍只有两次装载，
 * 所以标量版的天花板是每拍 1 个元素 = 4 字节，约 10 GB/s，
 * 不管数据在 L1 还是在内存，都是这个数——测不出缓存的影响。
 *
 * 换成 AVX2 之后一次装载 32 字节，天花板抬到几十 GB/s，
 * 数据一旦出了缓存，带宽才成为真正的限制。
 */
static void matvec_avx2(const float *A, const float *x, float *y, int n)
{
    for (int i = 0; i < n; i++) {
        const float *row = A + (size_t)i * n;
        __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
        __m128 lo, hi, t;
        float acc;
        int j = 0;

        for (; j + 16 <= n; j += 16) {
            s0 = _mm256_fmadd_ps(_mm256_loadu_ps(row + j), _mm256_loadu_ps(x + j), s0);
            s1 = _mm256_fmadd_ps(_mm256_loadu_ps(row + j + 8), _mm256_loadu_ps(x + j + 8), s1);
        }

        __m256 s = _mm256_add_ps(s0, s1);

        lo = _mm256_castps256_ps128(s);
        hi = _mm256_extractf128_ps(s, 1);
        t = _mm_add_ps(lo, hi);
        t = _mm_hadd_ps(t, t);
        t = _mm_hadd_ps(t, t);
        acc = _mm_cvtss_f32(t);

        for (; j < n; j++)
            acc += row[j] * x[j];
        y[i] = acc;
    }
}

static void test_cache(void)
{
    static const int dims[] = { 64, 256, 1024, 4096 };
    static const char *tags[] = { "16 KB   L1", "256 KB  L2", "4 MB    L3", "64 MB   内存" };

    printf("=== 三、缓存容量：矩阵多大，带宽掉到哪 ===\n");
    printf("y = A*x，A 是 n×n 的 float 矩阵，每一行都完整读一遍\n\n");
    printf("%-14s %8s %12s %14s\n", "工作集", "n", "GB/s", "相对最快");

    double first = 0.0;

    for (size_t k = 0; k < sizeof(dims) / sizeof(dims[0]); k++) {
        int n = dims[k];
        size_t cnt = (size_t)n * (size_t)n;
        float *A = malloc(cnt * sizeof(float));
        float *x = malloc((size_t)n * sizeof(float));
        float *y = malloc((size_t)n * sizeof(float));
        double best = 1e9;

        if (!A || !x || !y) {
            perror("malloc");
            exit(1);
        }
        for (size_t i = 0; i < cnt; i++)
            A[i] = (float)((int)(i % 17) - 8) * 0.125f;
        for (int i = 0; i < n; i++)
            x[i] = (float)(i % 5) * 0.25f;

        /* 跑三次取快的那次：第一遍还在把数据搬进缓存 */
        for (int r = 0; r < 3; r++) {
            uint64_t ns0 = bench_ns();

            matvec_avx2(A, x, y, n);
            double sec = (double)(bench_ns() - ns0) / 1e9;

            if (sec < best)
                best = sec;
        }

        double gbs = (double)cnt * 4.0 / best / 1e9;

        if (k == 0)
            first = gbs;
        printf("%-14s %8d %12.2f %13.1fx\n", tags[k], n, gbs, first / gbs);

        free(A);
        free(x);
        free(y);
    }
    printf("\n说明：算的东西一模一样，只是矩阵从 16 KB 长到 64 MB。\n");
    printf("      主频一点没变，带宽掉到不足三分之一。\n");
    printf("      这还是在这台虚拟机上——内存要过一层地址转换，真实机器上掉得更狠。\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_ghz = bench_calibrate_ghz();

    printf("核心实际频率 %.2f GHz\n\n", g_ghz);

    test_branch();
    test_ilp();
    test_cache();

    printf("\n=== 合起来看 ===\n");
    printf("分支猜错   清空流水线，白等十几拍\n");
    printf("依赖太长   计算单元空转，只用到几分之一的算力\n");
    printf("数据太远   计算单元等内存，等两百拍\n");
    printf("三种都是\"主频还在跑，活没干\"。\n");

    return 0;
}
