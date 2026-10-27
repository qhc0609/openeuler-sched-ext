# CPU 如何执行 AI 计算

五组演示程序，对应五篇文章。每个数字都能在 `results/` 里找到出处。

全部在一台 4 vCPU 的 openEuler 虚拟机上跑过（Intel i5-7300HQ，2.5 GHz 标称）。

## 先说清楚这台机器的三个限制

写微基准之前必须先摸清机器，否则量出来的东西没法解释。

**一、没有硬件性能计数器。** 虚拟机没有透传 PMU，`perf stat -e cycles` 直接返回
`<not supported>`，`instructions`、`branches`、`cache-misses` 全一样。所以"拍了多少次"
不能靠计数器，得先用一条已知延迟的指令链标定出真实主频，再把墙钟时间换算成拍。

**二、TSC 不等于核心频率。** 实测 TSC 走 2.30 GHz，核心实际 2.54~2.62 GHz，差 10% 上下。
直接把 `rdtsc` 的差值当周期数，结论会系统性偏小。

**三、虚拟机的抖动比物理机大。** 同一程序重复跑，fp32 吞吐能在 13~19 GMAC/s 之间浮动。
所以下文只信"倍数关系"，不信绝对值；每个实验都重复三次取最优。

## 六组程序

| 程序 | 对应文章 | 量什么 |
|---|---|---|
| `src/oneline.c` | 第 1 篇 | 三个函数从简到繁；两千万次乘加实测几拍 |
| `src/stage5.c` | 第 1 篇 | 一条指令五个阶段各要几拍；访存延迟阶梯 |
| `src/dtype.c` | 第 2 篇 | 四种数据类型的乘加吞吐；累加器位宽对精度的影响 |
| `src/gemm_*.c` | 第 3 篇 | 五个矩阵乘版本；ARM64 NEON 的数值校验 |
| `src/bottleneck.c` | 第 4 篇 | 分支预测、指令级并行、缓存容量 |
| `src/hotloop.c` | 第 5 篇 | 被 perf 观察的对象；热点分析 |

## 怎么跑

```bash
make                 # 全部编译（需要 gcc、clang、lld、qemu-user-static、perf）
bash test/run-all.sh # 五组演示，输出到 results/
bash test/run-perf.sh # 第 5 篇的 perf 部分
```

`run-all.sh` 默认把进程绑到 2 号核，避免四个 vCPU 之间互相抢。
换核就传参数：`bash test/run-all.sh 3`。

## ARM64 那部分是交叉编译的

这台机器是 x86_64，没有 aarch64 的 libc。`src/neon.c` 因此写成不依赖任何库的裸程序：
自己写 `_start`，用 `svc` 直接发系统调用。这样能用 `qemu-aarch64-static` 真跑起来。

```bash
clang --target=aarch64-linux-gnu -O2 -march=armv8-a+simd \
      -ffreestanding -nostdlib -static -fuse-ld=lld -o bin/neon.aarch64 src/neon.c
qemu-aarch64-static bin/neon.aarch64
```

`-fuse-ld=lld` 不能省：x86 上的 GNU ld 只会链 x86，遇到 aarch64 目标直接报
"无法辨认的仿真模式"。lld 是 LLVM 自带的多架构链接器。

**qemu 跑出来的时间没有意义**，它逐条翻译执行。NEON 这一段只证明"算得对"，
不证明"跑得快"。速度那一栏全部来自 x86 上的实测。

## 结果文件

```
results/00-env.txt          机器、编译器、perf 版本
results/01-stage5.txt       五个阶段
results/01b-oneline.txt     两千万次乘加；整数与浮点加法的延迟对比
results/01c-disasm.txt      三个函数编译出来的指令
results/02-dtype.txt        数据类型与精度
results/03-gemm.txt         五个矩阵乘版本
results/04-bottleneck.txt   三种瓶颈
results/05-neon.txt         NEON 数值校验与指令序列
results/06-perf-stat.txt    PMU 可用性逐项测试
results/07-perf-record.txt  软件事件采样做热点分析
results/08-objdump.txt      热点函数的反汇编
```

这些文件是文章里每个数字的唯一出处。改代码之后重跑，数字会变，文章要跟着改。
