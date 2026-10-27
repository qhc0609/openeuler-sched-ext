/*
 * 第 1 篇的起点：三个函数，从最简单的开始，一个一个加东西。
 *
 *   add()      一个加法。没有循环，不碰内存，两个数进一个数出。
 *   sum_int()  加一个循环。要读内存，要比较，要往回跳。
 *   dot_f32()  再加一次乘法。两个数组，一条依赖链，就是 AI 里的乘加。
 *
 * 编译：
 *   gcc -O2 -fno-tree-vectorize -o bin/oneline src/oneline.c
 *
 * 为什么关掉自动向量化：为了让一行 C 对应一条指令。打开之后编译器会把
 * 循环改成一条指令算八个，那就看不清"一条指令"长什么样了。打开以后
 * 变成什么样，第三篇再说。
 */
#include <stdint.h>
#include <stdio.h>

#include "../common/bench.h"

/* 防止结果被当成死代码删掉 */
static volatile float g_sink_f;
static volatile uint64_t g_sink_u;

/* ---------------- 一、一个加法 ---------------- */

int add(int a, int b)
{
    return a + b;
}

/* ---------------- 二、加一个循环 ---------------- */

long sum_int(const int *a, long n)
{
    long s = 0;

    for (long i = 0; i < n; i++)
        s += a[i];
    return s;
}

/* ---------------- 三、再加一次乘法 ---------------- */

float dot_f32(const float *a, const float *b, long n)
{
    float s = 0.0f;

    for (long i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

/* ---------------- 四、量两种加法各要几拍 ---------------- */

/*
 * 整数加法：x 和 x 自己加，后一条必须等前一条写回同一个寄存器。
 * 一条链咬到底，量出来的就是这条指令的延迟。
 */
static void chain_add_int(uint64_t n)
{
    uint64_t x = 1;

    for (uint64_t i = 0; i < n; i += 8)
        __asm__ __volatile__(
            "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0\n\t"
            "addq $1, %0\n\t" "addq $1, %0"
            : "+r"(x));

    g_sink_u = x;
}

/* 浮点加法：同样的依赖链，只是指令换成 addss（标量单精度） */
static void chain_add_float(uint64_t n)
{
    float x = 1.0f, one = 1.0f;

    for (uint64_t i = 0; i < n; i += 8)
        __asm__ __volatile__(
            "addss %1, %0\n\t" "addss %1, %0\n\t"
            "addss %1, %0\n\t" "addss %1, %0\n\t"
            "addss %1, %0\n\t" "addss %1, %0\n\t"
            "addss %1, %0\n\t" "addss %1, %0"
            : "+x"(x) : "x"(one));

    g_sink_f = x;
}

/* 跑一遍，取最短的一次：干扰只会让时间变长，不会变短 */
static double ns_per_op(void (*fn)(uint64_t), uint64_t n)
{
    uint64_t best = ~0ull;

    fn(n / 10);                       /* 热身 */
    for (int r = 0; r < 3; r++) {
        uint64_t t0 = bench_ns();

        fn(n);
        uint64_t d = bench_ns() - t0;

        if (d < best)
            best = d;
    }
    return (double)best / (double)n;
}

/* ---------------- 跑起来，看看要多久 ---------------- */

#define N 20000        /* 每个数组 80 KB，两个共 160 KB，稳在 L2 */
#define R 1000         /* 重复一千遍，总共两千万次乘加 */

int main(void)
{
    static float a[N], b[N];
    static int ai[N];
    float s = 0.0f;
    uint64_t ns0, ns1;
    double ghz, ms, cyc_per_mac, ideal_ms;
    double cyc_int, cyc_f32;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (int i = 0; i < N; i++) {
        a[i] = (float)(i % 17) * 0.25f;
        b[i] = (float)(i % 13) * 0.5f;
        ai[i] = i % 100;
    }

    printf("=== 一、先确认算得对 ===\n");
    printf("add(3, 4)            = %d\n", add(3, 4));
    printf("sum_int(ai, 100)     = %ld\n", sum_int(ai, 100));
    printf("dot_f32(a, b, 1000)  = %.3f\n", dot_f32(a, b, 1000));

    /* 主频标定：一条依赖 add 链，每拍只能走一条 */
    ghz = bench_calibrate_ghz();
    printf("\n=== 二、这台机器跑多快 ===\n");
    printf("核心实际频率  %.2f GHz\n", ghz);

    /* 热身，让频率稳定 */
    for (int r = 0; r < 50; r++)
        s += dot_f32(a, b, N);

    /*
     * 每次调用前改一个元素。不改的话，编译器发现输入从头到尾没变过，
     * 会把整个点积提到循环外面只算一遍——量出来会是几十倍的天文数字。
     */
    ns0 = bench_ns();
    for (int r = 0; r < R; r++) {
        a[r & 63] = (float)(r & 7) * 1e-3f;
        s += dot_f32(a, b, N);
    }
    ns1 = bench_ns();

    g_sink_f = s;

    ms = (double)(ns1 - ns0) / 1e6;
    cyc_per_mac = (double)(ns1 - ns0) * ghz / ((double)N * R);
    ideal_ms = (double)N * R / ghz / 1e6;

    printf("\n=== 三、两千万次乘加要多久 ===\n");
    printf("元素个数        %d\n", N);
    printf("重复次数        %d\n", R);
    printf("乘加总次数      %lld\n", (long long)N * R);
    printf("实测耗时        %.1f ms\n", ms);
    printf("每次乘加        %.2f 拍\n", cyc_per_mac);
    printf("按每拍一次算    %.1f ms\n", ideal_ms);
    printf("相差            %.1f 倍\n", ms / ideal_ms);

    printf("\n=== 四、同样是加法，两种差四倍 ===\n");
    cyc_int = ns_per_op(chain_add_int, 200000000ull) * ghz;
    cyc_f32 = ns_per_op(chain_add_float, 200000000ull) * ghz;
    printf("整数 add   依赖链  %.2f 拍/条\n", cyc_int);
    printf("浮点 addss 依赖链  %.2f 拍/条\n", cyc_f32);
    printf("浮点 / 整数        %.2f 倍\n", cyc_f32 / cyc_int);
    printf("说明：整数加法走一拍就出结果，浮点加法要走四拍。\n");
    printf("      这个比值不受主频估计误差影响，比绝对值可靠。\n");
    printf("      点积里那条 s += 是浮点加法，所以每 %.2f 拍才轮到下一次。\n",
           cyc_per_mac);

    printf("\n=== 五、结论 ===\n");
    printf("一行 s += a[i] * b[i] 编译成六条指令，\n");
    printf("其中累加那条要四拍才出结果，后面的只能等它。\n");
    printf("两千万次 × %.1f 拍 = %.0f 毫秒，实测 %.1f 毫秒。\n",
           cyc_per_mac, (double)N * R * cyc_per_mac / ghz / 1e6, ms);

    return 0;
}
