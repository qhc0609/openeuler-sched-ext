# 为什么 openEuler 24.03 的 6.6 内核「开不了」SCHED_EXT

## 一句话

`sched_ext` 是 **Linux 6.12 才合入主线**的特性。openEuler 24.03 LTS 的内核基线是 **6.6**，
stock 6.6 里**根本不存在 `CONFIG_SCHED_CLASS_EXT` 这个 Kconfig 符号**。

所以在 6.6 上"使能 sched_ext"不是改一个配置项，而是**把一整个子系统搬过去**。
这也是本项目第一步必须先做基线确认的原因。

```bash
# 在 openEuler 24.03 上直接验证：
grep SCHED_CLASS_EXT /boot/config-$(uname -r)
# → 无输出，说明符号不存在
ls /sys/kernel/sched_ext/
# → No such file or directory
```

---

## 为什么不是「加个 CONFIG 就行」

`sched_ext` 让调度决策跑在 BPF 程序里，它依赖一整条 **BPF struct_ops for sched** 的支撑链路，
而这条链路在 6.6 时代还不完整。

### 依赖矩阵

| 组件 | 6.6 现状 | 6.12 现状 | 移植难度 |
|---|---|---|---|
| `kernel/sched/ext.c`（调度类本体） | 不存在 | 约 8000 行 | 直接搬，但要适配 6.6 的 rq / sched_class 结构 |
| `include/linux/sched/ext.h` | 不存在 | 大改 `task_struct` | **高**：改 `task_struct` 布局，所有模块 ABI 全变 |
| BPF struct_ops for sched | 只支持 TCP congestion 等 | 新增 sched 类型 + 子程序调用 | **高**：纠缠 verifier、trampoline、`bpf_prog_pack` |
| `scx_bpf_*` kfuncs | 不存在 | 一套完整 kfunc | 中 |
| `CONFIG_EXT_GROUP_SCHED`（cgroup 集成） | 不存在 | 新增 cgroup 控制器 | **高**：cgroup v2 核心代码改动 |
| `scx_task_iter` / 迁移机制 | 不存在 | 完整实现 | 中 |
| `tools/sched_ext/`（用户态工具） | 不存在 | `scx_common`、`scx_simple` 等 | 低（用户态） |
| 符号导出 `EXPORT_SYMBOL_GPL` | — | 一批新符号 | 低 |

粗量级：**数十个 commit、上万行改动**，且高度纠缠。这不是 `git cherry-pick` 能干净落地的东西。

### 已经被做过的先例

| 项目 | 情况 |
|---|---|
| Android Common Kernel `android14-6.6` | Google 已把 sched_ext 反向移植进 6.6 分支，是**最值得参考的实现** |
| 各发行版的 6.8/6.9 回移 | Ubuntu 等做过部分回移，可参考补丁拆分方式 |
| openEuler OLK-6.6 | 有大量自研回移（动态复合页等），但 sched_ext 需自行确认 |

> 结论：**技术上可行，但有现成参考，不是一个下午能完成的事。**

---

## 两条路，选一条

### 路 A：保留 openEuler 24.03 用户态，换 6.12+ 主线内核（推荐先走这条）

```
openEuler 24.03 用户态（glibc / dnf / systemd 全是 24.03 的）
        +
Linux 6.12 LTS 内核（带 CONFIG_SCHED_CLASS_EXT=y）
        =
sched_ext 立刻可用
```

优点：**当天就能跑起来**。6.12 是首个含 sched_ext 的 LTS，生命周期到 2026-12，作为研究基线够用。
缺点：严格说不是"在 6.6 上"，而是"在 openEuler 24.03 上"。如果你的目标是研究调度器本身，
这完全够；如果目标是给 openEuler 6.6 提交回移补丁，这只算前置验证。

```bash
./scripts/10-build-kernel.sh -v 6.12 -i
sudo reboot
./scripts/20-verify.sh
```

### 路 B：把 sched_ext 反向移植进 openEuler OLK-6.6

这才是"在 6.6 上使能 sched_ext"的字面实现，也是这个项目真正的研究内容。

建议的拆分顺序（每一步都能独立编译通过再往下走）：

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| B0 | 取 openEuler 24.03 的 `kernel-source` SRPM，跑通原生编译 | `make rpm` 出包，装上去能开机 |
| B1 | 移植 `include/linux/sched/ext.h` + `task_struct` 字段 | 只改结构、不接逻辑，能编译能开机 |
| B2 | 移植 `kernel/sched/ext.c` 骨架 + `sched_class` 注册 | `ls /sys/kernel/sched_ext` 出现 |
| B3 | 移植 BPF struct_ops for sched（最难的一步） | `bpftool struct_ops list` 能看到 scx ops |
| B4 | 移植 `scx_bpf_*` kfuncs | `scx_simple` 能加载 |
| B5 | 移植 `CONFIG_EXT_GROUP_SCHED` cgroup 集成 | cgroup 限流下调度正常 |
| B6 | 移植 `tools/sched_ext/` | `scx_rusty` / `scx_simple` 跑通 |

参考实现优先级：**Android ACK `android14-6.6` > 各发行版 6.8/6.9 回移 > 直接对比 6.12 上游**。

```bash
# B0 起点：取 openEuler 的源码包
sudo dnf install -y rpm-build rpmdevtools dnf-plugins-core
dnf download --source kernel
rpm2cpio kernel-*.src.rpm | cpio -idmv
tar xf kernel.tar.xz && cd kernel
make openeuler_defconfig && make -j"$(nproc)"
```

---

## 怎么判断走哪条

```bash
./scripts/00-preflight.sh
```

- 输出 `sched_ext 已可用` → 直接 `./scripts/20-verify.sh`
- 输出 `需要自己构建内核` → 走**路 A**（`10-build-kernel.sh`）
- 你的项目目标是给 openEuler 提交回移 → 走**路 B**，路 A 只是前置验证

---

## 常见误区

| 误区 | 事实 |
|---|---|
| "6.6 加个 `CONFIG_SCHED_CLASS_EXT=y` 就行" | 符号不存在，Kconfig 会直接忽略这一行 |
| "6.6 也有 sched_ext，网上都这么说" | 网上很多文章把 6.12 的特性写成 6.6，抄错了。以 `/sys/kernel/sched_ext` 是否存在为准 |
| "换个 6.6 的 openEuler SP 版本就有了" | SP 是用户态与驱动增强，内核主线版本仍是 6.6 |
| "sched_ext 需要重启才能换调度器" | 恰恰相反，它的核心卖点就是**热切换、无需重启** |
