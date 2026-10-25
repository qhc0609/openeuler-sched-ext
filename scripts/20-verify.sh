#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 20-verify.sh — 确认 sched_ext 真的在跑，并加载一个 BPF 调度器验证
#
# 判据（从弱到强）：
#   1. /sys/kernel/sched_ext/ 存在          → 内核编译进了 sched_ext
#   2. bpftool feature 有 struct_ops 支持    → BPF 侧能力齐备
#   3. 加载 scx_simple 后 root/ops 非空      → 调度器真的接管了 CPU

set -uo pipefail

C_OK="\033[32m"; C_WARN="\033[33m"; C_ERR="\033[31m"; C_END="\033[0m"
ok()   { printf "${C_OK}  [ OK ]${C_END} %s\n" "$*"; }
warn() { printf "${C_WARN}  [WARN]${C_END} %s\n" "$*"; }
err()  { printf "${C_ERR}  [FAIL]${C_END} %s\n" "$*"; }
hdr()  { printf "\n\033[1m== %s ==\033[0m\n" "$*"; }

RC=0

hdr "1. 内核"
echo "  uname -r : $(uname -r)"
echo "  cmdline  : $(cat /proc/cmdline)"

hdr "2. sched_ext 是否编进内核"
if [[ -d /sys/kernel/sched_ext ]]; then
    ok "/sys/kernel/sched_ext 存在"
    for f in nr_rejected nr_resched root enable_seq; do
        [[ -e "/sys/kernel/sched_ext/$f" ]] && echo "    $f = $(cat "/sys/kernel/sched_ext/$f" 2>/dev/null)"
    done
else
    err "/sys/kernel/sched_ext 不存在"
    echo "    内核没有 CONFIG_SCHED_CLASS_EXT=y，或版本 < 6.12"
    echo "    下一步： ./scripts/10-build-kernel.sh -i"
    RC=1
fi

hdr "3. BPF struct_ops 能力"
if command -v bpftool >/dev/null 2>&1; then
    ok "bpftool 已安装"
    if bpftool feature probe kernel 2>/dev/null | grep -qi "struct_ops"; then
        ok "内核支持 BPF struct_ops"
    else
        warn "bpftool 未报告 struct_ops（部分版本不打印此项，可忽略）"
    fi
    n="$(bpftool struct_ops list 2>/dev/null | wc -l)"
    n="${n:-0}"
    echo "    struct_ops 已注册数量: ${n}"
else
    warn "没有 bpftool： sudo dnf install -y bpftool"
fi

hdr "4. 加载一个真实调度器"
SCX_BIN="$(command -v scx_simple || true)"
if [[ -z "$SCX_BIN" ]]; then
    for p in "$HOME/scx/target/release/scx_simple" "/usr/bin/scx_simple" \
             "$HOME/src/scx/target/release/scx_simple"; do
        [[ -x "$p" ]] && { SCX_BIN="$p"; break; }
    done
fi

if [[ -z "$SCX_BIN" ]]; then
    warn "没找到 scx_simple，跳过实机验证"
    cat <<'EOF'

    构建 sched-ext 用户态工具（需要 rust + meson）：
      sudo dnf install -y rust cargo meson ninja-build clang llvm libbpf-devel bpftool
      git clone https://github.com/sched-ext/scx ~/scx
      cd ~/scx && meson setup build && ninja -C build
      sudo ./build/scx_simple &

    或者只构建 C 版最小调度器：
      cd ~/scx && make -C scheds/c

EOF
else
    ok "找到 $SCX_BIN"
    echo "    后台启动 10 秒，观察 root/ops ..."
    sudo "$SCX_BIN" >/tmp/scx_simple.log 2>&1 &
    SCX_PID=$!
    sleep 10
    if [[ -r /sys/kernel/sched_ext/root/ops ]]; then
        ops="$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || true)"
        if [[ -n "$ops" ]]; then
            ok "调度器已接管 CPU，root/ops = ${ops}"
        else
            warn "root/ops 为空，调度器可能没加载成功"
            echo "    日志："
            sed 's/^/      /' /tmp/scx_simple.log || true
            RC=1
        fi
    fi
    sudo kill "$SCX_PID" 2>/dev/null || true
    wait "$SCX_PID" 2>/dev/null || true
    echo "    已停止 scx_simple，CPU 回到 CFS"
fi

hdr "结论"
if ((RC == 0)); then
    ok "sched_ext 验证通过"
else
    err "还有项目没通过，见上面的 [FAIL]"
fi
exit "$RC"
