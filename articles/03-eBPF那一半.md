eBPF 那一半：调度器到底写在哪儿

—— 从三个内核开关到一张 33 格的回调表（第 3/6 篇）


一、上一篇留下的那个空文件夹

上一篇结尾，你的系统应该长这样：

    $ cat /sys/kernel/sched_ext/state
    disabled

    $ ls /sys/kernel/sched_ext/root/ops
    （空）

    $ grep -c sched_ext /proc/kallsyms
    0

等等，最后一条是 0？那说明符号根本没进去。如果前两条能执行，说明 sched_ext 这个类已经编进内核了，但没有任何调度器在用它。

这两个状态差别很大。前者是"插座没装"，后者是"插座装好了，电器还没插"。

上一篇解决的是插座。这一篇解决电器。

为什么之前一直没讲电器？因为仓库里原本没有自己的调度器——验证时是借内核源码树里的 tools/sched_ext/scx_simple 来跑的。这有个副作用：读者把整篇文章看完，仍然不知道 BPF 调度器长什么样、怎么写、每一行在干什么。

这个缺口是真实存在的，所以我补了一个提交 3dc833c，往仓库里放了一个 200 行的调度器，每一行都带注释。这一篇讲它为什么这么写，下一篇逐行讲每一行。


二、eBPF 是被三个开关打开的，不是一个

很多人以为"内核支持 eBPF"是个布尔值。不是。它是一串依赖，而且每个依赖负责的事完全不同。

第一个：CONFIG_BPF_SYSCALL=y

这个决定 bpf() 这个系统调用存不存在。

用户态加载 BPF 程序，走的全是这一个系统调用，只是命令号不同：BPF_PROG_LOAD 加载程序，BPF_MAP_CREATE 建 map，BPF_MAP_LOOKUP_ELEM 读 map。

没有它，你连"请求加载"这个动作都发不出去。libbpf 里所有函数最后都落到这里，失败时返回 EPERM 或 ENOSYS。

第二个：CONFIG_BPF_JIT=y

这个决定字节码怎么执行。

BPF 程序编译出来是虚拟机字节码，不是机器码。内核有两条路执行它：解释器逐条翻译，或者 JIT 编译成本地指令。

调度器是热路径——每次任务切换都要跑你的 enqueue 和 dispatch。解释器慢一个数量级，在这个位置是不能接受的。

所以 CONFIG_BPF_JIT_ALWAYS_ON=y 也一起打开，强制走 JIT，不留解释器这个后路。省下的那点内核体积，换不来一次调度延迟。

第三个：CONFIG_DEBUG_INFO_BTF=y

这个决定 /sys/kernel/btf/vmlinux 这个文件存不存在。

它是三个里最容易被当成"可选"的，也是唯一一个少了就完全跑不起来的。

实测这个文件 5.0 MB：

    $ ls -lh /sys/kernel/btf/vmlinux
    -r--r--r-- 1 root root 5.0M ... /sys/kernel/btf/vmlinux


三、为什么 BTF 是硬要求，不是加分项

要理解这一点，得先知道 struct_ops 是什么。

普通 BPF 程序是"挂上去、被动触发"：你写一个程序，挂到某个 tracepoint 或 kprobe 上，事件发生了内核调用你。控制流在内核手里。

struct_ops 反过来。内核里有一张函数指针表，比如 struct sched_ext_ops，里面全是"遇到这个情况该调谁"。你把你的 BPF 函数地址填进这张表的格子里，内核在需要的时候按格子调用你。

填格子这件事，本质上是把一个地址写进内核结构体。问题来了：内核凭什么相信你填的地址指向的函数，参数类型和它声明的一致？

如果参数对不上，比如内核按 (task, flags) 调用，你的函数按 (flags, task) 接收，那就是把指针当整数用、把整数当指针解引用——直接崩，而且崩在调度器里，等于整机挂死。

所以必须有一个校验环节。

BTF 就是干这个的。它是内核里的一份类型信息，描述了每个结构体的字段布局、每个函数的参数类型。libbpf 在加载前，把 /sys/kernel/btf/vmlinux 读出来，找到 struct sched_ext_ops 的完整定义，然后逐个成员比对你填的函数的签名。

对不上，加载直接失败，一个字节都不会进内核。

没有 BTF，libbpf 只拿到一个裸地址，无从比较。它只能选择"信任"，而内核不接受盲填。

这就是为什么上一篇里 CONFIG_DEBUG_INFO_BTF 不能省。它不是调试信息，它是类型系统的载体。


四、这张表有 33 个格子

6.12 的 BTF 里数出来，struct sched_ext_ops 有 33 个回调成员。分组看：

调度决策类——select_cpu、enqueue、dequeue、dispatch、tick

任务生命周期类——runnable、running、stopping、quiescent、yield、init_task、exit_task

CPU 与拓扑类——cpu_acquire、cpu_release、cpu_online、cpu_offline、update_idle、set_cpumask、set_weight、core_sched_before

cgroup 类——cgroup_init、cgroup_exit、cgroup_prep_move、cgroup_move、cgroup_cancel_move、cgroup_set_weight，共 6 个

调试与生命周期——init、exit、enable、disable、dump、dump_cpu、dump_task

33 个。听起来很吓人。

关键在这句话：没填的格子，内核用默认行为。

这是 sched_ext 设计的核心取舍。它不要求你实现全部，只要求你实现"你打算改的那部分"。剩下的内核替你兜底，行为和 CFS 接近。

所以一个能真正跑起来的调度器，只需要 5 个：

select_cpu、enqueue、dispatch——三个核心决策

init、exit——建队列和收尾

33 分之 5。剩下 28 个空着，内核照常工作。


五、三个决策点，就是调度的全部

把调度这件事剥到最里面，只有三个时刻需要做决定。

时刻一：任务要跑了，给它挑个核。

对应 select_cpu。任务从睡眠里被唤醒，或者刚 fork 出来，内核问你：放哪个 CPU？

考虑的因素通常有缓存亲和性（上次在哪个核跑，L1/L2 还是热的）、哪些核空闲、NUMA 距离。

时刻二：任务要排队了，放哪个队列。

对应 enqueue。任务变成可运行状态，但 CPU 正忙，得先排队。你决定它进哪个队。

这一步就是"策略"发生的地方。所有任务进一个队，就是 FIFO；按权重算虚拟时间进队，就是公平调度；按 deadline 进队，就是实时。

时刻三：CPU 空了，从队列里取谁。

对应 dispatch。一个 CPU 要挑下一个任务执行，你从队列里拿一个给它。

三个时刻，三个函数。想清楚这三个，sched_ext 就通了。

剩下的 30 个回调，都是在这三个之上做精细化，不是必需。


六、DSQ：不是链表，是能被抢的队列

三个决策点里反复出现"队列"。sched_ext 里这个东西叫 DSQ，dispatch queue。

它和普通链表有个关键区别：DSQ 可以被多个 CPU 同时抢。

一个任务在全局 DSQ 里，任何空闲的 CPU 都能来 consume 它。谁先到谁拿走，拿走的同时从队列里摘掉。

这个设计解决了一个经典问题：任务放错核了怎么办。传统做法是负载均衡器定期搬任务，搬的时候要加锁、要迁移缓存。DSQ 不需要——任务躺在共享队列里，哪个核空了自己来拿，天然就是负载均衡。

DSQ 有三种：

SCX_DSQ_LOCAL——当前 CPU 的本地队列。塞进这里，只有这个 CPU 能取，不参与抢夺。

SCX_DSQ_GLOBAL——内核内置的全局队列。所有 CPU 共享，但只能当 FIFO 用，不能排序。

自建 DSQ——用 scx_bpf_create_dsq() 建，编号自己定。可以配合 scx_bpf_dispatch_vtime() 按虚拟时间排序。

为什么最小实现要自建一个？因为 SCX_DSQ_GLOBAL 不能排序。现在用不上排序，但接口留着，改成按权重公平调度只需要换一个 dispatch 函数。


七、所以这个最小调度器的策略是什么

一句话：所有任务进同一个自建队列，先来先跑，每个跑 20 毫秒。

20 毫秒来自 SCX_SLICE_DFL，内核给的默认时间片。任务连续跑满 20 ms 就被换下来，回队列重新排。

这个策略不好用——它完全不考虑权重，一个 niced 到 19 的编译进程和一个交互式 shell 抢 CPU，机会完全一样。但它有三个好处：

代码短到能一次读完。三个核心回调加起来不到 30 行。

每个决策点都露在外面。你能清楚看到"挑核""入队""出队"分别发生在哪一行。

改起来有明确的落点。想加权重，改 enqueue 那一行；想加 deadline，还是改那一行。

它的价值不是"能用"，是"能看懂"。看懂之后，scx_lavd 那两千行就不再是天书。


八、这一篇没讲代码，下一篇全是代码

机制讲完了：三个开关、BTF 校验、33 格回调表、三个决策点、三种 DSQ。

但你现在还不知道这 200 行长什么样，为什么第一行要 include 一个奇怪的头文件，为什么 _license 必须写 GPL，为什么 stats 这个 map 是必需的而不是调试用的。

下一篇逐行拆。包括三个让我卡了半天的构建错误，报错原文一字不改地贴出来。


本篇对应提交

这一篇只有一条提交，但它是整个项目最核心的一条：3dc833c。

为什么必须有这一条：在这之前，仓库能证明"内核里有 sched_ext"，但证明方式是借来的——直接用内核树里的 tools/sched_ext/scx_simple。也就是说，BPF 那一半的故事活在项目外面。读者打开仓库，看不到一行 BPF 代码。

这条提交把它拿了进来。四个文件，454 行：

    scheds/minimal.bpf.c   215 行   调度器本体（BPF 侧）
    scheds/minimal.c       159 行   加载器（用户态）
    scheds/Makefile         74 行   两段式构建：先 BPF，再用户态
    .gitignore               6 行   别把生成物提交上去

215 行里有一大半是注释。这不是凑数——sched_ext 的难点不在写，在知道每个回调在什么时机被调用、里面能调哪些 helper。所以每一行旁边都标了它占据内核的哪个决策点。

    https://github.com/qhc0609/openeuler-sched-ext/commit/3dc833c

本地看这一条改了什么：

    git show 3dc833c --stat
    git show 3dc833c scheds/minimal.bpf.c

看提交说明里记的三个构建坑的成因：

    git log -1 3dc833c

配套的 b395f23 是验证路径，第 5 篇用它。


下一篇预告

逐行读懂这个 200 行的调度器——包括 BPF_STRUCT_OPS 为什么会和 vmlinux.h 里的枚举撞名，以及为什么 BPF 目标文件绝对不能参与链接。
