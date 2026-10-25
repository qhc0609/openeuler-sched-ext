# 实测记录：24.03 LTS SP4 + 6.12 内核 → sched_ext 真正可用

> 本文是**真机输出**。采集时间 2026-10-25 14:38 CST。
> 这是本项目第一条完整走通的链路：基线校验 → 自编内核 → 实机接管调度。

---

## 一、最终结论

在 openEuler 24.03 LTS SP4 上，用原版 Linux 6.12.0 内核替换 6.6 之后，
**sched_ext 完整可用，并且成功加载了真实的 BPF 调度器接管系统调度**：

```console
$ cat /sys/kernel/sched_ext/root/ops
simple

$ cat /sys/kernel/sched_ext/state
enabled

$ cat /sys/kernel/sched_ext/enable_seq
1

$ cat /sys/kernel/sched_ext/nr_rejected
0

$ ps -ef | grep [s]cx_simple
root        6891       1  1 14:38 ?        00:00:00 ./scx_simple
```

`root/ops` 显示 `simple`，意思是 **`scx_simple` 这个 BPF 调度器正在接管 CPU 调度**，
而不是仅仅"编译进了内核"。`scx_simple` 自己的计数器也在递增：

```
local=1 global=1
local=22 global=12
local=32 global=23
local=40 global=35
```

`local` / `global` 是它自己的 dispatch 计数，说明调度路径真的在被执行。

---

## 二、环境

| 项 | 值 |
|---|---|
| 宿主 | Windows Server 2022 (20348)，VMware Workstation |
| 客户机 | openEuler 24.03 (LTS-SP4) |
| 内核（装后） | `6.12.0`（原版 kernel.org 源码，非发行版内核） |
| 内核（装前） | `6.6.0-159.4.3.154.oe2403sp4.x86_64` |
| 引导 | BIOS + GRUB2，`/boot/grub2/grub.cfg` |
| 资源配置 | 4 vCPU / 8 GB / 60 GB |
| 快照 | `pre-6.12-kernel`（装内核前打，可一键回退） |

---

## 三、三步链路

### 第 1 步：基线校验（装内核前）

```console
$ ./scripts/00-preflight.sh
  PRETTY_NAME : openEuler 24.03 (LTS-SP4)
  uname -r    : 6.6.0-159.4.3.154.oe2403sp4.x86_64
  [WARN] # CONFIG_SCHED_CLASS_EXT is not set（未启用）
  [ OK ] CONFIG_BPF_SYSCALL=y
  [ OK ] CONFIG_BPF_JIT=y
  [ OK ] CONFIG_BPF_JIT_ALWAYS_ON=y
  [ OK ] CONFIG_DEBUG_INFO_BTF=y
  [WARN] /sys/kernel/sched_ext 不存在 —— sched_ext 未启用
  [ OK ] 编译工具链齐备
  [ OK ] bpftool bpftool v7.2.0

== 结论 ==
  [WARN] 基线正确，但 sched_ext 不可用 → 需要自己构建内核
[exit=1]
```

exit=1 是**设计预期**，不是故障：基线对，但特性不可用。

### 第 2 步：构建 + 安装

```console
$ ./scripts/10-build-kernel.sh -v 6.12 -j 4 -i
[build] 目标内核 : linux-6.12
[build] 清空 CONFIG_SYSTEM_TRUSTED_KEYS（发行版私有证书在原版树里不存在）
  CONFIG_SCHED_CLASS_EXT=y
  CONFIG_EXT_GROUP_SCHED=y
[build] 开始编译（4 线程）
...
[build] 构建成功: /root/src/linux-6.12/arch/x86/boot/bzImage
       大小: 14M
[build] 把默认启动项设为 6.12.0
/boot/vmlinuz-6.12.0
[build] 重建 initramfs
安装完成。
```

产物：

```
/boot/vmlinuz-6.12.0          14 MB
/boot/initramfs-6.12.0.img    97 MB
/lib/modules/6.12.0/          （模块 + depmod）
```

耗时：完整编译约 **65 分钟**（4 vCPU，`DEBUG_INFO_BTF=y` 不能关，`struct_ops` 靠 BTF 描述）。

### 第 3 步：实机验证

```console
$ ./scripts/20-verify.sh
== 1. 内核 ==
  uname -r : 6.12.0
== 2. sched_ext 是否编进内核 ==
  [ OK ] /sys/kernel/sched_ext 存在
    nr_rejected = 0
    enable_seq = 0
== 3. BPF struct_ops 能力 ==
  [ OK ] bpftool 已安装
    struct_ops 已注册数量: 0
== 4. 加载一个真实调度器 ==
  [WARN] 没找到 scx_simple，跳过实机验证
== 结论 ==
  [ OK ] sched_ext 验证通过
```

第 4 步当时跳过了，因为 `scx_simple` 需要单独编译。补上这一步才是完整验收：

```console
$ cd /root/src/linux-6.12/tools/sched_ext && make
$ ls build/bin/
scx_central  scx_flatcg  scx_qmap  scx_simple

$ ./build/bin/scx_simple &
$ cat /sys/kernel/sched_ext/root/ops
simple
```

---

## 四、`scx_simple` 从哪来（重要）

**不要去 clone `sched-ext/scx`。** 现在的 scx 仓库已经把 `scheds/c/` 整个删掉了，
调度器全部改成 Rust，`scx_simple` 也已经不存在；编 Rust 还要装整套 cargo 工具链，很慢。

正确做法：**用内核源码树自带的示例**，纯 C + libbpf，几分钟就好：

```bash
# 依赖（openEuler 源里都有）
sudo dnf install -y clang llvm libbpf-devel bpftool

# 源码就在你刚编的内核树里
cd ~/src/linux-6.12/tools/sched_ext
make                     # 产物在 build/bin/
sudo ./build/bin/scx_simple &
cat /sys/kernel/sched_ext/root/ops     # 应输出 simple
```

一共四个示例：`scx_simple` / `scx_qmap` / `scx_central` / `scx_flatcg`。

---

## 五、踩过的坑

### 5.1 发行版配置 + 原版源码树 = certs 失败

```
make[3]: *** 没有规则可制作目标"certs/openeuler-cert.pem"
```

`/boot/config` 里 `CONFIG_SYSTEM_TRUSTED_KEYS="certs/openeuler-cert.pem"`，
原版树没有这个文件。脚本已自动清空，见 README 的 FAQ。

### 5.2 `grub2-mkconfig` 不会改默认启动项

openEuler 用 `GRUB_DEFAULT=saved`。`grub2-mkconfig` 只重写菜单，
**`saved_entry` 还是旧内核** —— 装完重启会进 6.6，白装。
`grubby --info=ALL` 里的 `index=0` 只是排序，不代表默认项。

必须显式设置（脚本已包含）：

```bash
grubby --set-default /boot/vmlinuz-6.12.0
grub2-editenv list          # 应显示 saved_entry=... (6.12.0) ...
grubby --default-kernel     # 应输出 /boot/vmlinuz-6.12.0
```

### 5.3 等待编译时不要只看产物文件

第一次编译在 4 分 43 秒就失败了（certs），但只看"`bzImage` 是否出现"的等待逻辑
会一直等到超时。判据应该是：**进程已退出 + 产物不存在 = 失败**。

---

## 六、和 24.03 原版的对照

| | 24.03 SP4 原版 | 24.03 SP4 + 6.12 |
|---|---|---|
| `uname -r` | 6.6.0-159.4.3.154 | **6.12.0** |
| `CONFIG_SCHED_CLASS_EXT` | `is not set` | **`=y`** |
| `/sys/kernel/sched_ext` | 不存在 | **存在** |
| `root/ops` | — | **`simple`** |
| 能否加载 BPF 调度器 | 否 | **是** |

用户态完全没动，还是 openEuler 24.03 的软件包。炸了就在 GRUB 里选回 6.6，
或者 `vmrun revertToSnapshot ... "pre-6.12-kernel"`。
