#!/bin/bash
# run-latency.sh — 大脑想得越久，动作会晚多少
#
# 这一组要回答的问题是：大脑的推理耗时会不会直接变成机器人的动作延迟？
#
# 三档推理耗时：0 / 10 ms / 40 ms。规划周期是 50 ms，所以 40 ms 已经快把周期吃满。
# 看两件事：
#   1. 端到端延迟（轨迹点被规划出来的时刻 → 被小脑执行完的时刻）涨了多少
#   2. 小脑的周期有没有被带歪——这才是"大小脑分离"的意义
#
# 用法: ./run-latency.sh [每组秒数]
source "$(dirname "$0")/common.sh"

DUR=${1:-6}

bench() {
    local tag=$1 think=$2

    reset_shm
    start_spin "0 2 3"

    "$BIN/cerebellum" --transport "$SHM" --quiet --cpu 1 --rt 50 \
        > "$OUT/$tag-cereb.log" 2>&1 &
    local cp=$!
    sleep 0.4

    "$BIN/brain" --transport "$SHM" --duration-s "$DUR" --think-us "$think" \
        > "$OUT/$tag-brain.log" 2>&1

    kill -TERM "$cp" 2>/dev/null
    wait "$cp" 2>/dev/null
    stop_spin

    report_line "$tag"
}

banner "大脑推理耗时 → 动作延迟（每组 ${DUR}s）"
printf "%-22s %s\n" "推理耗时" "结果"
echo "-------------------------------------------------------------------------------"

bench "推理0us"    0
bench "推理10ms"   10000
bench "推理40ms"   40000
bench "推理80ms"   80000

echo "-------------------------------------------------------------------------------"
echo "推理 80 ms 已经超过 50 ms 的规划周期，大脑开始跟不上。"
echo "此时端到端延迟会涨，但小脑的周期 max 应当基本不变——这就是分离的意义。"
