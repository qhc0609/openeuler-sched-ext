# amp-rtos —— 一台普通 PC 上的 Linux + RTOS 混合部署

OpenAMP + openEuler，不插板卡，不用虚拟机。

---

## 一、先说结论

**没有板卡不是障碍。**

混合部署（AMP）真正需要的东西只有两样：

1. 一块 Linux 和 RTOS 都能读写的内存；
2. 一个能让一侧叫醒另一侧的门铃。

真实的 SoC 上这两样是现成的：一段 reserved-memory，一个 mailbox 寄存器。
但"现成"不等于"必须"——这两样在一台普通 PC 上都能造出来：

- **内存**：写个内核模块，用 `alloc_pages_exact()` 留出 4 MB 连续的物理页。
  两个进程各自 `mmap` 同一段物理页，看到的就是同一份字节。
- **门铃**：模块导出一个 `ioctl`，调用时唤醒正在 `poll()` 等着的对侧进程。

造好之后，把这块内存包装成一个 `virtio_device` 交给 OpenAMP，
**上游 rpmsg 的代码一行都不用改**。

```
        ┌──────────────────────┐        ┌──────────────────────┐
        │  Linux 侧（host）    │        │  RTOS 侧（remote）   │
        │  bin/amp_host        │        │  bin/amp_rtos        │
        │  ┌────────────────┐  │        │  ┌────────────────┐  │
        │  │ rpmsg（上游）  │  │        │  │ rpmsg（上游）  │  │
        │  ├────────────────┤  │        │  ├────────────────┤  │
        │  │ virtio/vring   │  │        │  │ virtio/vring   │  │
        │  ├────────────────┤  │        │  ├────────────────┤  │
        │  │ amp_transport  │◄─┼────────┼─►│ amp_transport  │  │
        │  └────────────────┘  │  同一份 │  └────────────────┘  │
        └──────────┬───────────┘  代码   └──────────┬───────────┘
                   │ mmap / ioctl / poll           │
        ┌──────────▼───────────────────────────────▼───────────┐
        │  kernel/amp_shm.ko                                   │
        │  4 MB 连续物理页 + 门铃（wait_queue）+ 计数          │
        └──────────────────────────────────────────────────────┘
```

---

## 二、为什么不是虚拟机

虚拟机那条路在这台机器上确实走不通：`lscpu` 里没有 `vmx`/`svm`，
`/dev/kvm` 也不存在。Jailhouse、KVM、Xen 这类方案都要 CPU 的虚拟化扩展。

但**混合部署本来就不需要虚拟化**。

- 虚拟机做的事是：把一颗物理核切出两份，让两个内核都以为自己独占一个核。
  这必须靠硬件辅助，否则每次特权指令都要陷出陷进，慢到没法用。
- AMP 做的事是：两个内核各占一颗核，各跑各的，中间只共享一段内存。
  它不需要隔离，只需要**一条通道**。

所以"没有 vmx"只否掉了前者，跟后者没关系。缺的那条通道第一节已经说了，能造。

---

## 三、第一层：把"硬件"造出来

`kernel/amp_shm.c`，328 行。

```c
g_shm  = alloc_pages_exact(AMP_SHM_SIZE, GFP_KERNEL | __GFP_ZERO);
g_phys = virt_to_phys(g_shm);
```

`alloc_pages_exact` 要的是**物理连续**，不是虚拟连续——因为要 `remap_pfn_range`
映射给用户态，两边的虚拟地址可以不同，物理页必须是同一批。

内存布局是写死的常量，不靠协商（`include/amp_layout.h`）：

| 偏移 | 大小 | 用途 |
|------|------|------|
| `0x000000` | 4 KB | 控制块：magic、virtio status、特性位、门铃计数 |
| `0x001000` | 64 KB | vring 0 —— Linux 发、RTOS 收 |
| `0x011000` | 64 KB | vring 1 —— RTOS 发、Linux 收 |
| `0x021000` | 余下 3.9 MB | rpmsg 数据缓冲池 |

真实板卡上这些地址来自设备树，这里来自头文件，性质一样：两侧都当常量用。

门铃就是一个 `wait_queue`：

```c
static int amp_kick(struct amp_file *f, int target)
{
	struct amp_file *peer = g_files[target];

	if (!peer)
		g_early[target] = true;      /* 对侧还没起来，绑定后补投 */
	else {
		atomic_set(&peer->pending, 1);
		wake_up_interruptible(&peer->wq);
	}
	...
}
```

`poll()` 把 `pending` 取走并返回 `EPOLLIN`。对用户态来说，这和等一个中断
没有区别——OpenAMP 需要的"有事了，去看 vring 的 used 环"这个语义，已经齐了。

**这一层先单独验过再往上搭。** `test/amp_shm_test.c` 不起任何协议，
父子两个进程只交换门铃：

```
[role 0] phys=0x192c00000 size=4194304
[role 1] phys=0x192c00000 size=4194304
[rtos] 看到的 magic=0x414d5043，和父进程写的一样
100 次往返：最小 11002 ns，中位 116667 ns，p99 349122 ns，最大 349122 ns
```

这一步的意义在后面调协议的时候体现得很清楚：出问题时，先能确定通道是好的。

---

## 四、第二层：接上 OpenAMP

`common/amp_transport.c`，233 行，两侧编同一份。

OpenAMP 的 virtio 层只认一个东西：`struct virtio_dispatch` 里的函数指针。

```c
static const struct virtio_dispatch amp_virtio_ops = {
	.create_virtqueues = amp_create_virtqueues,
	.get_status        = amp_get_status,      /* 读控制块 */
	.set_status        = amp_set_status,      /* 写控制块 */
	.get_features      = amp_get_features,    /* 只宣告 NS */
	.notify            = amp_notify,          /* ioctl 敲门铃 */
	...
};
```

11 个函数，全部是"读写共享内存里的某个字段"或者"敲一下门铃"。
没有一行和硬件寄存器打交道，所以这段代码在板卡上和在 PC 上是同一份。

两个容易踩的地方，都是 OpenAMP 的隐式约定：

**（1）描述符里存的是"物理地址"，不是指针。**

```c
static metal_phys_addr_t g_physmap[1] = { 0 };
#define AMP_PAGE_SHIFT	22	/* 4 MB，整块共享内存当一页 */
metal_io_init(&t->io, shm, g_physmap, AMP_SHM_SIZE, AMP_PAGE_SHIFT, 0, NULL);
```

两个进程各自 mmap，虚拟地址不同。用一张单页表把"物理地址"定义成
**相对共享内存起点的偏移**，两侧算出来就是同一个数，指针各自翻译。

这里如果传 `NULL`，`metal_io` 做不了换算，描述符里的地址全是无效值，
对侧会往共享内存开头写——第一版就是这么把控制块的 `magic` 踩成 `0x00000400` 的。

**（2）角色决定 vring 编号。**

| | rvq（收） | svq（发） |
|---|---|---|
| Linux（`VIRTIO_DEV_DRIVER`） | vrings[0] | vrings[1] |
| RTOS（`VIRTIO_DEV_DEVICE`） | vrings[1] | vrings[0] |

设备角色在 `rpmsg_init_vdev()` 里会一直等，直到 host 把
`VIRTIO_CONFIG_STATUS_DRIVER_OK`（0x04）写进 status。
所以启动顺序是**先 Linux，后 RTOS**。

---

## 五、第三层：RTOS 侧

`rtos/main.c`，264 行，跑的是 FreeRTOS V11.1.0 官方 POSIX 端口。

这个端口的意义就是"没有板子的时候先跑通逻辑"：每个任务是一个 pthread，
tick 由一个专门的定时线程用信号投递，任务切换也靠信号。但调度器本身是真的——
1 kHz tick、8 个优先级、抢占、时间片，都是 FreeRTOS 自己的代码在跑。

三个任务：

| 任务 | 优先级 | 干什么 |
|------|--------|--------|
| `amp_task` | **1（最低）** | 等门铃 → `virtqueue_notification()`，对应真板卡上的 mailbox 中断 |
| `control_task` | 3 | 1 kHz 控制环，算正弦设定值，同时记录每一拍的抖动 |
| `stats_task` | 2 | 每秒打一行状态 |

**优先级不能随便排，这是这个端口的一个硬约束。**

任务阻塞在 `poll()` 上时，FreeRTOS 并不知道它"在等"——调度器眼里它就是在正常运行。
所以等门铃的任务如果优先级最高，它会把 CPU 一直占住，控制环一次都轮不到。
第一版就是这么写的，跑出来两秒内 `tick_count` 是 0，控制环一拍没跑。

把它放到最低之后，每来一个 tick，控制环和统计任务就绪，`vTaskSwitchContext`
选中它们，端口用信号把正卡在 `poll()` 里的 `amp_task` 挂起。门铃来了再唤醒，
延迟还是几十微秒。

回显端点就是最朴素的那种：

```c
static int echo_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		   uint32_t src, void *priv)
{
	rpmsg_send(ept, data, len);   /* 收到什么原样发回去 */
	g_rt->echo_count++;
	return 0;
}
```

---

## 六、怎么跑

依赖：openEuler（内核 6.12）、`open-amp` 和 `libmetal` 源码、
`FreeRTOS-Kernel` 源码、`sysfsutils-devel`。

```bash
make                      # 编内核模块 + 两侧程序 + 两个小工具
insmod kernel/amp_shm.ko  # 造"硬件"
bash test/run-all.sh      # 跑完整套测量，结果写进 results/
```

单跑一次往返：

```bash
bash test/run-amp.sh 2000              # RTOS 独占 CPU3（默认）
MODE=shared bash test/run-amp.sh 2000  # RTOS 不绑核，和 Linux 抢
```

`Makefile` 里的 `patch-rtos-kernel` 会给 FreeRTOS 端口打一个单点补丁，
原因见第八节。

---

## 七、实测

这台机器：4 vCPU 虚拟机，openEuler，内核 6.12.0，clocksource = tsc。
Linux 侧跑在 CPU0-2，RTOS 侧 `taskset -c 3 chrt -f 50`。

### 7.1 通道本身（`results/shm-test.log`）

只有门铃，没有任何协议：100 次往返，最小 11.0 µs，中位 **116.7 µs**，最大 349 µs。
两侧看到同一个物理地址 `0x192c00000`，magic 一致，门铃计数 100/100。

这就是"造出来的硬件"的全部开销：一次 `ioctl` 进内核 + 一次 `poll()` 醒来，
往返两个来回。

### 7.2 rpmsg 往返（`results/run-spin-0.log`，2000 条一问一答）

```
发送 2000 条，收回 2000 条
最小   3816 ns
中位  57064 ns
p90  128427 ns
p99  318733 ns
最大 1219241 ns
平均  68719 ns
一问一答 3014 条/秒
```

**中位 57 µs** 是这条通道的真实成本：两次门铃（两次 `ioctl` + 两次进程唤醒）、
两次 vring 操作、两次 rpmsg 打包解包。单向约 29 µs。

最小 3.8 µs 那几条是门铃投递时对侧正好在跑，没进睡眠队列。

### 7.3 平台能给到的定时精度（`results/tick-floor-*.log`）

这一组和 AMP 无关，量的是这台机器本身：一个裸 pthread，`SCHED_FIFO 50`，
绝对时刻 `clock_nanosleep`，1 kHz。

| 自旋窗口 | p50 | p99 | < 50 µs | < 1 ms |
|---|---|---|---|---|
| 0（纯睡） | 522.4 µs | 1307.7 µs | 4.6% | 91.8% |
| 300 µs | 233.5 µs | 3587.2 µs | 32.8% | 98.3% |
| 700 µs | 0.1 µs | 4378.6 µs | 66.7% | 98.1% |
| 900 µs | 0.1 µs | 2959.5 µs | 81.4% | 98.7% |

纯睡的话，1 kHz 的拍子**中位数晚 522 µs**——这台虚拟机上空闲的 vCPU 会被挂起，
定时器到期后把 vCPU 叫回来要几百微秒。

这不是 Linux 的问题，`/proc/timer_list` 里 hrtimer 分辨率是 1 ns；
是虚拟化本身的问题。同样的代码在一台物理机上，纯睡就能到几十微秒。

### 7.4 精度和延迟在抢同一颗核（`results/run-spin-*.log`）

把 7.3 那个自旋窗口用到 FreeRTOS 的 tick 上，代价立刻出现：

| 自旋 | RTT 中位 | RTT p99 | RTT 平均 | 控制环 < 50 µs | 迟到率 |
|---|---|---|---|---|---|
| 0 | 57.1 µs | 318.7 µs | 68.7 µs | 21.4% | 23.9% |
| 300 µs | 60.6 µs | 452.6 µs | 104.4 µs | 25.0% | 23.8% |
| 700 µs | 178.6 µs | 836.8 µs | 333.5 µs | 51.1% | 11.6% |
| 900 µs | 781.7 µs | 1866.9 µs | 599.6 µs | 69.0% | 5.5% |

一眼能看出来的两件事：

- **自旋窗口越大，拍子越准**：迟到率从 23.9% 降到 5.5%；
- **自旋窗口越大，门铃越慢**：RTT 中位从 57 µs 涨到 782 µs。

原因是同一颗核上，tick 线程和等门铃的任务在抢 CPU。自旋 700 µs，
就是每毫秒里有 0.7 毫秒被 tick 线程占着。

**真板卡上这两件事不冲突**：tick 是硬件定时器，不占 CPU；门铃是中断，
直接打断当前任务。所以真板卡上"拍子准"和"响应快"可以同时拿到。
这台机器上二者互斥，是"RTOS 的定时器退化成了一个 Linux 线程"造成的——
这是端口的代价，不是 AMP 的代价。

### 7.5 隔核与不隔核（`results/run-isolated.log` / `run-shared.log`）

自旋窗口固定 300 µs：

| 模式 | RTT 中位 | RTT p99 | RTT 平均 | RTT 最大 | 控制环 < 50 µs |
|---|---|---|---|---|---|
| isolated（CPU3 + FIFO 50） | 59.0 µs | 383.5 µs | 91.4 µs | 751.6 µs | 36.0% |
| shared（不绑核） | 58.8 µs | 362.8 µs | 78.6 µs | 2593.5 µs | 16.9% |

**中位数和 p99 一样，差别只在尾巴上**：最坏情况 752 µs 对 2593 µs，
控制环落在 50 µs 以内的拍数翻了一倍多。

原因不难解释：这个实验里 Linux 侧几乎不干活——它只是把消息转出去然后等回复，
三个核空着。没有竞争，隔不隔核对常态延迟自然没影响；
隔核真正挡住的是偶发的大毛刺。

要让这一组更有说服力，得让 Linux 侧真的忙起来（编译、跑负载），这次没做。

---

## 八、踩过的五个坑

### 1. `struct virtqueue` 末尾是零长数组

```c
struct virtqueue {
	...
	struct vq_desc_extra vq_descx[0];   /* 零长 */
};
```

上游的约定是调用方额外分配 `num_descs` 个 `vq_desc_extra` 跟在结构体后面。
按普通成员声明 `struct virtqueue vqs[2]` 再取地址，`virtqueue_add_buffer`
写 cookie 时会越过队列末尾，踩到相邻的 `io.ops` 上——现象是
`rpmsg_init_vdev()` 里 SIGSEGV 在 `metal_io_block_set`，地址 `0x1`。

```c
#define AMP_VQ_BYTES \
	(sizeof(struct virtqueue) + \
	 (size_t)AMP_NUM_DESCS * sizeof(struct vq_desc_extra))
t->vqs[i] = calloc(1, AMP_VQ_BYTES);
```

### 2. `metal_io_init` 的 physmap 不能传 NULL

见第四节第（1）点。传 NULL 的后果不是崩溃，是**静默的数据错乱**：
描述符地址算错，对侧往偏移 0 写，控制块 magic 被覆盖。
比崩溃难查得多。

### 3. `rpmsg_send()` 成功时返回字节数，不是 0

```c
/* rpmsg_virtio_send_offchannel_nocopy() 最后一行 */
return len;
```

第一版写的是 `if (ret == 0) 成功`，于是每一条"发送成功"都被当成失败，
程序打印 `rpmsg_send 失败 7` 就退出了——7 是 `"ping 0"` 加结尾 0 的长度。

判 `ret > 0`。

### 4. 等门铃的任务必须是最低优先级

见第五节。这条不修，控制环永远跑不起来，而且现象很隐蔽：
消息全通，日志全对，只是 `tick_count` 一直是 0。

### 5. FreeRTOS 官方 POSIX 端口的 tick 不准

```c
pthread_kill( thread->pthread, SIGALRM );
usleep( portTICK_RATE_MICROSECONDS );   /* 相对睡眠 */
```

两个问题叠在一起：`usleep` 是相对睡眠会累积漂移；这台机器上
`usleep(1000)` 实测要 2073 µs。结果 1 kHz 的 tick 实际只有 685 Hz。

`rtos/patch-freertos-tick.py` 把它改成绝对时刻的 `clock_nanosleep`
加一个自旋窗口（见 7.3），`make` 时会自动打上，幂等。

---

## 九、这台机器测不出来的东西

写结论之前先划清楚边界，免得把端口的限制当成 AMP 的性质：

- **绝对延迟不能外推。** 这是一台 4 vCPU 虚拟机，vCPU 会被宿主机挂起，
  偶发几十到一百多毫秒的整体停顿（`run-spin-0.log` 里控制环最大偏差 131 ms）。
  这里能说的是"这条通道在这台机器上中位 57 µs"，不能说是"AMP 的延迟是 57 µs"。
- **1 kHz 的相位精度不能外推。** 7.3 那张表是虚拟机的下限，不是 AMP 的下限。
- **隔核的收益测不出来。** 见 7.5，Linux 侧太闲。
- **RTOS 的实时性只到"能跑"这一层。** FreeRTOS 的调度器是真的，
  但它的 tick 和中断在这台机器上都退化成了用户态线程，和真板卡不是一回事。

能确定的是：**这套代码在一台没有板卡、没有 vmx 的普通 PC 上，
让 Linux 内核和一个真的 FreeRTOS 调度器通过 OpenAMP 的 rpmsg 通了 2000 条消息，
一条没丢。** 通道是可以这样造的。

---

## 十、文件清单

```
include/amp_layout.h        两侧共用的内存布局和 ABI
kernel/amp_shm.c            内核模块：4 MB 连续物理页 + 门铃（328 行）
common/amp_transport.c      virtio 传输层，两侧同一份（233 行）
common/amp_transport.h
host/amp_host.c             Linux 侧：host 角色 + 往返测量（332 行）
rtos/main.c                 RTOS 侧：remote 角色 + 1 kHz 控制环（264 行）
rtos/FreeRTOSConfig.h       1 kHz tick，8 优先级，抢占
rtos/patch-freertos-tick.py 修端口 tick 的单点补丁
test/amp_shm_test.c         通道自检：只有门铃，没有协议
test/tick-floor.c           裸 1 kHz 定时器精度（平台下限）
test/run-amp.sh             跑一次往返
test/run-all.sh             跑完整套测量
results/                    全部原始日志
```
