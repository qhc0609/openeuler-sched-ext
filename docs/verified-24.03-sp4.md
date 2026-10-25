# 实测记录：openEuler 24.03 LTS SP4 基线

> 本文是**真机输出**，不是推断。采集时间 2026-10-25 11:30 CST。
> 复现方式：VMware Workstation 虚拟机，4 vCPU / 8 GB / 60 GB，最小化安装。

---

## 一、环境

| 项 | 值 |
|---|---|
| 宿主 | Windows Server 2022 (20348)，VMware Workstation |
| 客户机 | openEuler 24.03 (LTS-SP4) |
| 内核 | `6.6.0-159.4.3.154.oe2403sp4.x86_64` |
| vCPU / 内存 / 磁盘 | 4 / 8 GB / 60 GB |
| 网络 | VMware NAT，DHCP `192.168.17.130` |

---

## 二、原始输出

```console
[root@192 ~]# uname -r
6.6.0-159.4.3.154.oe2403sp4.x86_64

[root@192 ~]# grep -E '^(ID|VERSION_ID|PRETTY_NAME)=' /etc/os-release
ID="openEuler"
VERSION_ID="24.03"
PRETTY_NAME="openEuler 24.03 (LTS-SP4)"

[root@192 ~]# grep SCHED_CLASS_EXT /boot/config-$(uname -r)
# CONFIG_SCHED_CLASS_EXT is not set

[root@192 ~]# ls -d /sys/kernel/sched_ext
ls: 无法访问 '/sys/kernel/sched_ext': No such file or directory

[root@192 ~]# grep -ci 'scx_' /proc/kallsyms
0

[root@192 ~]# ls /boot/config-*
/boot/config-6.6.0-159.4.3.154.oe2403sp4.x86_64
```

---

## 三、结论

### 3.1 第一条前提成立：是 6.6，不是 5.12

```
6.6.0-159.4.3.154.oe2403sp4.x86_64
```

openEuler 24.03 LTS SP4 的内核基线确实是 **Linux 6.6**。
（对比：22.03 LTS 是 5.10，20.03 是 4.19。所以"24.03 = 6.6"这条可以写死在文档里。）

### 3.2 第二条前提不成立：sched_ext 开箱不可用

三条判据全部为否：

| 判据 | 结果 | 含义 |
|---|---|---|
| `/boot/config-*` 里 `CONFIG_SCHED_CLASS_EXT` | `is not set` | 没编进去 |
| `/sys/kernel/sched_ext` | 不存在 | 运行时没有这个子系统 |
| `/proc/kallsyms` 里 `scx_` 符号数 | `0` | 内核里没有任何 sched_ext 代码 |

**注意 `0` 这个数字很关键**：如果只是"代码在但配置关掉"，kallsyms 里不会有符号；
但如果代码编进去了却还没加载调度器，kallsyms 里**会有** `scx_*` 符号（因为它们是内核函数）。
现在是 0，说明连代码都没有。

### 3.3 一个未解释的细节（诚实记录）

`/boot/config-6.6.0-159...` 里出现了 `# CONFIG_SCHED_CLASS_EXT is not set` 这一行。
但对照 openEuler 上游仓库 `OLK-6.6` 分支：

- `kernel/sched/ext.c` —— 不存在（目录列表里没有该文件）
- `init/Kconfig` —— 没有 `SCHED_CLASS_EXT` 这个符号
- `kernel/sched/Makefile` —— 没有 `ext.o`

按上游源码，`make olddefconfig` **不应该**生成这一行。
推测 SP4 的构建树在 `OLK-6.6` 之上又叠了一层 openEuler 内部 patch（`kernel.spec` 的补丁队列），
其中带了 Kconfig 片段但没带实现。

**这个细节不影响结论**（运行时确实没有 sched_ext），但它影响一件事：
如果 SP4 的构建树里其实**有** `ext.c` 只是关掉了，那 24.03 上也能靠重编内核打开，
工作量就和 26.09 一样低。要确认只有一个办法——下载 `kernel-source` SRPM 解开看有没有 `ext.c`。
在没做这一步之前，不要假设"24.03 也能改开关打开"。

---

## 四、对项目的意义

这次实测把「24.03 LTS SP4 没有 sched_ext」从**文档推断**变成了**可复现的实测结论**，
并且给出了三条可交叉验证的判据（config / sysfs / kallsyms），而不是只看一条。

下一步按 `docs/openeuler-kernel-matrix.md` 的 B 路线走：
在 openEuler 26.09 上装 `kernel-6.18.48`，再跑一遍同样的三条判据做对照。
