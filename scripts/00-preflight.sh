#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 00-preflight.sh — 确认基线环境，并判定 sched_ext 是否可用
#
# 目标：openEuler 24.03 LTS（内核 6.6），而不是 5.10 / 5.12 一类的旧基线。
# 输出一个明确的结论：READY / NEED_KERNEL_BUILD / NEED_BACKPORT
#
# 用法：  ./scripts/00-preflight.sh
# 退出码：0 = sched_ext 已可用；1 = 需要后续步骤（不是错误）

set -uo pipefail

C_OK="\033[32m"; C_WARN="\033[33m"; C_ERR="\033[31m"; C_DIM="\033[2m"; C_END="\033[0m"
ok()   { printf "${C_OK}  [ OK ]${C_END} %s\n" "$*"; }
warn() { printf "${C_WARN}  [WARN]${C_END} %s\n" "$*"; }
err()  { printf "${C_ERR}  [FAIL]${C_END} %s\n" "$*"; }
dim()  { printf "${C_DIM}         %s${C_END}\n" "$*"; }
hdr()  { printf "\n\033[1m== %s ==\033[0m\n" "$*"; }

FAILED=0
NEED_BUILD=0

# ---------------------------------------------------------------- 1. 发行版
hdr "1. 发行版"
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "  PRETTY_NAME : ${PRETTY_NAME:-unknown}"
    echo "  ID/VERSION  : ${ID:-?} ${VERSION_ID:-?}"

    if [[ "${ID:-}" == "openEuler" ]]; then
        case "${VERSION_ID:-}" in
            24.03*) ok "openEuler 24.03 LTS 基线正确（OLK-6.6，无 sched_ext 回移）" ;;
            26.09*) ok "openEuler 26.09（6.6 内核已回移 sched_ext，但默认 =n）"
                    dim "也可直接装 6.18 内核包：dnf install kernel-6.18.48" ;;
            22.03*) err "这是 openEuler 22.03（5.10 基线），不是 24.03" ; FAILED=1 ;;
            *)      warn "openEuler 但不是 24.03/26.09（${VERSION_ID}），请自行确认" ;;
        esac
    else
        err "不是 openEuler（ID=${ID:-unknown}）"
        dim "本项目只针对 openEuler 24.03 LTS"
        FAILED=1
    fi
else
    err "读不到 /etc/os-release"
    FAILED=1
fi

# ---------------------------------------------------------------- 2. 内核版本
hdr "2. 内核版本"
KREL="$(uname -r)"
echo "  uname -r    : ${KREL}"

KMAJ="${KREL%%.*}"
KREST="${KREL#*.}"; KMIN="${KREST%%.*}"
if [[ "$KMAJ" =~ ^[0-9]+$ && "$KMIN" =~ ^[0-9]+$ ]]; then
    echo "  解析为      : ${KMAJ}.${KMIN}"
    if (( KMAJ == 6 && KMIN >= 6 )); then
        ok "内核 >= 6.6（符合 openEuler 24.03 基线）"
    elif (( KMAJ == 5 )); then
        err "内核是 5.x —— 这是 22.03 时代的基线，不是 24.03"
        FAILED=1
    else
        warn "内核版本不在预期范围（${KMAJ}.${KMIN}）"
    fi
else
    warn "无法解析 uname -r，跳过版本判定"
fi

# ---------------------------------------------------------------- 3. 内核配置
hdr "3. 内核配置"
CFG=""
for c in "/boot/config-${KREL}" "/proc/config.gz"; do
    if [[ -r "$c" ]]; then CFG="$c"; break; fi
done

if [[ -z "$CFG" ]]; then
    warn "找不到内核配置（容器里通常如此）"
    dim "在真实 openEuler 上应有 /boot/config-${KREL}"
    NEED_BUILD=1
else
    echo "  配置来源    : ${CFG}"
    if [[ "$CFG" == *.gz ]]; then
        cfggrep() { zcat "$CFG" | grep -E "$1" || true; }
    else
        cfggrep() { grep -E "$1" "$CFG" || true; }
    fi

    for sym in CONFIG_SCHED_CLASS_EXT CONFIG_BPF_SYSCALL CONFIG_BPF_JIT \
               CONFIG_BPF_JIT_ALWAYS_ON CONFIG_DEBUG_INFO_BTF CONFIG_EXT_GROUP_SCHED; do
        line="$(cfggrep "^${sym}=" )"
        if [[ -z "$line" ]]; then
            warn "${sym} 不存在于配置中"
            if [[ "$sym" == "CONFIG_SCHED_CLASS_EXT" ]]; then
                NEED_BUILD=1
                dim "该内核树没有回移 sched_ext（24.03 LTS 即如此）"
            fi
        elif [[ "$line" == *"=y" ]]; then
            ok "${line}"
        else
            warn "${line}（不是 =y）"
            if [[ "$sym" == "CONFIG_SCHED_CLASS_EXT" ]]; then
                NEED_BUILD=1
                dim "符号存在但被关掉 —— 该树已回移，改 =y 重编即可（26.09 即如此）"
            fi
        fi
    done
fi

# ---------------------------------------------------------------- 4. 运行时状态
hdr "4. 运行时状态（最权威的判据）"
if [[ -d /sys/kernel/sched_ext ]]; then
    ok "/sys/kernel/sched_ext 存在 —— 内核确实编译进了 sched_ext"
    if [[ -r /sys/kernel/sched_ext/root/ops ]]; then
        ops="$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || true)"
        echo "  当前加载的调度器 : ${ops:-<none>}"
    fi
    for f in nr_rejected nr_resched root; do
        [[ -e "/sys/kernel/sched_ext/$f" ]] && dim "/sys/kernel/sched_ext/$f"
    done
else
    warn "/sys/kernel/sched_ext 不存在 —— sched_ext 未启用"
    NEED_BUILD=1
fi

# ---------------------------------------------------------------- 5. 工具链
hdr "5. 工具链"
need_tools=(gcc make bc bison flex elfutils-libelf-devel openssl-devel dwarves)
missing=()
for t in "${need_tools[@]}"; do
    case "$t" in
        gcc) cmd=gcc ;; make) cmd=make ;; bc) cmd=bc ;; bison) cmd=bison ;;
        flex) cmd=flex ;; dwarves) cmd=pahole ;;
        elfutils-libelf-devel|openssl-devel) cmd="" ;;  # 头文件包，无法用 which 判断
    esac
    if [[ -n "$cmd" ]] && ! command -v "$cmd" >/dev/null 2>&1; then
        missing+=("$t")
    fi
done
if ((${#missing[@]})); then
    warn "缺少：${missing[*]}"
    dim "sudo dnf install -y gcc make bc bison flex elfutils-libelf-devel openssl-devel dwarves"
else
    ok "编译工具链齐备"
fi

if command -v bpftool >/dev/null 2>&1; then
    ok "bpftool $(bpftool version 2>/dev/null | head -1)"
else
    warn "没有 bpftool（sudo dnf install -y bpftool）"
fi

# ---------------------------------------------------------------- 结论
hdr "结论"
if (( FAILED )); then
    err "基线不符合要求，先修好上面标 FAIL 的项"
    exit 1
elif (( NEED_BUILD )); then
    warn "基线正确，但 sched_ext 不可用 → 需要自己构建内核"
    dim "下一步： ./scripts/10-build-kernel.sh --help"
    dim "更省事： 在 26.09 上 dnf install kernel-6.18.48（十几分钟，无需编译）"
    dim "版本矩阵： docs/openeuler-kernel-matrix.md"
    dim "回移分析： docs/backport-6.6.md"
    exit 1
else
    ok "sched_ext 已可用，直接跑 ./scripts/20-verify.sh"
    exit 0
fi
