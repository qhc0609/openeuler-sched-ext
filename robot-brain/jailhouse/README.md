# 真机部署：Jailhouse 版大小脑

**这里的所有内容都没有验证过。** 原因写在下面第一节，先看完再动手。

## 一、为什么在虚拟机上跑不了

Jailhouse 的启动过程是这样的：

1. 作为 Linux 内核模块加载（`jailhouse.ko`）
2. 它接管所有 CPU，把当前的 Linux 降级成一个 cell（root cell）
3. 之后新建的每个 cell 拿到**独占的物理核**和**独占的物理内存区间**
4. 它自己进入 VMX root 模式，用 EPT 做两阶段地址翻译

第 2 步和第 4 步要求 CPU 暴露 `vmx`（Intel）或 `svm`（AMD）。当前的 VMware 虚拟机：

```
$ grep -o -m1 -E "vmx|svm" /proc/cpuinfo
（无输出）
$ ls /dev/kvm
（不存在）
$ grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep hypervisor
hypervisor
```

只有 `hypervisor` 这一位，说明这台机器自己就是别人的 guest，而且宿主没有把虚拟化扩展透传下来。**这不是配置错误，是宿主设置。**

同一台机器上也查过 Bao：它支持 ARMv8-A / ARMv7-A / ARMv8-R / RISC-V / Tricore / RH850，**没有 x86 版本**，所以连试都不用试。

## 二、想真跑，先解决硬件

### 方案 A：打开嵌套虚拟化

宿主 CPU 是 i5-7300HQ，支持 VT-x，缺的只是 VMware 没透传。

1. 虚拟机**彻底关机**（不是重启）
2. 虚拟机设置 → 处理器 → 勾选「虚拟化 Intel VT-x/EPT 或 AMD-V/RVI」
3. 开机后 `grep -o vmx /proc/cpuinfo | head -1` 应当有输出

**但即使打开了也不建议走这条路**：这台机器只有 4 个物理核，Jailhouse 要给 root cell 和 inmate 各分至少一个独占核，宿主 Windows 还要用。实测环境里跑出来的时序数据会被宿主调度污染得一塌糊涂，没有参考价值。

### 方案 B：买一块支持的板子（推荐）

Bao 和 Jailhouse 都支持、且便宜的板子：

| 板子 | 价格量级 | 说明 |
|---|---|---|
| Raspberry Pi 4 | 几百元 | Jailhouse 和 Bao 都支持，社区资料最多 |
| i.MX8M Plus EVK | 一两千元 | Bao 官方支持，工业场景常用 |
| Zynq UltraScale+ ZCU102 | 较贵 | 两个都支持，适合做严肃验证 |

有了真板子，`test/` 下四组测试的脚本可以照搬，只需要把 `--transport` 换掉。

## 三、真机上的步骤

### 1. 确认硬件支持

```bash
# x86
grep -o -m1 -E 'vmx|svm' /proc/cpuinfo
# ARM64：需要 EL2，看 dmesg 里有没有 "CPU: ... EL2"
dmesg | grep -i 'virtualization\|EL2'
```

### 2. 编译并加载 Jailhouse

```bash
git clone https://github.com/siemens/jailhouse
cd jailhouse
make
sudo insmod hypervisor/jailhouse.ko
sudo tools/jailhouse hardware check    # 会告诉你哪些资源能用
```

### 3. 生成 root cell 配置

```bash
sudo tools/jailhouse config create -o rootcell.cell rootcell.c
```

从生成的配置里抄出：可用的 RAM 区间、PCI 设备的 BDF、GIC 地址、串口地址。

### 4. 填 `robot-cell.c`

`robot-cell.c` 里每个 `TODO` 都要按实际板子替换。重点两处：

- **`cpus`** —— 小脑独占的物理核，位图，不能和 root cell 重叠
- **`mem_regions[3]`** —— 通信区，64 KB，必须 4 KB 对齐，且**在 root cell 配置里也要出现一次，指向同一段物理地址**

### 5. 在 root cell 里创建 ivshmem 设备

```bash
sudo tools/jailhouse cell create robot.cell
lspci | grep -i shmem        # 找到 ivshmem 的 BDF
```

把它填回 `robot-cell.c` 的 `pci_devices[0].bdf`。

### 6. 准备小脑镜像

小脑要跑在 inmate 里，需要一个裸机可执行的镜像。两条路：

- **FreeRTOS / Zephyr**：官方有 ivshmem 的驱动示例，改一改就能用
- **从现有代码裁**：`cerebellum.c` 里的插值、限幅、看门狗、状态机都不依赖 Linux，把 `clock_nanosleep` 换成 RTOS 的定时器、`rb_cmd_pop` 照搬即可

镜像要链接到 `mem_regions[0].phys_start`。

### 7. 跑起来

```bash
# root cell（大脑）
bin/brain --transport ivshmem:/sys/bus/pci/devices/0000:00:01.0 --hz 20

# inmate（小脑）
sudo tools/jailhouse cell load robot-cerebellum cerebellum.bin
sudo tools/jailhouse cell start robot-cerebellum
```

`--transport` 换成 ivshmem 之后，`include/shm_proto.h` 一行都不用改——这就是当初把传输层抽出来的原因。

## 四、换成 Bao 要改什么

Bao 的 IVC（inter-VM communication）和 ivshmem 是同一个形状：一段共享内存 + 一个通知机制。要改的只有两处：

1. 设备路径：Bao 的共享内存区域在 VM 配置（dtsi / JSON）里定义，guest 侧通过 `bao-ipc` 驱动暴露的设备节点访问
2. doorbell：ivshmem 是写 BAR0 触发 MSI，Bao 是写 IVC 的通知寄存器

`transport_ivshmem.c` 里的 `notify` 和 `wait` 两个函数换掉就行，其余不动。

但 Bao 没有 x86 版本，要试它必须先有 ARM 板子。

## 五、时钟问题

两个 cell 的 `CLOCK_MONOTONIC` 不同源，`t_gen_ns` 的差值会变成无意义的数。

`cerebellum.c` 里已经处理了：

```c
if (gen > 0 && t_end > gen && t_end - gen < 1000000000ull) {
    /* 差值在 1 秒内才认为是同一时钟，否则不计入 */
}
```

上真机后如果发现计划年龄一直是 0，说明两边时钟差太多，需要加一个偏移估计（类似 PTP 的做法：来回打时间戳，取最小往返的一半作为偏移）。
