/*
 * 第 3 篇的 ARM64 部分：同一段点积，用 NEON 写一遍。
 *
 * 这台虚拟机是 x86_64，没有 aarch64 的 libc 和动态链接器，所以这个文件
 * 是不依赖任何库的裸程序：自己写 _start，用 svc 直接发系统调用。
 * 这样才能用 qemu-aarch64-static 真把它跑起来，而不是只贴一段汇编。
 *
 * 编译（在 x86_64 上交叉编译）：
 *   clang --target=aarch64-linux-gnu -O2 -march=armv8-a+simd \
 *         -ffreestanding -nostdlib -static -o neon.aarch64 neon.c
 * 运行：
 *   qemu-aarch64-static ./neon.aarch64
 *
 * 注意：跑起来只能证明"算得对"，不能证明"跑得快"。qemu 是逐条翻译执行，
 * 时间没有任何意义。速度那一栏必须看 x86 上的实测。
 */
#include <arm_neon.h>

#define N 1024

static float A[N];
static float B[N];

/* ---------------- 没有 libc，自己发系统调用 ---------------- */

static long sys_write(int fd, const void *buf, unsigned long n)
{
    register long x8 __asm__("x8") = 64;    /* __NR_write */
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)n;

    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void sys_exit(int code)
{
    register long x8 __asm__("x8") = 93;    /* __NR_exit_group */
    register long x0 __asm__("x0") = code;

    __asm__ __volatile__("svc #0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_i64(char *p, long v)
{
    char tmp[24];
    int n = 0;

    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

/* ---------------- 两个版本 ---------------- */

/* NEON：一次 4 个 float，四个累加器避开 FMA 的 4 拍延迟 */
static float dot_neon(const float *a, const float *b, int n)
{
    float32x4_t s0 = vdupq_n_f32(0.0f);
    float32x4_t s1 = vdupq_n_f32(0.0f);
    float32x4_t s2 = vdupq_n_f32(0.0f);
    float32x4_t s3 = vdupq_n_f32(0.0f);
    int i = 0;

    for (; i + 16 <= n; i += 16) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        s2 = vfmaq_f32(s2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        s3 = vfmaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }

    float r = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));

    for (; i < n; i++)
        r += a[i] * b[i];
    return r;
}

static float dot_scalar(const float *a, const float *b, int n)
{
    float r = 0.0f;

    for (int i = 0; i < n; i++)
        r += a[i] * b[i];
    return r;
}

void _start(void)
{
    char buf[96];
    char *p = buf;

    /* 取值都取小整数，乘积和累加在 float 里都是精确的，
     * 这样两种加法顺序会得到逐位相同的结果，比较才有意义。
     * 不用对称的取值，否则和接近 0，看不出算得对不对。 */
    for (int i = 0; i < N; i++) {
        A[i] = (float)(i % 7);
        B[i] = (float)(i % 5);
    }

    long vn = (long)dot_neon(A, B, N);
    long vs = (long)dot_scalar(A, B, N);

    p = put_str(p, "neon=");
    p = put_i64(p, vn);
    p = put_str(p, " scalar=");
    p = put_i64(p, vs);
    p = put_str(p, vn == vs ? " equal=1\n" : " equal=0\n");

    sys_write(1, buf, (unsigned long)(p - buf));
    sys_exit(vn == vs ? 0 : 1);
}
