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
stock 6.6 里不存在 `CONFIG_SCHED_CLASS_EXT` 这个符号 —— 所以"在 6.6 上使能 sched_ext"
不是改一个配置项，而是要把一整个子系统搬过去。

详细分析见 **[docs/backport-6.6.md](docs/backport-6.6.md)**。

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

## 两条路线

| | 路 A：换 6.12 主线内核 | 路 B：回移进 OLK-6.6 |
|---|---|---|
| 用户态 | openEuler 24.03 | openEuler 24.03 |
| 内核 | 6.12 LTS（含 sched_ext） | openEuler 6.6 + 回移补丁 |
| 工作量 | 半天（构建 + 验证） | 数周（上万行、数十个 commit） |
| 用途 | 立刻能研究调度器行为 | 真正给 openEuler 6.6 使能 sched_ext |
| 脚本 | `10-build-kernel.sh` | 见 `docs/backport-6.6.md` 的 B0–B6 拆分 |

**建议**：先走路 A 把链路跑通，再决定要不要投入路 B。路 A 的产物（配置、验证脚本、观测手段）
在路 B 里全部可复用。

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
│   └── backport-6.6.md      # 为什么 6.6 开不了，以及两条路线
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
正常。6.6 没有这个符号。走 `10-build-kernel.sh`。

**Q: 一定要装 6.12 吗？**
sched_ext 首次合入是 6.12。6.12 也是首个含它的 LTS。更高版本当然也行，`-v` 指定即可。

**Q: 装完新内核起不来怎么办？**
用户态完全没动。重启进 GRUB 选回原内核即可。所以在虚拟机里做，先打快照。

**Q: sched_ext 换调度器要重启吗？**
不用，热切换是它的核心卖点。

---

## License

GPL-2.0。内核模块与内核头文件衍生物必须 GPL-2.0 兼容，本项目统一使用该协议。
