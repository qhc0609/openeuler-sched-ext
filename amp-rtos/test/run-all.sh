#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# run-all.sh —— 一次把所有测量跑完，结果写进 results/
#
# 四组：
#   1. 共享内存通道自检            （先把"硬件"单独验一遍）
#   2. 裸 1 kHz 定时器的精度下限   （这台机器能给到多准）
#   3. 自旋窗口扫描                （精度和门铃延迟在抢同一颗核）
#   4. 隔核 / 不隔核               （taskset + SCHED_FIFO 有没有用）
#
# 第 2 组最关键：后面看到的毛刺，先要能分清是 AMP 的锅还是这台虚拟机的锅。

set -u
cd "$(dirname "$0")/.."

N=${N:-1000}
mkdir -p results

make -s all || exit 1

echo "=========== 1/4 共享内存通道自检 ==========="
./bin/amp_shm_test 20 > results/shm-test.log 2>&1
tail -3 results/shm-test.log | sed 's/^/  /'

echo
echo "=========== 2/4 裸 1 kHz 定时器精度（平台下限）==========="
# 只报分档占比，不报最大值：这台虚拟机会偶发几百毫秒的整体挂起，
# 一个毛刺就能把 p90 拖走，看不出常态。
printf "  %-10s %10s %10s %12s %12s\n" \
	"自旋窗口" "p50(us)" "p99(us)" "<50us占比" "<1ms占比"
for w in 0 300 700 900; do
	./bin/tick_floor 5 3 "$w" > "results/tick-floor-$w.log" 2>&1
	p50=$(grep '偏差' "results/tick-floor-$w.log" | sed 's/.*p50 \([0-9.]*\) us.*/\1/')
	p99=$(grep '偏差' "results/tick-floor-$w.log" | sed 's/.*p99 \([0-9.]*\) us.*/\1/')
	f50=$(grep '<     50 us' "results/tick-floor-$w.log" | awk '{print $5}')
	u1=$(awk '/^    </{s+=$4} /^    >=/{t+=$4} END{printf "%.1f%%", 100*s/(s+t)}' \
		"results/tick-floor-$w.log")
	printf "  %-10s %10s %10s %12s %12s\n" \
		"${w} us" "$p50" "$p99" "$f50" "$u1"
done

echo
echo "=========== 3/4 自旋窗口扫描：精度换延迟 ==========="
printf "  %-10s %10s %10s %10s %8s %10s\n" \
	"自旋" "RTT中位" "RTT p99" "RTT平均" "<50us" "迟到率"
for w in 0 300 700 900; do
	MODE=isolated AMP_SPIN_US="$w" RUNTIME=3 bash test/run-amp.sh "$N" \
		> "results/run-spin-$w.log" 2>&1
	med=$(grep '中位' "results/run-spin-$w.log" | head -1 | awk '{print $2}')
	p99=$(grep 'p99 ' "results/run-spin-$w.log" | head -1 | awk '{print $2}')
	avg=$(grep '平均' "results/run-spin-$w.log" | head -1 | awk '{print $2}')
	fast=$(grep '<     50 us' "results/run-spin-$w.log" | awk '{print $4}')
	late=$(grep '迟到超过半拍' "results/run-spin-$w.log" | sed 's/.*（\(.*\)）/\1/')
	printf "  %-10s %10s %10s %10s %8s %10s\n" \
		"${w} us" "${med:-?}" "${p99:-?}" "${avg:-?}" "${fast:-?}" "${late:-?}"
done

echo
echo "=========== 4/4 隔核 / 不隔核（自旋 300 us）==========="
printf "  %-10s %10s %10s %10s %10s\n" "模式" "RTT中位" "RTT p99" "RTT平均" "RTT最大"
for m in isolated shared; do
	MODE="$m" AMP_SPIN_US=300 RUNTIME=3 bash test/run-amp.sh "$N" \
		> "results/run-$m.log" 2>&1
	med=$(grep '中位' "results/run-$m.log" | head -1 | awk '{print $2}')
	p99=$(grep 'p99 ' "results/run-$m.log" | head -1 | awk '{print $2}')
	avg=$(grep '平均' "results/run-$m.log" | head -1 | awk '{print $2}')
	mx=$(grep '最大' "results/run-$m.log" | head -1 | awk '{print $2}')
	printf "  %-10s %10s %10s %10s %10s\n" "$m" "${med:-?}" "${p99:-?}" "${avg:-?}" "${mx:-?}"
done

echo
echo "结果都在 results/，单次完整日志见 results/run-*.log"
