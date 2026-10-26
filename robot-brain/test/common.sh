#!/bin/bash
# common.sh — 四组测试共用的起停与取数逻辑
#
# 约定：
#   共享内存名固定 /robot-brain（spec 写成 shm:/robot-brain）
#   小脑先起，它负责初始化共享区；大脑后起，等魔数
#   所有输出落在 test/out/

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/bin"
OUT="$ROOT/test/out"
SHM="shm:/robot-brain"
SHMFILE="/dev/shm/robot-brain"

mkdir -p "$OUT"

SPIN_PIDS=()

reset_shm() {
    rm -f "$SHMFILE"
}

start_spin() {
    # $1 可以是 "any"（不绑核）、"none"（不起干扰）、或空格分隔的核号列表
    local spec="$1"
    stop_spin
    [ "$spec" = "none" ] && return 0
    for c in $spec; do
        if [ "$c" = "any" ]; then
            "$BIN/spin" 0 &
        else
            taskset -c "$c" "$BIN/spin" 0 &
        fi
        SPIN_PIDS+=($!)
        disown 2>/dev/null      # 不然收尸时会往终端打一堆 Killed
    done
}

stop_spin() {
    local p
    for p in "${SPIN_PIDS[@]:-}"; do
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    SPIN_PIDS=()
}

# 从日志里抠一个数值：pick <文件> <关键字> <第几列>
pick() {
    local f=$1 key=$2 col=${3:-1}
    grep -F "$key" "$f" 2>/dev/null | tail -1 | awk -v c="$col" '{print $c}'
}

# 打一行结果，字段固定，方便直接贴进文章
report_line() {
    local tag=$1 log="$OUT/$1-cereb.log"
    local p50 pmax miss misspct buf lat_avg track

    p50=$(pick "$log" "周期 min/avg/max" 5)
    pmax=$(pick "$log" "周期 min/avg/max" 7)
    miss=$(pick "$log" "超时(>1.5倍)" 2)
    misspct=$(pick "$log" "超时(>1.5倍)" 3)
    buf=$(pick "$log" "缓冲余量最小" 2)
    lat_avg=$(pick "$log" "计划年龄" 2 | sed 's/avg=//')
    track=$(pick "$log" "最大跟随误差" 2)

    printf "%-20s 周期avg=%-9s max=%-9s 丢拍=%-6s %-9s 缓冲=%-5s 跟随误差=%s\n" \
        "$tag" "${p50:-NA}" "${pmax:-NA}" "${miss:-NA}" "${misspct:-}" \
        "${buf:-NA}" "${track:-NA}"
}

banner() {
    echo
    echo "==============================================================="
    echo " $1"
    echo "==============================================================="
}
