#!/bin/bash
# run-demo.sh — 最小可看的演示
#
# 做的事：
#   1. 起小脑（1 kHz，绑核 1，SCHED_FIFO 50）
#   2. 起大脑（20 Hz 规划）
#   3. 跑几秒，看机器人末端沿圆走
#   4. 到点后正常结束大脑，再观察小脑两秒——它应当进入保持，然后安全停机
#
# 用法: ./run-demo.sh [秒数]
source "$(dirname "$0")/common.sh"

DUR=${1:-8}

banner "演示：大脑规划轨迹，小脑 1 kHz 执行，然后拔掉大脑"
reset_shm

"$BIN/cerebellum" --transport "$SHM" --hz 1000 --rt 50 --cpu 1 \
    > "$OUT/demo-cereb.log" 2>&1 &
CPID=$!
sleep 0.5

"$BIN/brain" --transport "$SHM" --hz 20 --duration-s "$DUR" \
    > "$OUT/demo-brain.log" 2>&1

echo "--- 大脑已退出，继续观察小脑 2 秒 ---"
sleep 2
kill -TERM $CPID 2>/dev/null
wait $CPID 2>/dev/null

echo
echo "大脑（每行一秒）："
grep '^\[brain\]' "$OUT/demo-brain.log" | head -"$DUR"

echo
echo "小脑状态切换："
grep '状态切换' "$OUT/demo-cereb.log" || echo "  （无切换，全程 RUN）"

echo
sed -n '/小脑汇总/,$p' "$OUT/demo-cereb.log"

echo
echo "末端轨迹（每秒一行，单位米）："
grep '状态=' "$OUT/demo-cereb.log" | head -"$DUR"
