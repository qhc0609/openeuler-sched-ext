# openEuler 版本 × sched_ext 支持矩阵（实测核对）

> 本文所有结论都标注了可复现的取证方式，不依赖博客或二手资料。
> 核对时间：2026-10-25。

---

## 一、先纠正三个常见误解

**误解 1：「openEuler 有 26.03」**
只有 **openEuler Embedded 26.03**（2026 年 3 月，嵌入式/具身智能方向，面向 Hi3403、Hi3591 等开发板）。
**没有 26.03 服务器版**。服务器线的创新版本从 25.09 直接跳到 **26.09**。

**误解 2：「6.6 里没有 sched_ext 的代码」**
对 **24.03 LTS** 成立，对 **26.09** 不成立。
26.09 的 6.6 内核已经把 sched_ext **回移**进去了（`kernel/sched/ext.c` 209 KB），只是默认关闭。

**误解 3：「sched_ext 是 6.12 才有的，所以 6.6 一定开不了」**
上游主线确实 6.12 才合入。但**发行版可以自己回移**——openEuler 在 26.09 就做了。
所以「6.6 能不能开 sched_ext」不是内核版本问题，而是**这个发行版有没有回移**的问题。

---

## 二、版本矩阵

| 版本 | 类型 | 内核 | `kernel/sched/ext.c` | `CONFIG_SCHED_CLASS_EXT` | 开箱可用 |
|---|---|---|---|---|---|
| 22.03 LTS | LTS | 5.10 | 无 | 符号不存在 | ✗ |
| **24.03 LTS SP4** | LTS | 6.6.0-175 | **无** | 符号不存在 | ✗ |
| 25.09 | 创新 | 6.6 | 无 | 符号不存在 | ✗ |
| **26.09**（服务器，6.6 内核） | 创新 | 6.6.0-175.0.0.5.oe2609 | **有**（209,174 B） | **`=n`** | ✗ 需重编 |
| **26.09**（6.18 内核） | 创新 | 6.18.48-0.0.0.15.oe2609 | 有（上游原生） | 待实测 | 装包即可，需验证 |
| Embedded 26.03 | 嵌入式 | 6.18 系 | — | — | 不适用（非服务器场景） |

### 关键结论

1. **24.03 LTS SP4 是你项目的正确起点**：它是「6.6 基线 + 完全没有 sched_ext」的干净对照组。
2. **26.09 的 6.6 内核已经回移了 sched_ext，但 `CONFIG_SCHED_CLASS_EXT=n`**。
   这意味着在 26.09 上使能 sched_ext **不需要写回移补丁**，只需要**改一个开关重编内核**。
   工作量从「数周」降到「一次内核构建」。
3. **26.09 提供独立的 6.18 内核包**，装在 update 仓库里，一条 `dnf` 就能拿到。

---

## 三、取证方式（可复现）

### 3.1 26.09 的 6.6 内核里到底有没有 ext.c

```bash
# 有内容 = 文件存在
curl -s "https://gitee.com/api/v5/repos/openeuler/kernel/contents/kernel/sched/ext.c?ref=openEuler-26.09" \
  | head -c 120
# {"type":"file","encoding":"base64","size":209174,"name":"ext.c",...

# 24.03 对应的分支
curl -s "https://gitee.com/api/v5/repos/openeuler/kernel/contents/kernel/sched/ext.c?ref=OLK-6.6"
# []
```

`[]` 就是文件不存在。这是 24.03 和 26.09 的分水岭。

### 3.2 开关的默认值

```bash
curl -sL "https://gitee.com/openeuler/kernel/raw/openEuler-26.09/arch/x86/configs/openeuler_defconfig" \
  | grep -n SCHED_CLASS_EXT
# 120:CONFIG_SCHED_CLASS_EXT=n
```

注意这个 defconfig 头部写的是 `Linux/x86 6.6.0 Kernel Configuration` ——
即 **26.09 的默认内核仍是 6.6**，6.18 是并列的可选内核。

### 3.3 6.18 内核包是否真的发布

```bash
# 默认 OS 仓库：只有 6.6
curl -sL "https://repo.openeuler.org/openEuler-26.09/OS/x86_64/Packages/" | grep -oE 'kernel-[0-9][^"<>]*\.rpm' | sort -u
# kernel-6.6.0-175.0.0.5.oe2609.x86_64.rpm
# kernel-devel / headers / source / tools（均 6.6.0-175）

# update 仓库：6.18 在这里
curl -sL "https://repo.openeuler.org/openEuler-26.09/update/x86_64/Packages/" | grep -oE 'kernel-[0-9][^"<>]*\.rpm' | sort -u
# kernel-6.18.48-0.0.0.15.oe2609.x86_64.rpm
# kernel-devel / headers / source / tools（均 6.18.48）
```

所以 6.18 是**完整的一套**（kernel + devel + headers + source + tools），
不是只给 DevStation 用的半套。官方文案里「基于 6.18 增强 DevStation」说的是**用途侧重**，
不代表包只在 DevStation 镜像里。

---

## 四、由此得到的三条路线

| | A：24.03 + 自编主线内核 | B：26.09 + 6.18 内核包 | C：26.09 + 重编 6.6 开开关 |
|---|---|---|---|
| 用户态 | 24.03 LTS SP4 | 26.09 | 26.09 |
| 内核 | 6.12 / 6.18 主线（自编） | 6.18.48（发行版包） | 6.6.0-175 + `=y` |
| 工作量 | 半天 | **十几分钟**（dnf + 重启） | 一次内核构建（1–3 小时） |
| 回移补丁 | 不需要 | 不需要 | **不需要**（openEuler 已回移） |
| 适合 | 研究调度器行为、追上游 | 最快拿到可用的 sched_ext | 想在 6.6 基线上做产品化 |
| 风险 | 自编内核与 openEuler 用户态可能有摩擦 | 双内核切换需改 GRUB 默认项 | 发行版内核重编，ABI 可能变动 |

**推荐顺序：B → A → C。**
先用 B 在十几分钟内拿到一个能跑 `scx_simple` 的系统，把 `20-verify.sh` 跑通；
再决定要不要为了「6.6 基线」这个约束投入 C。

---

## 五、B 路线的具体命令（待实机验证）

```bash
# 1. 确认当前内核
uname -r                       # 期望 6.6.0-175.0.0.5.oe2609.x86_64

# 2. 看 6.18 是否可见
dnf --showduplicates list kernel | grep 6.18

# 3. 安装 6.18（与 6.6 并存，不删除旧内核）
sudo dnf install -y kernel-6.18.48 kernel-devel-6.18.48 kernel-headers-6.18.48

# 4. 确认 GRUB 里多了一个 6.18 启动项
sudo grubby --info=ALL | grep -E "^kernel|^title|^index"

# 5. 设为默认并重启（保留 6.6 作为回退）
sudo grubby --set-default /boot/vmlinuz-6.18.48

# 6. 重启后验证
uname -r
ls /sys/kernel/sched_ext
grep SCHED_CLASS_EXT /boot/config-6.18.48
```

第 6 步的三条输出才是**唯一权威判据**。
`dnf` 装上了不等于 openEuler 的 6.18 构建里开了 `CONFIG_SCHED_CLASS_EXT`——
上游有这个符号，发行版仍可能关掉。这一步必须实测，不能推断。

---

## 六、对 24.03 LTS SP4 的结论（不变）

24.03 LTS SP4 上：

```
$ uname -r
6.6.0-xxx.oe2403.x86_64

$ grep SCHED_CLASS_EXT /boot/config-$(uname -r)
（无输出 —— 符号不存在）

$ ls /sys/kernel/sched_ext
ls: cannot access '/sys/kernel/sched_ext': No such file or directory
```

这三条正是 `00-preflight.sh` 期望看到的「NEED_KERNEL_BUILD」状态。
它证明的是**基线正确**（6.6，不是 5.10/5.12），而不是环境有问题。
