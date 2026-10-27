ARM64 NEON/SIMD 如何加速 AI 计算

—— 向量指令、数据并行，以及一个反直觉的结果（第 3/5 篇）


上一篇量到，一条 AVX2 的 FMA 指令一次做 8 次乘加，但实测每拍只有 6.7 次——因为取数跟不上了。

那么把矩阵乘写得更"向量化"一些，是不是就快了？

这一篇拿同一道矩阵乘做五个版本，每次只改一个因素，看每一步到底值多少钱。最后一个版本是手写向量指令，结果**没有跑赢编译器自动做的**。

这个结果比"向量化快 N 倍"有用得多。


一、先定一个基线：教科书上的三重循环

矩阵乘 C = A × B，最直觉的写法是三层循环：

    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float s = 0.0f;

            for (int k = 0; k < n; k++)
                s += A[(size_t)i * n + k] * B[(size_t)k * n + j];
            C[(size_t)i * n + j] = s;
        }

（下面几段代码里的 `(size_t)` 是下标转无符号 64 位整数。矩阵最大 512×512，`int` 本来也装得下，写它是为了在更大的尺寸下不至于先溢出。）

读法就是数学定义："C 的第 i 行第 j 列，等于 A 的第 i 行点乘 B 的第 j 列。"

问题在最内层那个 k。`A[i*n+k]` 是顺序读的，没问题；`B[k*n+j]` 每走一步 k 就跳 n 个 float——**内存是跨行跳着读的。**

一个缓存行 64 字节，能装 16 个 float。这么跳着读，每读一个数就要扔掉 15 个。

四核 2.5 GHz 的机器上，512×512 的矩阵乘只有 0.81 GFLOPS。这个数很难看——CPU 理论上能做几十 GFLOPS。


二、五个版本，每次只改一个因素

想搞清楚"到底哪一步起作用"，就不能一次改好几样。五个版本这样设计：

| 版本 | 改了什么 |
|---|---|
| i,j,k 教科书顺序 | 什么都不改，基线 |
| i,k,j 只换顺序 | 只把 k 提到 j 外面 |
| i,k,j + 自动向量化 | 顺序 + 让编译器自己用向量指令 |
| i,j,k + 手写 AVX2 | 只手写向量指令，顺序还是错的 |
| i,k,j + 手写 AVX2 | 顺序对 + 手写向量指令 |

关键在第四行。它和第三行比，只差"谁写的向量指令"；它和第一行比，只差"有没有向量指令"。**有了这一行，才能知道"向量化"和"访存顺序"各占多少。**

只换顺序是这么写的：

    memset(C, 0, (size_t)n * (size_t)n * sizeof(float));

    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            const float a = A[(size_t)i * n + k];

            for (int j = 0; j < n; j++)
                C[(size_t)i * n + j] += a * B[(size_t)k * n + j];
        }

内层 j 现在同时顺序访问 C 的一行和 B 的一行，两个都是连续读，缓存行全用上。

手写 AVX2 的版本：

    for (int i = 0; i < n; i++) {
        float *crow = C + (size_t)i * n;

        memset(crow, 0, (size_t)n * sizeof(float));

        for (int k = 0; k < n; k++) {
            const float av = A[(size_t)i * n + k];
            const __m256 a = _mm256_set1_ps(av);
            const float *brow = B + (size_t)k * n;
            int j = 0;

            for (; j + 8 <= n; j += 8) {
                __m256 c = _mm256_loadu_ps(crow + j);

                c = _mm256_fmadd_ps(a, _mm256_loadu_ps(brow + j), c);
                _mm256_storeu_ps(crow + j, c);
            }
            for (; j < n; j++)
                crow[j] += av * brow[j];
        }
    }

`_mm256_set1_ps(av)` 把 A 的一个数复制到 8 个通道——这叫广播。一次装载，喂给 8 次乘加。这正是上一篇说的"突破取数墙"的办法。

`_mm256_loadu_ps` 里的 u 是 unaligned，不要求 32 字节对齐。慢一点点，但不用管内存布局。


三、实测

四个尺寸，单位 GFLOPS（每秒十亿次浮点运算，一次乘加记两次）：

n = 64（一个矩阵 16 KB）：

| 版本 | GFLOPS | 相对 |
|---|---|---|
| i,j,k 教科书顺序 | 2.12 | 1.0x |
| i,k,j 只换顺序 | 2.44 | 1.2x |
| i,k,j + 自动向量化 | 16.64 | 7.9x |
| i,j,k + 手写 AVX2 | 17.54 | 8.3x |
| i,k,j + 手写 AVX2 | 19.06 | 9.0x |

n = 128（一个矩阵 64 KB）：

| 版本 | GFLOPS | 相对 |
|---|---|---|
| i,j,k 教科书顺序 | 1.51 | 1.0x |
| i,k,j 只换顺序 | 2.51 | 1.7x |
| i,k,j + 自动向量化 | 11.66 | 7.7x |
| i,j,k + 手写 AVX2 | 8.41 | 5.6x |
| i,k,j + 手写 AVX2 | 11.78 | 7.8x |

n = 256（一个矩阵 256 KB）：

| 版本 | GFLOPS | 相对 |
|---|---|---|
| i,j,k 教科书顺序 | 1.32 | 1.0x |
| i,k,j 只换顺序 | 2.38 | 1.8x |
| i,k,j + 自动向量化 | 13.12 | 9.9x |
| i,j,k + 手写 AVX2 | 7.31 | 5.5x |
| i,k,j + 手写 AVX2 | 15.71 | 11.9x |

n = 512（一个矩阵 1024 KB）：

| 版本 | GFLOPS | 相对 |
|---|---|---|
| i,j,k 教科书顺序 | 0.81 | 1.0x |
| i,k,j 只换顺序 | 2.40 | 3.0x |
| i,k,j + 自动向量化 | 12.59 | 15.5x |
| i,j,k + 手写 AVX2 | 4.85 | 6.0x |
| i,k,j + 手写 AVX2 | 13.96 | 17.2x |

三个结论，一个比一个反直觉。

**结论一：矩阵越大，换顺序越值钱。**

只换顺序那一行的倍数，随尺寸走成 1.2 → 1.7 → 1.8 → 3.0。n=64 时几乎没用——16 KB 的矩阵整个在 L1 里，怎么跳都命中缓存。n=512 时是 3.0 倍，因为 1 MB 的矩阵装不进 L1，跳着读就真的在等内存。

同一件事在基线那一行也有反证：教科书顺序从 2.12 一路掉到 0.81，越大的矩阵越惨。**它掉的不是算力，是缓存。**

**结论二：向量化本身值 5 倍左右。**

从 2.40 到 12.59，5.2 倍。这部分是实打实的：一条指令算 8 个数，而不是 1 个。

**结论三：手写向量指令，没有跑赢编译器。**

n=512：手写 13.96，编译器 12.59。差 11%，在重复测量的波动范围里。n=128 时几乎一模一样（11.78 vs 11.66）。

但第四行说明了另一件事：**只写向量指令、不改顺序，只有 4.85**，比编译器自动做的 12.59 差了将近三倍。

为什么？因为我的版本每算完一个 k 就要把 C 从内存读回来、算完再写回去。编译器那版把 C 留在寄存器里做了更大的累积块。想赢它，就得自己做分块——那已经是在手写 GEMM 库了。

**所以正确的顺序是：先改访存，再谈向量。** 反过来做，投入的力气有一大半是白费的。


四、换到 ARM64：同一段代码，NEON 版

x86 的 AVX2 一条指令 256 位，装 8 个 float。ARM64 的 NEON 是 128 位，装 4 个。

同样的点积，NEON 版这样写：

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

指令名字不一样，思路一模一样：四个累加器，一次 4 个 float，循环末尾把四个累加器合并成一个标量。

对照表：

| x86 AVX2 | ARM64 NEON |
|---|---|
| _mm256_loadu_ps | vld1q_f32 |
| _mm256_set1_ps | vdupq_n_f32 |
| _mm256_fmadd_ps | vfmaq_f32 |
| _mm256_add_ps | vaddq_f32 |
| _mm_hadd_ps（两次） | vaddvq_f32（一条） |
| 8 个 float / 指令 | 4 个 float / 指令 |

反汇编出来，内层循环完整是这样：

    2102c4:  ldp   q4, q5, [x11, #-0x20]        从 a 装 32 字节
    2102c8:  add   x10, x10, #0x10              下标 +16
    2102cc:  cmp   x10, #0x3e1                  走完了没有
    2102d0:  ldp   q6, q7, [x12, #-0x20]        从 b 装 32 字节
    2102d4:  fmla  v0.4s, v6.4s, v4.4s          累加器 0
    2102d8:  ldp   q16, q17, [x11], #0x40       再装 32 字节，地址 +64
    2102dc:  fmla  v1.4s, v7.4s, v5.4s          累加器 1
    2102e0:  ldp   q18, q19, [x12], #0x40       再装 32 字节，地址 +64
    2102e4:  fmla  v2.4s, v18.4s, v16.4s        累加器 2
    2102e8:  fmla  v3.4s, v19.4s, v17.4s        累加器 3
    2102ec:  b.lo  2102c4                       没算完就跳回去

十一条指令，一轮处理 16 个元素。

拆开数：四条 `fmla` 算 16 次乘加，四条 `ldp` 一共装 128 字节（a 和 b 各 64）。剩下三条是循环控制。

`ldp` 是"一次装一对寄存器"，一条装两个 128 位寄存器就是 32 字节，装完自动把地址往后挪。x86 的 `vmovups` 一条也是 32 字节，两者一样，没有谁更省。

值得算的是另一个比例：**每做一次乘加，要搬 8 个字节。**

    NEON   128 字节 ÷ 16 次乘加 = 8 字节/次
    AVX2    64 字节 ÷  8 次乘加 = 8 字节/次

两种架构、两种指令集，这个比例一模一样。

因为它是硬件决定的：一次乘加要读两个 float，每个 4 字节，就是 8 字节。**只要两个操作数都得从内存读，这个比例就降不下来。**

这也解释了为什么 NEON 的吞吐是 AVX2 的一半：一条指令装 4 个 float 而不是 8 个，但搬的字节数一样，所以算的次数少一半。

想打破这个比例，只有一个办法：**让一个操作数留在寄存器里重复用**。矩阵乘里广播 A 就是在干这个——A 的一个数被 8 个通道共用，8 次乘加只花一次装载的钱。这才是矩阵乘能跑到几十 GFLOPS、而点积只能跑到十几的原因。

这一段怎么验证的

这台机器是 x86_64，没有 aarch64 的运行环境。用 clang 交叉编译成 ARM64 的可执行文件，再用 qemu 的用户态模拟跑：

    clang --target=aarch64-linux-gnu -O2 -march=armv8-a+simd \
          -ffreestanding -nostdlib -static -fuse-ld=lld \
          -o bin/neon.aarch64 src/neon.c

    qemu-aarch64-static ./bin/neon.aarch64
    neon=6129 scalar=6129 equal=1

两件事要说明白。

**一、`-fuse-ld=lld` 不能省。** x86 上的 GNU ld 只会链 x86，遇到 aarch64 目标直接报"无法辨认的仿真模式"。lld 是 LLVM 自带的多架构链接器。

**二、`-ffreestanding -nostdlib` 是因为没有 aarch64 的 libc。** 这个程序自己写 `_start`，用 `svc` 指令直接发系统调用：

    static long sys_write(int fd, const void *buf, unsigned long n)
    {
        register long x8 __asm__("x8") = 64;    /* __NR_write */
        register long x0 __asm__("x0") = fd;
        register long x1 __asm__("x1") = (long)buf;
        register long x2 __asm__("x2") = (long)n;

        __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
        return x0;
    }

x8 放系统调用号，x0/x1/x2 放参数，`svc #0` 陷入内核。这是 ARM64 调用系统调用的全部约定。

**qemu 跑出来的时间没有意义**——它逐条翻译执行，比真机慢几十倍。这一段只证明"算得对"，速度那一栏全部来自 x86 上的实测。

数据取的都是小整数，乘积和累加在 float 里都是精确的，所以两种加法顺序会得到逐位相同的结果。`equal=1` 说的是这个。


五、回到场景

如果要在 CPU 上把一个矩阵乘提速，力气该往哪使，现在有答案了：

| 顺序 | 做什么 | 收益 |
|---|---|---|
| 先 | 看访存顺序 | 3 倍，改几行循环嵌套 |
| 再 | 让编译器向量化 | 5 倍，加一个 -O3 -march=native |
| 最后 | 才考虑手写 | 0~10%，而且写错顺序会倒亏 |

第三行不是说手写没用。是说**在编译器已经能自动向量化的今天，手写的边际收益很小，而它带来的维护成本和可移植成本很高。**

真正需要手写的场合只有两类：编译器不肯向量化的（比如带复杂分支的、数据依赖绕不过的），以及你要榨干最后 20% 的（那就得连分块、预取、寄存器分配一起做，等于自己写一个 GEMM 库）。

下一篇讲另一个方向：就算你向量化做对了，程序还是可能很慢。慢在三个地方——分支、依赖、缓存。


---

本系列用到的代码都在提交 `4b4364c` 里：

    https://github.com/qhc0609/openeuler-sched-ext/commit/4b4364c

    git show 4b4364c:cpu-ai/src/gemm_ijk.c
    git show 4b4364c:cpu-ai/src/gemm_ikj_body.h
    git show 4b4364c:cpu-ai/src/gemm_avx2.c
    git show 4b4364c:cpu-ai/src/gemm_avx2_ikj.c
    git show 4b4364c:cpu-ai/src/neon.c

自己跑一遍：

    cd cpu-ai && make
    taskset -c 2 ./bin/gemm
    qemu-aarch64-static ./bin/neon.aarch64
