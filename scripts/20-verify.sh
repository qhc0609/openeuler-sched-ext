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
# 允许外部指定： SCX_BIN=/path/to/scx_simple ./scripts/20-verify.sh
SCX_BIN="${SCX_BIN:-$(command -v scx_simple || true)}"
if [[ -z "$SCX_BIN" ]]; then
    # 内核源码树自带的示例最省事（纯 C，不需要 rust）：
    #   cd ~/src/linux-<ver>/tools/sched_ext && make
    for p in "$HOME"/src/linux-*/tools/sched_ext/build/bin/scx_simple \
             /usr/bin/scx_simple \
             "$HOME"/scx/target/release/scx_simple; do
        [[ -x "$p" ]] && { SCX_BIN="$p"; break; }
    done
fi

if [[ -z "$SCX_BIN" ]]; then
    warn "没找到 scx_simple，跳过实机验证（内核层面已通过，但没证明真能接管）"
    cat <<'EOF'

    最省事的办法：用你刚编的内核源码树自带的示例（纯 C，不需要 rust）
      sudo dnf install -y clang llvm libbpf-devel bpftool
      cd ~/src/linux-$(uname -r | cut -d. -f1-2)/tools/sched_ext && make
      sudo ./build/bin/scx_simple &
      cat /sys/kernel/sched_ext/root/ops      # 应输出 simple

    注意：不要去 clone sched-ext/scx —— 它已经把 scheds/c 删了、全改 Rust，
    scx_simple 在那边已经不存在。

EOF
    SKIPPED=1
else
    ok "找到 $SCX_BIN"
    echo "    后台启动 10 秒，观察 root/ops ..."
    sudo "$SCX_BIN" >/tmp/scx_simple.log 2>&1 &
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
    # sudo 会把 $! 指向 sudo 自己，直接 kill 那个 pid 可能杀不掉子进程
    sudo pkill -f 'scx_simple' 2>/dev/null || true
    sleep 1
    echo "    已停止 scx_simple，CPU 回到 CFS"
fi

hdr "结论"
if ((RC != 0)); then
    err "还有项目没通过，见上面的 [FAIL]"
elif [[ "${SKIPPED:-0}" == "1" ]]; then
    warn "内核层面通过，但第 4 步被跳过 —— 还没证明调度器真能接管"
    RC=1
else
    ok "sched_ext 验证通过（已实际加载调度器并接管 CPU）"
fi
exit "$RC"
