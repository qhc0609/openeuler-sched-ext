#!/bin/bash
# run-jitter.sh — 小脑的控制周期在不同隔离条件下抖成什么样
#
# 四组，每组都跑同样的负载：小脑 1 kHz 执行，大脑 20 Hz 规划，四个纯 CPU 占用进程捣乱。
# 唯一变化的是"小脑被怎么对待"：
#   A  不绑核、普通调度、干扰也不绑核        —— 完全混在一起
#   B  绑到 CPU1、普通调度、干扰也压在 CPU1   —— 绑了核但还在跟人抢
#   C  绑到 CPU1、SCHED_FIFO 50、干扰压 CPU1  —— 抢占，实时任务永远先跑
#   D  绑到 CPU1、SCHED_FIFO 50、干扰在别的核 —— 独占一个核
#
# 用法: ./run-jitter.sh [每组秒数]
source "$(dirname "$0")/common.sh"

DUR=${1:-6}

bench() {
    local tag=$1 spin=$2
    shift 2

    reset_shm
    start_spin "$spin"

    "$BIN/cerebellum" --transport "$SHM" --quiet "$@" \
        > "$OUT/$tag-cereb.log" 2>&1 &
    local cp=$!
    sleep 0.4

    "$BIN/brain" --transport "$SHM" --duration-s "$DUR" \
        > "$OUT/$tag-brain.log" 2>&1

    kill -TERM "$cp" 2>/dev/null
    wait "$cp" 2>/dev/null
    stop_spin

    report_line "$tag"
}

banner "小脑周期抖动：四组对照（每组 ${DUR}s，四个干扰进程）"
printf "%-22s %s\n" "配置" "结果"
echo "-------------------------------------------------------------------------------"

bench "A-混在一起"        "any any any any"
bench "B-绑核-不实时"     "1 1 1 1"        --cpu 1
bench "C-绑核-FIFO-同核"  "1 1 1 1"        --cpu 1 --rt 50
bench "D-绑核-FIFO-独占"  "0 2 3"          --cpu 1 --rt 50

echo "-------------------------------------------------------------------------------"
echo "周期 max 是小脑两次循环之间最长的那一次，单位纳秒。1 kHz 的目标周期是 1000000 ns。"
echo "超时列统计超过 1.5 倍周期（1.5 ms）的次数——对 1 kHz 控制环，这就是丢拍。"
