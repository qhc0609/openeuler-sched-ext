# Jailhouse 版大小脑：多系统混合部署

## 一、先把"多系统"这件事说清楚

Linux 上跑两个进程，**不是**多系统。两个进程共用一套页表、一个内核、一个调度器。小脑那个线程被抢，大脑也跟着卡——因为它们是同一个系统里的两个对象。

Jailhouse 做的是另一件事：**把一台物理机切成几个互不相干的系统**。

| | 两个进程（`transport_shm.c`） | 两个 cell（`transport_ivshmem.c`） |
|---|---|---|
| 小脑是什么 | Linux 里的一个线程 | **另一个操作系统**，有自己的内核 |
| CPU | 和大脑共用 4 个核，靠调度器分 | 独占物理核，别的 cell 碰不到 |
| 内存 | 同一套页表，靠权限位隔离 | 独立的物理内存区间，EPT 隔离 |
| 互相影响 | 小脑卡住，大脑一起卡 | 小脑崩了，大脑继续跑 |
| 唯一的共享 | 共享内存段 | 一段 ivshmem，就 64 KB |
| 出问题怎么查 | 看同一个 dmesg | 各看各的串口 |

所以"混合部署"这四个字，在 Jailhouse 里的具体含义是：

```
物理核 0,1  ──►  Cell 0 (root)  ──►  openEuler + 大脑进程
物理核 2    ──►  Cell 1         ──►  FreeRTOS/Zephyr + 小脑控制环
物理内存    ──►  各自一段，互不可见
0x3e000000  ──►  两边都映射这一段，64 KB  ← 这是唯一的通道
```

那段 64 KB 在两个 cell 的配置里各写一次，指向同一个物理地址。**这就是"共享"的全部含义**——hypervisor 不转发任何消息，它只是允许两个系统看到同一块内存。

大脑往这段内存里写轨迹，然后写 ivshmem 的 BAR0 触发一个中断；小脑收到中断，从同一段内存里读。反过来也一样。

## 二、现在卡在哪

`check-host.sh` 在当前开发机上的实际输出：

```
  1) 硬件虚拟化扩展
  [NO]   vmx / svm                 都没有 —— Jailhouse 无法初始化
  2) 虚拟化嵌套状态
  [!]    hypervisor 标志            这台机器自己就是 guest
  3) 核数预算
  [OK]   可用核数                  4 个
  4) 内存预算
  [OK]   内存                      7430 MB
  5) 构建依赖
  [OK]   gcc / make / git / python3 / 内核头文件
  结论：NO-GO —— 有 1 项硬性条件不满足
```

宿主是 i5-7300HQ，支持 VT-x，但 VMware 没把它透传进虚拟机。这一项不是装软件能解决的。

另外这台机器连不上 github，Jailhouse 源码都取不到。

## 三、怎么解决

### 方案 A：打开嵌套虚拟化（5 分钟）

1. 虚拟机**彻底关机**（不是重启）
2. 虚拟机设置 → 处理器 → 勾选「虚拟化 Intel VT-x/EPT 或 AMD-V/RVI」
3. 开机后 `bash jailhouse/check-host.sh` 应当从 NO-GO 变 GO

⚠️ 即使变成 GO，这台机器只有 4 个物理核，root cell 要 2 个、inmate 要 1 个，宿主 Windows 还要用。**能跑起来，但时序数据不可信。**

### 方案 B：换一块支持的板子（推荐）

| 板子 | 价格量级 | 说明 |
|---|---|---|
| Raspberry Pi 4 | 几百元 | Jailhouse / Bao 都支持，资料最多 |
| i.MX8M Plus EVK | 一两千元 | 工业场景常用 |
| Zynq UltraScale+ ZCU102 | 较贵 | 做严肃验证用 |

### 方案 C：x86 工控机

任何带 VT-x 的 Intel 工控机都行，Jailhouse 在 x86 上是最成熟的。二手的几百块。

## 四、硬件到位后怎么跑

```bash
bash jailhouse/check-host.sh        # 先确认 GO
sudo bash jailhouse/deploy.sh build      # 取源码 + 编译
sudo bash jailhouse/deploy.sh rootcell   # 生成 root cell 配置
#   → 把生成的配置里的内存区、PCI BDF、GIC 地址抄进 robot-cell.c
sudo bash jailhouse/deploy.sh up         # 建 ivshmem、建 cell、启动小脑
sudo bash jailhouse/deploy.sh brain      # 在 root cell 里跑大脑
sudo bash jailhouse/deploy.sh down       # 拆掉
```

`deploy.sh` 里每一步都做了检查，失败会停在原地并说清原因。

## 五、小脑镜像从哪来

小脑跑在 inmate 里，需要一个裸机可执行的镜像。两条路：

**路一：用现成 RTOS。** FreeRTOS 和 Zephyr 都有 Jailhouse 的移植，官方 `inmates/` 目录里有示例。把 `cerebellum.c` 里的控制环搬过去——插值、限幅、看门狗、状态机都不依赖 Linux，只要把 `clock_nanosleep` 换成 RTOS 的定时器、`rb_cmd_pop` 照搬。

**路二：裸机。** 直接写一个 `-ffreestanding -nostdlib` 的程序，自己配 IDT 和 APIC 定时器。Jailhouse 的 `inmates/demo.c` 是最小参考。

两条路都只需要改 `transport` 这一层：把 `transport_shm.c` 换成 `transport_ivshmem.c`，`include/shm_proto.h` 一行不用改。

## 六、和 Bao 的关系

Bao 的 IVC 和 ivshmem 是同一个形状（共享内存 + 通知）。要换过去只改两处：设备路径、doorbell 的写法。

但 **Bao 没有 x86 版本**——官方支持 ARMv8-A / ARMv7-A / ARMv8-R / RISC-V / Tricore / RH850。想试 Bao 必须先有 ARM 板子。

## 七、这份代码的验证状态

| 部分 | 状态 |
|---|---|
| `check-host.sh` | 已在本机跑过，正确报出 NO-GO |
| `deploy.sh` | 未跑过（没有硬件） |
| `robot-cell.c` | 模板，不能直接编译，要按板子填 TODO |
| `transport_ivshmem.c` | 编译通过，未运行 |
| `cerebellum.c` / `brain.c` / `shm_proto.h` | 已在载体 A 上实测 |

前四项不是"应该能跑"，是"没跑过"。上板子后大概率要现场调，尤其 `robot-cell.c` 里的物理地址和中断号。
