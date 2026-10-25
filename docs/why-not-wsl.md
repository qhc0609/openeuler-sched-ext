# 为什么这个项目不能靠 WSL，以及"一键"到底能一键到哪一步

## 结论先说

| 问题 | 答案 |
|---|---|
| VMware 上能一键吗？ | **能**。用 SSH 把宿主和虚拟机连起来，宿主一条命令 = 虚拟机一条命令 |
| WSL 更好吗？ | **不能用于这个项目**。WSL 跑的是 Ubuntu + 微软定制内核，不是 openEuler 的 6.6 |
| WSL + Windows + Claude + DeepSeek 是最优解吗？ | **不是**。最优解是 **Windows + Agent + DeepSeek + VMware/openEuler + SSH 桥** |
| "一键"能到什么程度？ | 发布仓库：**真一键（秒级）**；跑通验证：**一次配置后一键（分钟级）**；编内核：**永远不可能秒级** |

---

## 一、WSL 从根上就不适用于这个项目

不是因为性能，是因为**它跑的根本不是你要的东西**。

| | WSL2 | VMware |
|---|---|---|
| 用户态 | Ubuntu / Debian / Alma…（**没有官方 openEuler 发行版**） | 你自己装的 openEuler 24.03 |
| 内核 | 微软定制的 `*-microsoft-standard-WSL2` 内核 | **openEuler 自己的 6.6 内核** |
| 能否验证"openEuler 的 6.6 基线" | **不能** | 能 |
| 能否替换成自编内核 | 能（`.wslconfig kernel=`），但内核一换就脱离了 openEuler 语境 | 能，且是标准 GRUB 流程 |
| 快照 / 回滚 | 无 | 有 |
| 串口控制台 / kgdb | 无 | 有（虚拟串口） |

你用 `wsl --import` 确实能把 openEuler 的 rootfs 导进 WSL，但 **WSL 的内核是微软的，不是 openEuler 的**。
这个项目第一步要证明的就是"这是 openEuler 24.03 的 6.6 内核" —— 在 WSL 里做这件事，
结论从第一天起就是假的。

### 附带两个现实问题

1. **装 WSL2 会让你的 VMware 变慢。** WSL2 依赖 `VirtualMachinePlatform`（= 启用 Windows hypervisor），
   一旦启用，VMware 只能退到 WHP/ULM 兼容模式，CPU 性能降 20–40%。
2. **本机的 `wsl.exe` 在命令安全黑名单里**，Agent 沙箱内根本调不到。要用就得先把它从黑名单移出来，
   而移出来之后第 1 条问题照样存在。

---

## 二、VMware 不是障碍，SSH 就是那座桥

Agent（WorkBuddy / Claude Code）跑在 Windows 宿主上，内核编译必须在虚拟机里。
把两端连起来有三种方式：

| 方式 | 命令 | 评价 |
|---|---|---|
| **SSH** | `ssh root@<vm-ip> "cmd"` | **推荐**。稳定、跨平台、可脚本化、能传文件 |
| `vmrun` | `vmrun -T ws -gu u -gp p runProgramInGuest vm.vmx /bin/bash -c "cmd"` | 需要 VMware Tools，输出捕获不如 SSH 干净 |
| 共享文件夹 | `\\vmware-host\Shared Folders\...` | **只用于小文件**。内核树放这里会比 `/mnt/c` 还慢 |

本项目已经把 SSH 桥封装成 `vm-ssh.ps1`：

```powershell
# 一次性配置
Copy-Item .vm.env.example .vm.env
notepad .vm.env                      # 填 VM_HOST / VM_USER

# 之后所有操作都从 Windows 侧发起
.\vm-ssh.ps1 -Command "uname -r"
.\vm-ssh.ps1 -Upload                                  # 同步项目到虚拟机
.\vm-ssh.ps1 -Command "cd ~/openeuler-sched-ext && ./scripts/00-preflight.sh"
```

虚拟机里只需一次性准备：

```bash
sudo dnf install -y openssh-server
sudo systemctl enable --now sshd
firewall-cmd --add-service=ssh --permanent && firewall-cmd --reload
hostname -I            # 记下 IP
```

---

## 三、"一键"分三层，只有第一层是真的秒级

| 层 | 内容 | 耗时 | 能否一键 |
|---|---|---|---|
| **L1 发布仓库** | `git init` → commit → `gh repo create` → push | **5 秒** | ✅ 真一键（`push.ps1`） |
| **L2 基线验证** | 同步到虚拟机 → 跑 `00-preflight.sh` | 30 秒 | ✅ 配置一次后一键 |
| **L3 内核构建** | 下载 6.12 源码 → 编译 → 安装 → 重启 → 验证 | **30–60 分钟** | ⚠️ 能一键发起，但不可能一键完成 |

**不要为了"看起来一键"把 L3 伪装成秒级。** 一个 `make -j4` 编 6.12 全量内核，
在 4 核 4 线程的 i5-7300HQ 上就是半小时起步，这是物理事实。

正确的做法是：**L1 立刻发出去，让别人能看到项目存在；L3 后台跑，跑完再推结果。**
仓库先上线比什么都重要 —— 这也是"最快方式发布 GitHub"的真正含义。

---

## 四、最终架构

```
┌─────────────────────────── Windows 宿主 ───────────────────────────┐
│                                                                    │
│   WorkBuddy / Claude Code  ←→  DeepSeek V4-Pro (api.deepseek.com)  │
│            │                                                       │
│            ├── git / gh  ──────────────→  GitHub                   │
│            │                                                       │
│            └── vm-ssh.ps1 ── SSH ──┐                               │
│                                     │                              │
└─────────────────────────────────────┼──────────────────────────────┘
                                      │
┌─────────────────────────────────────▼──────────────────────────────┐
│  VMware：openEuler 24.03 LTS                                        │
│    ├── 内核 6.6（基线，00-preflight.sh 验证）                        │
│    ├── 构建 6.12 内核（10-build-kernel.sh）→ sched_ext 可用          │
│    ├── 验证 scx 调度器（20-verify.sh）                               │
│    └── 快照 / 虚拟串口 / kgdb（VMware 独有）                          │
└────────────────────────────────────────────────────────────────────┘
```

**Agent 放在 Windows 侧而不是虚拟机里**，原因：

- 虚拟机里装 Node + Claude Code 要多占 500MB，且每次开机要重新起
- Agent 需要读写的核心文件（仓库、脚本）在 Windows 侧，虚拟机只需要执行
- 走 SSH 之后，Agent 的能力边界和直接登录虚拟机完全一致

如果你更习惯在虚拟机里直接跑 Agent，也可以 —— 把 `docs/` 里 Claude Code 的
环境变量配到虚拟机的 `~/.bashrc` 即可，两者不冲突。

---

## 五、一句话

**VMware 不是障碍，WSL 也不是答案。** 用 SSH 把宿主和虚拟机连起来，
`push.ps1` 负责 5 秒发布，`00-preflight.sh` 负责 30 秒给结论，
`10-build-kernel.sh` 负责那半小时 —— 各司其职，才是真正的"最快"。
