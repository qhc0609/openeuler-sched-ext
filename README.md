# openeuler-sched-ext

在 **openEuler 24.03 LTS（Linux 6.6 基线）** 上确认内核基线并启用 **sched_ext**（BPF 可扩展调度类）。

> 目标：把"openEuler 24.03 上的 6.6 内核 + sched_ext"这件事从零散的命令行，变成一套可复现、可验证、可 CI 的流程。

---

## 快速开始

```bash
git clone https://github.com/<you>/openeuler-sched-ext.git
cd openeuler-sched-ext

./scripts/00-preflight.sh      # 第 1 步：确认基线 + 判定 sched_ext 可用性
./scripts/10-build-kernel.sh   # 第 2 步：构建带 sched_ext 的内核
./scripts/20-verify.sh         # 第 3 步：实机验证调度器接管
```

---

## 这个项目要解决什么问题

`sched_ext` 是 **Linux 6.12 才合入主线**的特性，而 openEuler 24.03 LTS 的内核基线是 **6.6**。
24.03 的内核树（`OLK-6.6` 分支）里**连 `kernel/sched/ext.c` 都不存在** —— 所以"在 24.03 上使能
sched_ext"不是改一个配置项，而是要把一整个子系统搬过去。

**但这件事在 26.09 上有转机**：openEuler 26.09 的 6.6 内核**已经把 sched_ext 回移进来了**
（`ext.c` 209 KB），只是 `openeuler_defconfig` 里写死 `CONFIG_SCHED_CLASS_EXT=n`。
所以版本选择直接决定工作量，先看矩阵：

| 版本 | 内核 | `ext.c` | `SCHED_CLASS_EXT` | 开箱可用 |
|---|---|---|---|---|
| 24.03 LTS SP4 | 6.6.0-175 | **无** | 符号不存在 | ✗ |
| 26.09（6.6 内核） | 6.6.0-175.oe2609 | **有** | **`=n`** | ✗ 需重编 |
| 26.09（6.18 内核） | 6.18.48-0.0.0.15 | 有 | 待实测 | 装包即可 |

完整取证过程（含可复现的 curl 命令）见 **[docs/openeuler-kernel-matrix.md](docs/openeuler-kernel-matrix.md)**。
回移的工程量分析见 **[docs/backport-6.6.md](docs/backport-6.6.md)**。

### 已实测：这条链路是通的（真机输出，非推断）

在 VMware 里装好 openEuler 24.03 LTS SP4，**原版 6.6 上 sched_ext 不可用**：

```console
$ uname -r
6.6.0-159.4.3.154.oe2403sp4.x86_64          # ✅ 是 6.6，不是 5.12
$ grep SCHED_CLASS_EXT /boot/config-$(uname -r)
# CONFIG_SCHED_CLASS_EXT is not set          # ❌
$ ls -d /sys/kernel/sched_ext
ls: cannot access '/sys/kernel/sched_ext'    # ❌
$ grep -ci 'scx_' /proc/kallsyms
0                                            # ❌ 连代码都没有
```

跑完 `10-build-kernel.sh` 装上 6.12 之后，**同一个系统上 sched_ext 真正接管了调度**：

```console
$ uname -r
6.12.0
$ cat /sys/kernel/sched_ext/root/ops
simple                                       # ✅ scx_simple 正在调度
$ cat /sys/kernel/sched_ext/state
enabled
$ cat /sys/kernel/sched_ext/nr_rejected
0
```

`root/ops` 显示 `simple` 而不是空 —— 这才是"能用"，不是"编进去了"。

完整记录：[docs/verified-24.03-sp4.md](docs/verified-24.03-sp4.md)（基线）
· [docs/verified-6.12-sched-ext.md](docs/verified-6.12-sched-ext.md)（走通全链路）

---

## 三步验证链路

| 步骤 | 脚本 | 做什么 | 输出 |
|---|---|---|---|
| 1 | `00-preflight.sh` | 校验发行版是 openEuler 24.03、内核 ≥ 6.6（**不是 5.10/5.12**）、检查配置符号与 `/sys/kernel/sched_ext` | `READY` / `NEED_KERNEL_BUILD` |
| 2 | `10-build-kernel.sh` | 以当前内核配置为基线，合并 `configs/sched-ext.fragment`，构建 6.12+ 主线内核 | `arch/x86/boot/bzImage` |
| 3 | `20-verify.sh` | 从 `/sys/kernel/sched_ext` 到 `bpftool struct_ops` 再到真实加载 `scx_simple` | 调度器 `root/ops` 非空 |

### 第 1 步的判据（从弱到强）

```bash
uname -r                                  # 6.6.x = 正确基线
grep SCHED_CLASS_EXT /boot/config-$(uname -r)   # 有 =y 才行
ls /sys/kernel/sched_ext/                 # 存在 = 内核真编进去了
cat /sys/kernel/sched_ext/root/ops        # 非空 = 调度器真的在跑
```

---

## 三条路线

| | A：24.03 + 自编主线 | B：26.09 + 6.18 包 | C：26.09 + 重编 6.6 |
|---|---|---|---|
| 用户态 | openEuler 24.03 | openEuler 26.09 | openEuler 26.09 |
| 内核 | 6.12 / 6.18 主线（自编） | 6.18.48（发行版包） | 6.6.0-175 + `=y` |
| 工作量 | 半天 | **十几分钟** | 一次内核构建（1–3 小时） |
| 回移补丁 | 不需要 | 不需要 | **不需要**（已回移） |
| 适合 | 追上游、研究调度器行为 | 最快拿到能跑的 sched_ext | 要在 6.6 基线上产品化 |
| 脚本 | `10-build-kernel.sh` | `dnf install kernel-6.18.48` | `10-build-kernel.sh --source-tree` |

**建议顺序：B → A → C。**
先用 B 在十几分钟内跑通 `20-verify.sh`，再决定要不要为"6.6 基线"这个约束投入 C。
B 和 C 的完整命令见 `docs/openeuler-kernel-matrix.md` 第四、五节。

> 注意：`dnf` 装上 6.18 不等于 `CONFIG_SCHED_CLASS_EXT=y`。
> 上游有这个符号，发行版构建时仍可能关掉。**必须用 `ls /sys/kernel/sched_ext` 实测**，不能推断。

---

## 配置片段

`configs/sched-ext.fragment` 是完整的 Kconfig 片段，包含：

```
CONFIG_SCHED_CLASS_EXT=y     # sched_ext 本体（6.12+ 才存在）
CONFIG_BPF_SYSCALL=y         # BPF 基础设施
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y      # ops 结构靠 BTF 描述
CONFIG_EXT_GROUP_SCHED=y     # cgroup 集成
CONFIG_SCHED_CORE=y
```

手动合并：

```bash
./scripts/kconfig/merge_config.sh -m .config configs/sched-ext.fragment
make olddefconfig
```

---

## 环境要求

- openEuler 24.03 LTS（SP1/SP2/SP3/SP4 均可）
- 内核 ≥ 6.6（基线确认）；路 A 需要能构建 6.12+
- 4 vCPU / 12GB 内存 / 30GB 空闲磁盘（构建产物约 3GB）
- 建议在虚拟机里做，**构建前先打快照**

```bash
sudo dnf install -y gcc make bc bison flex elfutils-libelf-devel \
                    openssl-devel dwarves perl python3 rsync cpio xz bpftool
```

---

## 目录结构

```
.
├── scripts/
│   ├── 00-preflight.sh      # 基线校验 + sched_ext 可用性判定
│   ├── 10-build-kernel.sh   # 构建带 sched_ext 的内核
│   └── 20-verify.sh         # 实机验证
├── configs/
│   └── sched-ext.fragment   # Kconfig 片段
├── docs/
│   ├── why-not-wsl.md       # 为什么不能靠 WSL，以及"一键"能到哪一步
│   ├── openeuler-kernel-matrix.md  # 各版本 × sched_ext 支持矩阵（实测核对）
│   ├── verified-24.03-sp4.md       # 24.03 LTS SP4 真机实测原始输出
│   ├── verified-6.12-sched-ext.md  # 装上 6.12 后 sched_ext 真正接管调度
│   └── backport-6.6.md      # 回移的工程量，以及三条路线
├── push.ps1                 # Windows 侧一键发布到 GitHub（5 秒）
├── vm-ssh.ps1               # Windows 侧通过 SSH 驱动虚拟机
├── .vm.env.example          # 虚拟机连接配置模板
├── .gitattributes           # 强制 LF，避免 CRLF 污染 patch
├── LICENSE                  # GPL-2.0
└── .github/workflows/ci.yml # shellcheck + openEuler 容器内冒烟
```

---

## Windows + VMware 场景

Agent 跑在 Windows 宿主，内核编译在 VMware 的 openEuler 里。用 SSH 把两端连起来：

```powershell
Copy-Item .vm.env.example .vm.env
notepad .vm.env                                   # 填 VM_HOST / VM_USER

.\vm-ssh.ps1 -Upload                              # 同步项目进虚拟机
.\vm-ssh.ps1 -Command "cd ~/openeuler-sched-ext && ./scripts/00-preflight.sh"
```

详见 **[docs/why-not-wsl.md](docs/why-not-wsl.md)**。


---

## 常见问题

**Q: 我的 openEuler 24.03 上 `grep SCHED_CLASS_EXT /boot/config-$(uname -r)` 没输出？**
正常。24.03 的内核树里连 `kernel/sched/ext.c` 都没有，符号自然不存在。走 `10-build-kernel.sh`。

**Q: openEuler 26.03 有吗？**
没有 26.03 **服务器版**。只有 `openEuler Embedded 26.03`（2026 年 3 月，嵌入式/具身智能方向）。
服务器创新版本是 **26.09**（2026-09-30 发布），双内核 6.6 + 6.18。

**Q: 26.09 上是不是直接就有 sched_ext？**
一半对。26.09 的 **6.6 内核已经把 sched_ext 回移进来**，但 `CONFIG_SCHED_CLASS_EXT=n` 默认关闭。
要么装它单独发布的 **6.18.48 内核包**（`dnf install kernel-6.18.48`），要么把 6.6 内核改开关重编。
两条路都不用写回移补丁。详见 `docs/openeuler-kernel-matrix.md`。

**Q: 一定要装 6.12 吗？**
上游首次合入是 6.12。但**发行版可以自己回移** —— openEuler 26.09 就做了。
所以「6.6 能不能开 sched_ext」取决于发行版有没有回移，不是内核版本本身。`-v` 指定任意 ≥6.12 版本都行。

**Q: 编译到 certs 阶段报 `没有规则可制作目标"certs/openeuler-cert.pem"`？**
这是把**发行版内核配置**直接喂给**原版内核源码树**的必然结果：
openEuler 的 `/boot/config` 里 `CONFIG_SYSTEM_TRUSTED_KEYS="certs/openeuler-cert.pem"`，
而原版 6.12 的 `certs/` 目录里没有这个文件，于是 `certs/x509_certificate_list` 无法生成。

`10-build-kernel.sh` 已经自动处理（合并片段后把该符号清空）。手工构建时这样做：

```bash
scripts/config --file .config --set-str SYSTEM_TRUSTED_KEYS ""
make olddefconfig
```

本项目不涉及内核模块签名信任链，清空后内核会改用自签名，不影响 sched_ext。

**Q: 装完新内核起不来怎么办？**
用户态完全没动。重启进 GRUB 选回原内核即可。所以在虚拟机里做，先打快照。

**Q: sched_ext 换调度器要重启吗？**
不用，热切换是它的核心卖点。

**Q: `push.ps1` 一直报 502 / 超时，但浏览器能打开 GitHub？**
先分清是哪一段断了 —— `git push` 只走 `github.com`，`gh` 走 `api.github.com`，
两者经常一个通一个不通。实测过一次：

| 端点 | 结果 |
|---|---|
| `api.github.com` | 200 / 0.35s |
| `codeload.github.com` | 301 / 0.64s |
| `github.com` | 超时 |
| `raw.githubusercontent.com` | 超时 |

这种情况下不是凭据问题，也不是仓库权限问题，只能等 `github.com` 恢复。
`push.ps1` 会自己判断并告诉你是哪一种，挂机重试：

```powershell
.\push.ps1 -Watch 30      # 每 30 秒试一轮，最多 30 分钟，成功即退出
```

另外 `push.ps1` 会自动在 `C:\Program Files\Git\cmd`、`C:\Program Files\GitHub CLI`
这些常见位置找 `git.exe` / `gh.exe` —— 装完没重开终端、或者安装器没写 PATH，
是"一键推送"失败最常见的原因。

---

## License

GPL-2.0。内核模块与内核头文件衍生物必须 GPL-2.0 兼容，本项目统一使用该协议。
