Linux 在机器人里到底管什么

—— 第一个控制程序：用户态应用、系统调用、线程、驱动，各在哪一行（1.2）


一、这一篇要跑起来的东西

上一篇拆了"大脑"和"小脑"。这一篇不管大脑，只往下看一层：**一个每毫秒算一次关节角、把结果交给电机的程序**。

场景还是那台二连杆臂。程序叫 `first-bot`，控制循环 1 kHz，每拍算两个关节角，通过 `/dev/robotmotor` 写下去。跑起来，终端里打出这么几行：

    == 单线程：感知写在控制循环里 ==
      拍了 5000 次，超过 1.5 倍周期 49 次（0.98%）
      周期 p50 992.2 us，p99 1499.4 us，最大 14827.4 us
      每拍 write() 平均 3.81 us（占 1 ms 预算的 0.38%）

这是其中一次运行。后三行分别是：打了几拍、超时的有几拍、周期分布，以及每拍花在"算指令 + 写下去"上的时间。

这就是一个能跑的机器人控制程序。下面看它用到的四样 Linux 的东西，各自在哪一行。


二、四样东西分别在哪

| Linux 的东西 | 在这个程序里是哪几行 | 它解决什么 |
|---|---|---|
| 用户态应用 | `main()` 到 `report()`，整个程序 | 不用内核权限就能跑；算错了、崩了，系统还在 |
| 系统调用 | `clock_nanosleep()` 定拍，`write()` 下发电机指令 | 唯一能越过 ring 3 / ring 0 那道门槛的通道 |
| 线程 | `pthread_create()` 起控制线程，`make_rt()` 绑核 | 让 1 kHz 的活和 30 Hz 的活互不拖累 |
| 驱动 | `motor.ko` 里的 `motor_write()` | 用户态碰不到设备寄存器和 IO 端口，得有人在内核里代劳 |

四样东西不是四个概念，是同一个程序里的四段代码。下面一段一段看。


三、控制循环，二十行

`first-bot.c` 里真正跑在 1 kHz 上的，是这个循环：

    while (n < (unsigned long long)secs * hz) {
        next += period;
        sleep_until(next);            /* 系统调用：睡到"下一拍的绝对时刻" */
        t0 = now_ns();

        if (sensor_us > 0 && n % 33 == 0)
            busy_us(sensor_us);       /* 感知：每 33 拍做一次，相当于 30 Hz */

        cmd.seq = seq++;
        cmd.q1_mrad = (int)(sinf((float)n * 0.001f) * 800.0f);
        cmd.q2_mrad = (int)(cosf((float)n * 0.001f) * 500.0f);

        /* 这一行是系统调用：用户态 → 内核 → 驱动 */
        if (write(fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
            break;

        t1 = now_ns();
        per = t1 - prev;
        if (per > period * 3 / 2)
            miss++;
        prev = t1;
        n++;
    }

四件事：**等这一拍、算指令、写下去、记数**。顺序不能换。

`sleep_until()` 里用的是 `clock_nanosleep` 的 `TIMER_ABSTIME`——睡到"第 n 拍的绝对时刻"，不是"睡 1 毫秒"。后者每拍差几十微秒，一万拍之后就差出半秒。定周期这件事，绝对时刻和相对时长的区别，就是能不能长期不漂。

`now_ns()` 里是 `clock_gettime`。这一句看着像系统调用，其实不是——glibc 把它做成了 vDSO，用户态直接读一页内核映射过来的共享内存，压根不进内核。

`write(fd, &cmd, 12)` 这一句是真的系统调用。它要把 12 个字节从你的地址空间送到内核里，内核再转给 `motor.ko`。

那这一句到底多贵？


四、一次 write 要多久

`syscall-cost.c` 量了五种调用，每种 20 万次：

| 调用 | 走哪条路 | 单次耗时 |
|---|---|---|
| `clock_gettime` | vDSO，用户态读共享页 | 21.8 ns |
| `syscall(SYS_getpid)` | 进内核，什么都不干 | 838.2 ns |
| `clock_nanosleep(0)` | 进内核，睡 0 纳秒 | 8839.8 ns |
| `write(/dev/null)` | 进内核，走文件系统 | 932.0 ns |
| `write(/dev/robotmotor)` | 进内核，再进自家驱动 | 942.6 ns |

第二行是"过门槛"的底价：**838 纳秒**。什么都不干，就是从 ring 3 进 ring 0 再回来。

第三行要多说一句。`clock_nanosleep(0)` 是"睡 0 纳秒、立刻返回"，却花了 8.8 微秒，是 `getpid` 的十倍——因为它还要走一遍调度器：挂定时器、选下一个任务、切上下文。**这 8.8 微秒不在 `write` 里，在 `sleep_until` 里**，而机器人每拍都要睡一次。

第四、五行跟第二行差 94 和 104 纳秒。也就是说，多走一个文件系统只要 94 纳秒，再进我们自己的驱动只要 10 纳秒。

**943 纳秒占 1 毫秒的 0.094%。** 一个 1 kHz 的控制循环，把指令交给驱动这件事，吃掉的是预算的千分之一。

所以"系统调用很慢"这句话，在 1 kHz 这个量级上不成立。它慢，但不是在这里慢。


五、驱动那一侧

`/dev/robotmotor` 是 `motor.ko` 造出来的。用户态 `write` 进来，落到这个函数：

    static ssize_t motor_write(struct file *filp, const char __user *buf,
                               size_t len, loff_t *off)
    {
        struct motor_cmd cmd;

        if (len != sizeof(cmd))
            return -EINVAL;
        if (copy_from_user(&cmd, buf, sizeof(cmd)))
            return -EFAULT;

        g_last = cmd;
        atomic_inc(&g_writes);

        return len;
    }

`copy_from_user` 是唯一不能省的一行。`buf` 是用户态传进来的地址，内核**不能直接解引用**：两边的页表不是一套，而且这个地址可能是野指针、可能指向内核自己的内存。`copy_from_user` 先查它确实在用户空间里，再拷，拷不动就返回 `-EFAULT`。少了这一行，一个恶意程序就能让内核读任意地址。

设备是 `motor_init()` 四步造出来的：`alloc_chrdev_region` 申请设备号（这台机器上是 243），`cdev_add` 挂上 `file_operations`，`class_create` + `device_create` 让 udev 在 `/dev` 下建节点。用户态 `open()` 这个路径，内核按 243 找到 `motor_fops`，之后每一次 `write` 就都进了 `motor_write`。

这就是"驱动"在 Linux 里的全部含义：**一个把文件操作翻译成硬件动作的函数表**。


六、线程还是进程

一个机器人程序至少有两件节奏完全不同的事：控制 1 kHz，感知 30 Hz。放一起还是分开？

`first-bot` 有三种结构，`--mode` 选：`single` 一个线程全干完，感知直接写在控制循环里；`thread` 控制线程 1 kHz 绑 CPU2，感知在主线程 30 Hz 绑 CPU1；`process` `fork()` 一个子进程跑控制，父进程跑感知。

"绑 CPU2"这件事在 `make_rt()` 里，只有两个系统调用：`sched_setaffinity(0, ...)` 把当前线程钉在某个核上，`sched_setscheduler(0, SCHED_FIFO, &sp)` 把它提到实时优先级 50。第一个参数 `0` 表示"当前线程"。

先看不加感知负载（`--sensor-us 0`）时，三者是不是一样：

| 结构 | 超时率 | 周期 p99 | 每拍 write |
|---|---|---|---|
| 单线程 | 0.92% | 1497.9 µs | 4.07 µs |
| 线程 | 0.86% | 1495.2 µs | 3.93 µs |
| 进程 | 0.80% | 1492.4 µs | 3.67 µs |

一样。0.8% 左右的超时和 1.5 毫秒的 p99 不是程序的问题，是这台虚拟机的底噪——宿主机会挂起空闲的 vCPU，这条在上一辑量过。表里每个数都是三轮重复测量的中位数。

所以"线程比进程快"在控制循环这里没有意义。没活干的时候，三种结构一模一样。


七、把感知塞进去，就不一样了

现在让感知真的干点活：30 Hz，一次 5 毫秒。这就是一次相机检测、或者一次小的点云配准的量级。

| 结构 | 超时率 | 周期 p99 | 每拍 write |
|---|---|---|---|
| 单线程 | 3.50% | 6008.8 µs | 156.83 µs |
| 线程 | 0.88% | 1495.6 µs | 3.74 µs |
| 进程 | 0.84% | 1494.3 µs | 3.67 µs |

单线程那一行，三列全崩了：超时率从 0.92% 涨到 3.50%，p99 从 1498 微秒涨到 6009 微秒，每拍 write 从 4.07 微秒涨到 156.83 微秒。

156.83 这个数不是巧合，是算术。5000 微秒的活，每 33 拍做一次，摊到每拍就是 5000 ÷ 33 = 151.5 微秒，再加上原本的 4 微秒，落到 156 上下。**感知的成本没有消失，它被平均进了控制周期里。**

而线程和进程那两行，跟上一张表几乎没动：0.88% 和 0.84%，p99 还是 1495 微秒左右。

线程 0.88、进程 0.84——这两个数在这台机器上分不出高下。**真正起作用的不是"用线程还是用进程"，是"分没分开"这件事本身。**

分开以后，两个执行体各自被绑在不同的核上，感知那 5 毫秒占的是 CPU1，控制线程在 CPU2 上按自己的拍子走。感知再贵，也只是让 CPU1 忙一点。


八、所以 Linux 在机器人里的位置

回到标题。

Linux 在机器人里不是一个"操作系统"这么空的说法，它是四个具体的接口：**用户态应用**让你用 C 写控制律，算错了只死一个进程；**系统调用**是唯一能碰硬件的门，一次 838 纳秒；**线程**让你把 1 kHz 和 30 Hz 拆到不同的核上；**驱动**把设备变成文件，`write` 就是下发指令。

这四样合起来，才是"Linux 能跑机器人"的意思。

它不保证你的控制律对，也不保证你的周期准——第七节那张表就是证据：同一个程序，感知写在循环里，p99 就从 1.5 毫秒涨到 6 毫秒。**Linux 给你的是"能拆开"的能力，拆不拆是你的事。**


---

本篇代码在 `robot-brain/linux/`，结果在 `robot-brain/results/`：

    https://github.com/qhc0609/openeuler-sched-ext/commit/222a5bc

六个文件，`git show` 直接看：

    git show 222a5bc:robot-brain/linux/first-bot.c
    git show 222a5bc:robot-brain/linux/syscall-cost.c
    git show 222a5bc:robot-brain/linux/motor.c
    git show 222a5bc:robot-brain/linux/motor_abi.h
    git show 222a5bc:robot-brain/results/first-bot.txt
    git show 222a5bc:robot-brain/results/syscall-cost.log

自己跑一遍（openEuler 24.03，内核 6.12）：

    cd robot-brain/linux && make
    sudo insmod motor.ko
    ls -l /dev/robotmotor

    sudo ./bin/syscall-cost
    sudo ./bin/first-bot --mode single  --sensor-us 0
    sudo ./bin/first-bot --mode single  --sensor-us 5000
    sudo ./bin/first-bot --mode thread  --sensor-us 5000
    sudo ./bin/first-bot --mode process --sensor-us 5000
