#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 10-build-kernel.sh — 在 openEuler 24.03 上得到一个带 sched_ext 的内核
#
# 背景：sched_ext 是 Linux 6.12 才合入主线的。openEuler 24.03 的基线是 6.6，
#       OLK-6.6 树里既没有 kernel/sched/ext.c，也没有 ext.o，只留了一行
#       "# CONFIG_SCHED_CLASS_EXT is not set"（符号在、代码不在，改开关无效）。
#       所以"在 6.6 上使能 sched_ext"只有两条路：
#
#         A) 保留 openEuler 24.03 的用户态，换成 6.12+ 主线内核   ← 本脚本实现
#         B) 把 sched_ext 反向移植进 openEuler 6.6 内核          ← 见 docs/backport-6.6.md
#
# A 是"今天就能跑起来"的路，B 是"这个项目真正的研究内容"。

set -euo pipefail

# Resolve the repo root BEFORE any cd. BASH_SOURCE[0] is usually a relative path
# ("./scripts/10-build-kernel.sh"), so computing it after cd-ing into the kernel
# source tree silently points at the wrong directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

KVER="${KVER:-6.12}"
JOBS="${JOBS:-$(nproc)}"
SRC="${SRC:-$HOME/src}"
DO_INSTALL=0

usage() {
    cat <<'EOF'
用法: ./scripts/10-build-kernel.sh [选项]

  -v, --kernel-version <x.y>   主线内核版本（默认 6.12，sched_ext 的首个 LTS）
  -j, --jobs <n>               并行度（默认 nproc）
  -s, --src <dir>              源码目录（默认 ~/src）
  -i, --install                构建完成后安装并更新 GRUB（需要 root）
  -h, --help                   显示本帮助

示例:
  ./scripts/10-build-kernel.sh                    # 只构建，不安装
  ./scripts/10-build-kernel.sh -v 6.12 -i         # 构建 + 安装
  KVER=6.12 JOBS=4 ./scripts/10-build-kernel.sh -i

注意:
  * 6.12 是首个包含 sched_ext 的 LTS 内核。
  * 安装后 GRUB 里会多一个条目，默认选新的；炸了就在 GRUB 里选回旧内核。
  * 在虚拟机里做这件事之前，先打快照。
EOF
}

while (($#)); do
    case "$1" in
        -v|--kernel-version) KVER="$2"; shift 2 ;;
        -j|--jobs)           JOBS="$2"; shift 2 ;;
        -s|--src)            SRC="$2";  shift 2 ;;
        -i|--install)        DO_INSTALL=1; shift ;;
        -h|--help)           usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

log()  { printf "\033[36m[build]\033[0m %s\n" "$*"; }
die()  { printf "\033[31m[error]\033[0m %s\n" "$*" >&2; exit 1; }

[[ "$KVER" =~ ^[0-9]+\.[0-9]+$ ]] || die "版本号格式应为 x.y，例如 6.12"

log "目标内核 : linux-${KVER}"
log "并行度   : ${JOBS}"
log "源码目录 : ${SRC}"
log "安装     : $([[ $DO_INSTALL == 1 ]] && echo yes || echo no)"

# ---------------------------------------------------------------- 依赖
log "检查构建依赖"
PKGS=(gcc make bc bison flex elfutils-libelf-devel openssl-devel dwarves
      perl python3 rsync cpio xz tar)
MISSING=()
for p in "${PKGS[@]}"; do
    rpm -q "$p" >/dev/null 2>&1 || MISSING+=("$p")
done
if ((${#MISSING[@]})); then
    log "安装缺失依赖: ${MISSING[*]}"
    sudo dnf install -y "${MISSING[@]}" || die "依赖安装失败"
else
    log "依赖齐备"
fi

# ---------------------------------------------------------------- 取源码
mkdir -p "$SRC"
cd "$SRC"
TARBALL="linux-${KVER}.tar.xz"
if [[ ! -f "$TARBALL" ]]; then
    MAJ="${KVER%%.*}"
    log "下载 ${TARBALL}"
    curl -fL --retry 3 -o "$TARBALL" \
        "https://cdn.kernel.org/pub/linux/kernel/v${MAJ}.x/${TARBALL}" \
        || die "下载失败，检查网络或版本号是否存在"
fi

if [[ ! -d "linux-${KVER}" ]]; then
    log "解压"
    tar xf "$TARBALL"
fi
cd "linux-${KVER}"

# ---------------------------------------------------------------- 配置
log "以当前运行内核的配置为基线"
if [[ -r "/boot/config-$(uname -r)" ]]; then
    cp "/boot/config-$(uname -r)" .config
    log "已复制 /boot/config-$(uname -r)"
else
    log "没有 /boot/config-$(uname -r)，改用 defconfig"
    make defconfig
fi

FRAG="${REPO_ROOT}/configs/sched-ext.fragment"
[[ -r "$FRAG" ]] || die "找不到配置片段: $FRAG"

log "合并 sched_ext 配置片段"
./scripts/kconfig/merge_config.sh -m -O . .config "$FRAG" >/dev/null

# openEuler 的 /boot/config 把 CONFIG_SYSTEM_TRUSTED_KEYS 指向自家签名证书
# (certs/openeuler-cert.pem)。原版内核源码树里没有这个文件，于是 certs 阶段直接报
#   make[3]: *** 没有规则可制作目标"certs/openeuler-cert.pem"
# 本项目不涉及内核模块签名信任链，清空即可（内核会改用自签名）。
if grep -qE '^CONFIG_SYSTEM_TRUSTED_KEYS=".+"' .config; then
    log "清空 CONFIG_SYSTEM_TRUSTED_KEYS（发行版私有证书在原版树里不存在）"
    ./scripts/config --file .config --set-str SYSTEM_TRUSTED_KEYS ""
fi
if grep -qE '^CONFIG_SYSTEM_REVOCATION_KEYS=".+"' .config; then
    log "清空 CONFIG_SYSTEM_REVOCATION_KEYS"
    ./scripts/config --file .config --set-str SYSTEM_REVOCATION_KEYS ""
fi

make olddefconfig

log "校验关键符号"
FAIL=0
for sym in CONFIG_SCHED_CLASS_EXT CONFIG_BPF_SYSCALL CONFIG_BPF_JIT \
           CONFIG_DEBUG_INFO_BTF CONFIG_EXT_GROUP_SCHED; do
    line="$(grep -E "^${sym}=" .config || true)"
    if [[ "$line" == "${sym}=y" ]]; then
        printf "  \033[32m%s\033[0m\n" "$line"
    else
        printf "  \033[31m%s (未生效)\033[0m\n" "${line:-${sym} 缺失}"
        FAIL=1
    fi
done
((FAIL == 0)) || die "配置校验失败。常见原因：内核版本低于 6.12，或 pahole 太旧导致 DEBUG_INFO_BTF 不可用"

# ---------------------------------------------------------------- 编译
log "开始编译（${JOBS} 线程，预计 20-60 分钟）"
time make -j"$JOBS"

[[ -f arch/x86/boot/bzImage ]] || die "没生成 arch/x86/boot/bzImage"
log "构建成功: $(pwd)/arch/x86/boot/bzImage"
ls -lh arch/x86/boot/bzImage | awk '{print "       大小: "$5}'

# ---------------------------------------------------------------- 安装
if ((DO_INSTALL)); then
    log "安装模块"
    sudo make modules_install
    log "安装内核"
    sudo make install

    log "更新 GRUB"
    if [[ -d /sys/firmware/efi ]]; then
        sudo grub2-mkconfig -o /boot/efi/EFI/openEuler/grub.cfg
    else
        sudo grub2-mkconfig -o /boot/grub2/grub.cfg
    fi

    NEWKREL="$(make -s kernelrelease)"

    # grub2-mkconfig 只重写菜单，不会改 saved_entry（openEuler 用 GRUB_DEFAULT=saved）。
    # 不显式设置的话，重启仍然进旧内核 —— 这一条踩过。
    log "把默认启动项设为 ${NEWKREL}"
    sudo grubby --set-default "/boot/vmlinuz-${NEWKREL}" \
        || log "警告: grubby 设置默认项失败，重启时请在 GRUB 菜单里手动选 ${NEWKREL}"
    grubby --default-kernel

    log "重建 initramfs"
    sudo dracut -f --kver "$NEWKREL"

    cat <<EOF

────────────────────────────────────────────────
安装完成。重启后确认：

  uname -r                       # 应为 ${KVER}.x
  ls /sys/kernel/sched_ext/      # 存在即 sched_ext 已启用

如果不启动：在 GRUB 菜单里选回原内核即可，用户态没动过。
────────────────────────────────────────────────
EOF
else
    log "跳过安装（加 -i 才会安装）"
fi
