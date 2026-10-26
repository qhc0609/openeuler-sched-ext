# 机器人大小脑：架构、载体与实测

## 一、先说结论

**Jailhouse 和 Bao 在当前这台 VMware 虚拟机上跑不起来，这不是配置问题。**

两个都要求 CPU 暴露硬件虚拟化扩展，并且由它们自己接管物理核、物理内存映射和中断：

| | Jailhouse | Bao |
|---|---|---|
| 本质 | Linux 内核模块，加载后 Linux 变成 root cell | 从零写的 type-1 hypervisor，先于任何 OS 启动 |
| 需要的硬件 | Intel VT-x / AMD-V | ARM EL2（arm64）、或 x86 VMX/SVM |
| 支持的平台 | x86（成熟）、arm64（部分板卡） | **只有 ARMv8-A / ARMv7-A / ARMv8-R / RISC-V / Tricore / RH850，没有 x86** |
| 代码量 | 较大，含 Linux 侧驱动 | 约一万行，TCB 很小 |
| 通信机制 | ivshmem（共享内存 + doorbell 中断） | IVC（inter-VM communication，同样是共享内存 + 通知） |

当前环境实测：

```
CPU           Intel Core i5-7300HQ（宿主，支持 VT-x）
guest flags   hypervisor        ← 只有这一位，没有 vmx / svm
/dev/kvm      不存在
内存          7.4 GB / 4 vCPU
```

`vmx` 没暴露，Jailhouse 连初始化都过不去；Bao 则根本不支持 x86。**换任何一个静态分区 hypervisor 都是同一个结论**——它们的门槛是同一个。

### 怎么才能真正跑起来

**路一：在 VMware 里打开嵌套虚拟化。** 宿主的 i5-7300HQ 是支持 VT-x 的，缺的是 VMware 没把它透传给虚拟机。操作：

1. 关掉虚拟机（不是重启，是彻底关机）
2. 虚拟机设置 → 处理器 → 勾选「虚拟化 Intel VT-x/EPT 或 AMD-V/RVI」
3. 开机后 `grep -o vmx /proc/cpuinfo | head -1` 应当有输出

打开之后 **Bao 仍然不行**（它没有 x86 版本），**Jailhouse 理论可行但风险很高**：这台机器只有 4 个物理核，Jailhouse 要给每个 cell 分配独占物理核，root cell 加一个 inmate 至少吃掉 2 个，剩下的还要给宿主 Windows。实际能跑通的可能性不大，不建议作为主路径。

**路二：买一块 Bao 官方支持的板子。** Raspberry Pi 4、i.MX8M Plus、Zynq UltraScale+ 都在支持列表里，几百到几千块。这是唯一能拿到**真实时序数据**的路。

**路三（本仓库采用的）：把隔离和协议拆开。** 协议层今天就能验证，换成真 hypervisor 时只替换传输后端。

## 二、为什么隔离载体可以替换

大小脑架构真正依赖的东西只有三样：

1. **两个执行环境互不干扰** —— 大脑卡死不能拖累小脑
2. **一条确定的通信通道** —— 共享内存 + 门铃
3. **一套时间语义** —— 大脑发的是"什么时候该执行"，不是"立刻执行"

hypervisor 只是第 1 条的一种实现。用 `SCHED_FIFO` + 绑核 + `mlockall` 也能做到第 1 条的大部分效果——**代价是隔离强度不如分区，收益是今天就能测出真数据**。

代码里 `include/transport.h` 就是这个抽象：

```
shm:/robot-brain                 载体 A：POSIX 共享内存 + futex
ivshmem:/sys/bus/pci/devices/... 载体 B：Jailhouse ivshmem
```

上层（大脑、小脑）只调 `rb_transport_open()`，二进制不用重编。Jailhouse 的 ivshmem 和 Bao 的 IVC 都是"共享内存 + 通知"，形状一样，换的是设备路径。

## 三、角色划分

### 小脑：只回答"这一毫秒输出什么"

- 1 kHz 固定周期
- 收轨迹 → 三次 Hermite 插值 → 指令变化率限幅 → 关节限位 → 伺服模型
- 不做规划，不等大脑
- **大脑断了，它必须能自己安全停下来**

### 大脑：只给意图，不碰电机

- 20 Hz 规划，输出未来 200 ms 的轨迹片段（21 个路点，间隔 10 ms）
- 势场法避障 + 二连杆逆解
- 每条轨迹带两个时间戳：`t_gen`（决策时刻）和 `t0`（第一个点该执行的时刻）

### 最关键的一条边界：大脑不能直接控制电机

如果大脑直接发"转到 30 度"，网络一抖机器人就抽搐。正确做法是大脑发**轨迹片段**，小脑按自己的时钟插值执行。大脑掉线，小脑沿最后的速度减速走完，然后安全停机。

## 四、通信协议

共享内存 64 KB，布局固定（`include/shm_proto.h`）：

```
ctrl          模式、心跳、急停标志、序号
cmd_prod      大脑写的队列头        ← 单独一条缓存行
cmd_cons      小脑写的队列尾        ← 单独一条缓存行
stat_prod     小脑写的队列头
stat_cons     大脑写的队列尾
doorbell[2]   CMD / ESTOP 两个门铃
cmd[8]        轨迹片段，每条 21 点
stat[64]      状态帧
metrics       统计
```

四个索引各占一条 64 字节缓存行，避免伪共享——这是无锁队列能稳的前提。

队列是单生产者单消费者，所以不需要锁，也不需要 CAS 循环：

```c
static inline int rb_cmd_push(struct rb_shared *s, const struct rb_traj_seg *seg)
{
    uint32_t h = __atomic_load_n(&s->cmd_prod.head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&s->cmd_cons.tail, __ATOMIC_ACQUIRE);

    if (h - t >= RB_NSLOTS_CMD)
        return -1;
    s->cmd[h & (RB_NSLOTS_CMD - 1)] = *seg;
    __atomic_store_n(&s->cmd_prod.head, h + 1, __ATOMIC_RELEASE);
    return 0;
}
```

**时间戳只用小脑的时钟。** 大脑说"这条轨迹从 T 开始生效"，T 用小脑的时钟表示。小脑按自己的钟判断，不依赖两边时钟同步——这一条在换成 Jailhouse 之后尤其重要，因为两个 cell 的时钟不同源。

## 五、三层安全机制

| 触发条件 | 行为 | 状态 |
|---|---|---|
| 超过 250 ms 没收到新轨迹 | 沿最后一段指令速度平滑减速到零 | HOLD(2) |
| 超过 800 ms 没收到 | 保持当前位置 | SAFE(3) |
| 急停标志置位 | 立即保持 | SAFE(3) |
| 关节角越限 | 截断并计数 | FAULT(4) |
| 指令跳变 | 变化率限幅到 3 rad/s | 不变 |

指令变化率限幅是最后一道闸：不管指令从哪来（新轨迹、保持、急停），每周期最多变化 `DQ_MAX * dt`。没有这一层，大脑一上来给一个远处的目标，关节会被猛拽一下——实测这一层把启动瞬态的跟随误差从 0.45 rad 压到 0.057 rad。

## 六、实测数据

环境：openEuler 24.03 SP4，内核 6.12.0，4 vCPU（VMware，i5-7300HQ），7.4 GB。

### 6.1 周期抖动：四组对照

四组都跑同样负载——小脑 1 kHz、大脑 20 Hz、四个纯 CPU 占用进程。唯一变化的是小脑被怎么对待。

| 配置 | 周期均值 | 丢拍次数 | 丢拍率 |
|---|---|---|---|
| A 不绑核、普通调度、干扰不绑核 | 1429527 ns | 646 | 14.4% |
| B 绑 CPU1、普通调度、干扰压 CPU1 | 1242569 ns | 640 | 12.4% |
| C 绑 CPU1、FIFO 50、干扰压 CPU1 | 1164058 ns | 611 | 11.1% |
| D 绑 CPU1、FIFO 50、干扰在别的核 | 1256380 ns | 360 | **7.1%** |

"丢拍"指周期超过 1.5 倍（1.5 ms）。从 A 到 D 降了一半。

**但这组数据同时也说明虚拟机不是干这个的地方**：四组的周期最大值都在 70~146 ms 之间。那是宿主把 vCPU 挂起了，任何 OS 层的调度手段都救不了。同一台机器上早先测过 `sleep-over.c`，空载时一次定时器唤醒要几毫秒，也是这个原因。

### 6.2 大脑失联

三个场景，小脑配置相同（1 kHz，独占 CPU1，FIFO 50）。

| 场景 | 周期均值 | 丢拍率 | 缓冲余量 | 跟随误差 |
|---|---|---|---|---|
| 正常 | 1155752 ns | 5.5% | 196 ms | 0.0570 rad |
| 大脑变慢（每次推理 120 ms） | 1299826 ns | 11.1% | 88 ms | 0.0570 rad |
| 大脑猝死（kill -9） | 1483101 ns | 5.9% | 9 ms | 0.0570 rad |

场景 3 的状态时间线：

```
状态切换 0 -> 1 @ 454 ms      大脑接入，开始跟轨迹
状态切换 1 -> 2 @ 3655 ms     大脑被 kill，250 ms 后进保持
[小脑] 800 ms 没收到轨迹，安全停机
状态切换 2 -> 3 @ 4204 ms     进安全停机
```

**跟随误差三个场景完全一致，都是 0.0570 rad。** 这是整组数据里最重要的一行：大脑死了，机器人的指令没有跳变，它只是减速、停住。

### 6.3 大脑想得越久，动作会不会晚

| 推理耗时 | 缓冲余量 | 跟随误差 | 状态 |
|---|---|---|---|
| 0 µs | 198 ms | 0.0570 | 全程 RUN |
| 10 ms | 154 ms | 0.0570 | 全程 RUN |
| 40 ms | 167 ms | 0.0570 | 全程 RUN |
| 80 ms（超过 50 ms 规划周期） | **50 ms** | 0.0570 | 全程 RUN |

推理 80 ms 时大脑已经跟不上规划周期，缓冲从 198 ms 掉到 50 ms，但小脑始终没有进入 HOLD——**缓冲还够用，动作一直是连续的**。

丢拍率这一列在这组里是噪声（6.8%~10.4%，没有随推理耗时单调变化），因为宿主挂起 vCPU 造成的抖动远大于大小脑之间的耦合。**在虚拟机上，大脑对小脑的影响只能从"缓冲余量"看出来，从"周期"看不出来。** 这不是架构的问题，是平台噪声太高。

## 七、代码结构

```
robot-brain/
├── include/shm_proto.h        共享内存布局、消息格式、无锁队列（唯一的契约）
├── include/transport.h        传输层接口
├── transport/transport.c      按 spec 前缀挑后端
├── transport/transport_shm.c  载体 A：共享内存 + futex（已实测）
├── transport/transport_ivshmem.c 载体 B：Jailhouse ivshmem（未验证）
├── brain/brain.c              大脑：势场避障 + 逆解 + 轨迹生成
├── cerebellum/cerebellum.c    小脑：1 kHz 控制环 + 安全机制
├── test/spin.c                纯 CPU 占用，模拟别的活
├── test/run-demo.sh           演示：跑起来，然后拔掉大脑
├── test/run-jitter.sh         四组周期抖动对照
├── test/run-degrade.sh        大脑变慢 / 猝死
├── test/run-latency.sh        推理耗时对动作的影响
└── jailhouse/                 cell 配置模板与真机部署说明
```

## 八、上真机要改什么

1. **换传输后端**：`--transport ivshmem:/sys/bus/pci/devices/0000:00:0f.0`
2. **改 cell 配置**：`jailhouse/robot-cell.c` 里的内存区、CPU 集合、ivshmem 设备号
3. **给小脑换真 RTOS**：现在的小脑是 Linux 上的 SCHED_FIFO 进程，换成 FreeRTOS/Zephyr 的 inmate，控制环代码逻辑可以照搬（插值、限幅、看门狗都不依赖 Linux）
4. **时钟**：两个 cell 时钟不同源，`t_gen_ns` 的差值会失去意义，`cerebellum.c` 里已经加了 1 秒的合理性判断，超出就不计入

前三步做完，`include/shm_proto.h` 一行都不用改。
