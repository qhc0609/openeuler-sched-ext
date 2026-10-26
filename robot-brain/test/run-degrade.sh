#!/bin/bash
# run-degrade.sh — 大脑出问题的时候，机器人会不会跟着出事
#
# 三个场景，小脑的配置完全一样（1 kHz，独占 CPU1，SCHED_FIFO 50）：
#   1. 正常：大脑 20 Hz 规划
#   2. 变慢：大脑每次推理 120 ms，已经超过 50 ms 的规划周期，跟不上了
#   3. 猝死：跑到一半 kill -9，连清理都来不及
#
# 要看的不是大脑怎么样，是小脑：
#   状态有没有从 RUN 走到 HOLD、再走到 SAFE
#   跟随误差有没有失控（> 0.1 rad 就说明指令跳变了）
#   周期有没有因为大脑挂了而变差
#
# 用法: ./run-degrade.sh
source "$(dirname "$0")/common.sh"

banner "大脑失联时小脑的安全行为"

run_case() {
    local tag=$1 mode=$2

    reset_shm
    start_spin "0 2 3"

    "$BIN/cerebellum" --transport "$SHM" --quiet --cpu 1 --rt 50 \
        --wd-hold-ms 250 --wd-safe-ms 800 \
        > "$OUT/$tag-cereb.log" 2>&1 &
    local cp=$!
    sleep 0.4

    case "$mode" in
    normal)
        "$BIN/brain" --transport "$SHM" --duration-s 5 \
            > "$OUT/$tag-brain.log" 2>&1
        ;;
    slow)
        "$BIN/brain" --transport "$SHM" --duration-s 5 --think-us 120000 \
            > "$OUT/$tag-brain.log" 2>&1
        ;;
    kill)
        "$BIN/brain" --transport "$SHM" > "$OUT/$tag-brain.log" 2>&1 &
        local bp=$!
        sleep 3
        kill -9 "$bp" 2>/dev/null
        echo "  （大脑已被 kill -9，继续观察小脑 3 秒）"
        sleep 3
        ;;
    esac

    kill -TERM "$cp" 2>/dev/null
    wait "$cp" 2>/dev/null
    stop_spin

    report_line "$tag"
}

printf "%-22s %s\n" "场景" "结果"
echo "-------------------------------------------------------------------------------"
run_case "1-正常"       normal
run_case "2-大脑变慢"   slow
run_case "3-大脑猝死"   kill
echo "-------------------------------------------------------------------------------"

echo
echo "场景 3 的状态时间线（这是最关键的一段）："
grep -E '状态切换|安全停机' "$OUT/3-大脑猝死-cereb.log" || echo "  （没有状态切换）"

echo
echo "三个场景的跟随误差对比（rad）："
for t in "1-正常" "2-大脑变慢" "3-大脑猝死"; do
    v=$(pick "$OUT/$t-cereb.log" "最大跟随误差" 2)
    printf "  %-12s %s\n" "$t" "${v:-NA}"
done

echo
echo "判断标准："
echo "  跟随误差 < 0.1 rad —— 指令没有跳变，机器人不会抽搐"
echo "  场景 3 必须出现 HOLD 或 SAFE —— 说明小脑自己接管了"
echo "  三个场景的周期 max 应当在同一量级 —— 说明大脑的毛病没有传染给小脑"
