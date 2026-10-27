/*
 * 第 2 篇的验证程序：数据类型决定了什么。
 *
 * 两件事：
 *   一、同样一条"乘加"，换成不同的数据类型，每拍能做几次。
 *   二、同样一个点积，累加器用多宽的浮点，结果能差多少。
 *
 * 第一件是吞吐，第二件是精度。AI 推理里这两个是一起做决策的：
 * 想快就得降位宽，降了位宽就得拿累加器补精度。
 *
 * 编译：gcc -O2 -mavx2 -mfma -fno-tree-vectorize -o dtype dtype.c -lm
 */
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/bench.h"

static double g_ghz;

/* ================= 一、吞吐：四种数据类型的乘加 ================= */

/*
 * 数组要小到两个加起来远小于 L1（32 KB），否则量的是 L2 带宽而不是计算单元。
 * 每对都是 8 KB + 8 KB = 16 KB，给栈和别的数据留一半。
 */
#define N_F32 2048
#define N_F64 1024
#define N_I32 2048
#define N_I8  8192

static float fa[N_F32], fb[N_F32];
static double da[N_F64], db[N_F64];
static int32_t ia[N_I32], ib[N_I32];
static uint8_t qa[N_I8];
static int8_t qb[N_I8];

/* 防止整段计算被提出循环：每次换一个起点，参数变了就没法复用上次的结果 */
static volatile double g_sink;

/* ---- 参考实现：纯标量，用来验证向量版算得对不对 ---- */

static float dot_f32_ref(const float *a, const float *b, int n)
{
    float s = 0.0f;

    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static double dot_f64_ref(const double *a, const double *b, int n)
{
    double s = 0.0;

    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static int32_t dot_i32_ref(const int32_t *a, const int32_t *b, int n)
{
    int32_t s = 0;

    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static int32_t dot_i8_ref(const uint8_t *a, const int8_t *b, int n)
{
    int32_t s = 0;

    for (int i = 0; i < n; i++)
        s += (int32_t)a[i] * (int32_t)b[i];
    return s;
}

/* ---- 向量版：AVX2 ---- */

/* 四个累加器：一个 FMA 的延迟是 4 拍，只用一个累加器的话每 4 拍才能发一条 */
static float dot_f32_avx2(const float *a, const float *b, int n)
{
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
    __m128 lo, hi, t;
    int i = 0;

    for (; i + 32 <= n; i += 32) {
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
    }
    s0 = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    lo = _mm256_castps256_ps128(s0);
    hi = _mm256_extractf128_ps(s0, 1);
    t = _mm_add_ps(lo, hi);
    t = _mm_hadd_ps(t, t);
    t = _mm_hadd_ps(t, t);

    float r = _mm_cvtss_f32(t);

    for (; i < n; i++)
        r += a[i] * b[i];
    return r;
}

static double dot_f64_avx2(const double *a, const double *b, int n)
{
    __m256d s0 = _mm256_setzero_pd(), s1 = _mm256_setzero_pd();
    __m256d s2 = _mm256_setzero_pd(), s3 = _mm256_setzero_pd();
    __m128d lo, hi, t;
    int i = 0;

    for (; i + 16 <= n; i += 16) {
        s0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i), s0);
        s1 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 4), _mm256_loadu_pd(b + i + 4), s1);
        s2 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 8), _mm256_loadu_pd(b + i + 8), s2);
        s3 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 12), _mm256_loadu_pd(b + i + 12), s3);
    }
    s0 = _mm256_add_pd(_mm256_add_pd(s0, s1), _mm256_add_pd(s2, s3));
    lo = _mm256_castpd256_pd128(s0);
    hi = _mm256_extractf128_pd(s0, 1);
    t = _mm_add_pd(lo, hi);
    t = _mm_hadd_pd(t, t);

    double r = _mm_cvtsd_f64(t);

    for (; i < n; i++)
        r += a[i] * b[i];
    return r;
}

static int32_t dot_i32_avx2(const int32_t *a, const int32_t *b, int n)
{
    __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
    __m128i lo, hi, t;
    int i = 0;

    for (; i + 16 <= n; i += 16) {
        s0 = _mm256_add_epi32(s0, _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(a + i)),
                                                     _mm256_loadu_si256((const __m256i *)(b + i))));
        s1 = _mm256_add_epi32(s1, _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(a + i + 8)),
                                                     _mm256_loadu_si256((const __m256i *)(b + i + 8))));
    }
    s0 = _mm256_add_epi32(s0, s1);
    lo = _mm256_castsi256_si128(s0);
    hi = _mm256_extracti128_si256(s0, 1);
    t = _mm_add_epi32(lo, hi);
    t = _mm_hadd_epi32(t, t);
    t = _mm_hadd_epi32(t, t);

    int32_t r = _mm_cvtsi128_si32(t);

    for (; i < n; i++)
        r += a[i] * b[i];
    return r;
}

/*
 * int8：这是量化推理真正用的指令。
 * pmaddubsw 一次吃 32 个无符号字节和 32 个有符号字节，两两相乘再相加，
 * 得到 16 个 int16 —— 一条指令 32 次乘加，是 fp32 的四倍。
 * 前提是第一个操作数必须无符号，这正好对上量化里的"uint8 激活 × int8 权重"。
 */
static int32_t dot_i8_avx2(const uint8_t *a, const int8_t *b, int n)
{
    const __m256i ones = _mm256_set1_epi16(1);
    __m256i acc = _mm256_setzero_si256();
    __m128i lo, hi, t;
    int i = 0;

    for (; i + 32 <= n; i += 32) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i p16 = _mm256_maddubs_epi16(va, vb);

        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(p16, ones));
    }
    lo = _mm256_castsi256_si128(acc);
    hi = _mm256_extracti128_si256(acc, 1);
    t = _mm_add_epi32(lo, hi);
    t = _mm_hadd_epi32(t, t);
    t = _mm_hadd_epi32(t, t);

    int32_t r = _mm_cvtsi128_si32(t);

    for (; i < n; i++)
        r += (int32_t)a[i] * (int32_t)b[i];
    return r;
}

/* ================= 二、精度：累加器位宽 ================= */

/*
 * 两两合并：每次把数组劈成两半，各自求和再相加。
 *
 * 关键在"一直劈到底"，不能只劈一层然后顺序累加——
 * 那样块之间还是顺序的，误差照样随块数线性增长，
 * 和朴素累加没有本质区别。
 *
 * 误差随 log2(n) 增长，不是随 n 增长。代价是 n 次加法之外
 * 还要一份栈，以及更差的缓存局部性。
 */
static double pairwise_f64_rec(const float *v, size_t lo, size_t hi)
{
    size_t n = hi - lo;
    double s = 0.0;

    if (n <= 32) {
        for (size_t i = lo; i < hi; i++)
            s += (double)v[i];
        return s;
    }
    return pairwise_f64_rec(v, lo, lo + n / 2) + pairwise_f64_rec(v, lo + n / 2, hi);
}

static float pairwise_f32_rec(const float *v, size_t lo, size_t hi)
{
    size_t n = hi - lo;
    float s = 0.0f;

    if (n <= 32) {
        for (size_t i = lo; i < hi; i++)
            s += v[i];
        return s;
    }
    return pairwise_f32_rec(v, lo, lo + n / 2) + pairwise_f32_rec(v, lo + n / 2, hi);
}

/* 参考值用 fp64 两两合并，误差在 1e-12 量级，可以当真值 */
static double sum_pairwise_f64(const float *v, size_t n)
{
    return pairwise_f64_rec(v, 0, n);
}

static double sum_fp16(const float *v, size_t n)
{
    _Float16 s = 0.0f;

    for (size_t i = 0; i < n; i++)
        s = (_Float16)(s + (_Float16)v[i]);
    return (double)s;
}

static double sum_fp32_naive(const float *v, size_t n)
{
    float s = 0.0f;

    for (size_t i = 0; i < n; i++)
        s += v[i];
    return (double)s;
}

static double sum_fp32_acc8(const float *v, size_t n)
{
    float acc[8] = { 0 };

    for (size_t i = 0; i + 8 <= n; i += 8)
        for (int j = 0; j < 8; j++)
            acc[j] += v[i + j];
    return (double)(((acc[0] + acc[1]) + (acc[2] + acc[3])) +
                    ((acc[4] + acc[5]) + (acc[6] + acc[7])));
}

static double sum_fp32_pairwise(const float *v, size_t n)
{
    return (double)pairwise_f32_rec(v, 0, n);
}

static double sum_fp32_kahan(const float *v, size_t n)
{
    float s = 0.0f, c = 0.0f;

    for (size_t i = 0; i < n; i++) {
        float y = v[i] - c;
        float t = s + y;

        c = (t - s) - y;
        s = t;
    }
    return (double)s;
}

static double sum_fp64(const float *v, size_t n)
{
    double s = 0.0;

    for (size_t i = 0; i < n; i++)
        s += (double)v[i];
    return s;
}

int main(void)
{
    const int repeat = 20000;
    double macs[4], secs[4];
    uint64_t ns0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_ghz = bench_calibrate_ghz();

    for (int i = 0; i < N_F32; i++) {
        fa[i] = (float)((i % 17) - 8) * 0.125f;
        fb[i] = (float)((i % 13) - 6) * 0.25f;
    }
    for (int i = 0; i < N_F64; i++) {
        da[i] = (double)((i % 17) - 8) * 0.125;
        db[i] = (double)((i % 13) - 6) * 0.25;
    }
    for (int i = 0; i < N_I32; i++) {
        ia[i] = (i % 17) - 8;
        ib[i] = (i % 13) - 6;
    }
    /* int8 的取值刻意压小：pmaddubsw 会饱和到 int16，数据大了结论就不对了 */
    for (int i = 0; i < N_I8; i++) {
        qa[i] = (uint8_t)(i % 9);
        qb[i] = (int8_t)((i % 17) - 8);
    }

    printf("=== 一、先验证向量版和标量版算的是同一个数 ===\n");
    printf("fp32  标量 %.6f   向量 %.6f\n", dot_f32_ref(fa, fb, N_F32), dot_f32_avx2(fa, fb, N_F32));
    printf("fp64  标量 %.6f   向量 %.6f\n", dot_f64_ref(da, db, N_F64), dot_f64_avx2(da, db, N_F64));
    printf("int32 标量 %d   向量 %d\n", dot_i32_ref(ia, ib, N_I32), dot_i32_avx2(ia, ib, N_I32));
    printf("int8  标量 %d   向量 %d\n", dot_i8_ref(qa, qb, N_I8), dot_i8_avx2(qa, qb, N_I8));

    printf("\n=== 二、每拍能做多少次乘加 ===\n");
    printf("（数据全部在 L1 里，排除访存影响，只看计算单元）\n\n");

    memset(macs, 0, sizeof(macs));
    memset(secs, 0, sizeof(secs));

    /*
     * 防止编译器把整个点积提到循环外面只算一次：每轮改一个元素。
     * 不能靠"换起点"来防——那会让长度不是 32 的倍数，尾部退回标量循环，
     * 量出来的时间一大半花在尾巴上，比原来的问题更糟。
     */
    {
        double sum = 0.0;

        ns0 = bench_ns();
        for (int t = 0; t < repeat; t++) {
            fa[t & 63] = (float)(t & 7) * 1e-3f;
            sum += dot_f32_avx2(fa, fb, N_F32);
            macs[0] += N_F32;
        }
        secs[0] = (double)(bench_ns() - ns0) / 1e9;
        g_sink = sum;
    }
    {
        double sum = 0.0;

        ns0 = bench_ns();
        for (int t = 0; t < repeat; t++) {
            da[t & 63] = (double)(t & 7) * 1e-3;
            sum += dot_f64_avx2(da, db, N_F64);
            macs[1] += N_F64;
        }
        secs[1] = (double)(bench_ns() - ns0) / 1e9;
        g_sink = sum;
    }
    {
        double sum = 0.0;

        ns0 = bench_ns();
        for (int t = 0; t < repeat; t++) {
            ia[t & 63] = t & 7;
            sum += dot_i32_avx2(ia, ib, N_I32);
            macs[2] += N_I32;
        }
        secs[2] = (double)(bench_ns() - ns0) / 1e9;
        g_sink = sum;
    }
    {
        double sum = 0.0;

        ns0 = bench_ns();
        for (int t = 0; t < repeat; t++) {
            qa[t & 63] = (uint8_t)(t & 7);
            sum += dot_i8_avx2(qa, qb, N_I8);
            macs[3] += N_I8;
        }
        secs[3] = (double)(bench_ns() - ns0) / 1e9;
        g_sink = sum;
    }

    {
        static const char *names[] = {
            "fp32  AVX2 FMA", "fp64  AVX2 FMA", "int32 AVX2 mullo", "int8  AVX2 maddubs",
        };
        double base = macs[0] / secs[0] / 1e9;

        printf("%-20s %12s %12s %12s\n", "类型", "GMAC/s", "MAC/拍", "相对 fp32");
        for (int i = 0; i < 4; i++) {
            double g = macs[i] / secs[i] / 1e9;

            printf("%-20s %12.2f %12.1f %11.2fx\n", names[i], g, g / g_ghz, g / base);
        }
    }

    printf("\n说明：MAC/拍 = 每拍做几次乘加。\n");
    printf("      fp32 的理论上限是 16（256 位装 8 个 float，两个 FMA 单元），\n");
    printf("      实测只有 7 左右，因为一条 FMA 要读两个 32 字节的操作数，\n");
    printf("      而每拍只有两次装载——卡住的是取数，不是运算。\n");
    printf("      fp64 正好是它的一半，也是同一个原因（操作数宽度翻倍）。\n");
    printf("      int8 一条指令吃 32 个字节，能突破这堵墙，但远不到四倍：\n");
    printf("      没有 VNNI 的 CPU 上，乘完还要两条指令把结果归约下来。\n");

    printf("\n=== 三、累加器位宽：同样一个点积，结果能差多少 ===\n");
    printf("（一百万个 [-1,1] 的随机数求和，真值是一个随机游走的结果）\n");
    printf("（换 15 组不同的随机数各做一遍，取误差的中位数）\n\n");
    {
        const size_t n = 1000000;
        const int trials = 15;
        float *v = malloc(n * sizeof(float));
        double errs[6][15];
        double ref = 0.0;
        uint32_t seed = 12345u;

        if (!v) {
            perror("malloc");
            return 1;
        }

        for (int t = 0; t < trials; t++) {
            /*
             * 只做一次试验分不出高下：不同求和顺序的误差都在 1e-5 量级，
             * 谁大谁小取决于那一次数据恰好怎么排。换 15 组取中位数才稳。
             */
            for (size_t i = 0; i < n; i++) {
                seed = seed * 1103515245u + 12345u;
                v[i] = (float)((double)(seed >> 8) / 16777216.0 - 0.5) * 2.0f;
            }

            ref = sum_pairwise_f64(v, n);

            errs[0][t] = fabs(sum_fp16(v, n) - ref);
            errs[1][t] = fabs(sum_fp32_naive(v, n) - ref);
            errs[2][t] = fabs(sum_fp32_acc8(v, n) - ref);
            errs[3][t] = fabs(sum_fp32_pairwise(v, n) - ref);
            errs[4][t] = fabs(sum_fp32_kahan(v, n) - ref);
            errs[5][t] = fabs(sum_fp64(v, n) - ref);
        }

        {
            static const char *names[] = {
                "fp16 累加", "fp32 朴素累加", "fp32 八个累加器",
                "fp32 两两合并", "fp32 Kahan 补偿", "fp64 直接累加",
            };
            double base;

            for (int k = 0; k < 6; k++)
                qsort(errs[k], trials, sizeof(double), bench_cmp_dbl);

            base = errs[1][trials / 2];
            if (base <= 0.0)
                base = 1e-12;

            printf("%-26s %14s %14s\n", "累加器", "误差中位数", "相对朴素");
            for (int k = 0; k < 6; k++) {
                double med = errs[k][trials / 2];

                printf("%-26s %14.4f %13.3gx\n", names[k], med,
                       med > 0.0 ? med / base : 0.0);
            }
        }

        printf("\n（真值取最后一组：%.3f，共 %zu 项）\n", ref, n);
        printf("fp16 累加器的误差是 fp32 的几千倍——它把大部分加法直接丢了。\n");
        printf("这就是量化推理里\"权重可以 8 位，累加器必须 32 位\"的原因。\n");
        printf("注意中间三行：两两合并和 Kahan 都是纯算法，不花任何硬件代价；\n");
        printf("只有 fp64 是拿一半吞吐去换精度。\n");

        free(v);
    }

    return 0;
}
