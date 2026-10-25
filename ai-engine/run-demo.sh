#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-demo.sh — 三组对照实验，量出 sched_ext 到底改善了多少
#
# 三组：
#   A  CFS          不挂任何调度器，这是基线
#   B  ai_prio/不踢  只做队列优先，不打断正在跑的任务
#   C  ai_prio/踢    队列优先 + 唤醒时强制重新调度
#
# 为什么要三组：B 和 C 只差一个 kick 开关。只比 A 和 C 的话，
# 看到变快了也说不清是"排队靠前"还是"抢占"在起作用。
#
# 为什么要跑多轮：单次结果里尾巴波动很大（一次中断、一次宿主调度都会影响）。
# 默认 3 轮，看的是同一组在不同轮次之间稳不稳，而不是某一轮的数字。
#
# 用法：
#   sudo ./run-demo.sh
#   REPS=5 THREADS=8 ./run-demo.sh
#
# 环境变量：
#   REPS        每组的重复轮数（默认 3）
#   THREADS     压测线程数（默认 2×核数）
#   REQUESTS    每轮请求数（默认 600）
#   INTERVAL    请求间隔毫秒（默认 10）
#   DURATION    压测持续秒数（默认 60）

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
SCHED_DIR="$REPO/scheds"
cd "$HERE" || exit 1

NCPU="$(nproc)"
REPS="${REPS:-3}"
THREADS="${THREADS:-$((NCPU * 2))}"
REQUESTS="${REQUESTS:-600}"
INTERVAL="${INTERVAL:-10}"
DURATION="${DURATION:-60}"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

say()  { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }

jget() { grep -o "\"$2\":[0-9]*" "$1" 2>/dev/null | head -1 | cut -d: -f2; }
median() { sort -n | awk '{a[NR]=$1} END{if(NR==0){print "-";exit} if(NR%2)print a[(NR+1)/2]; else printf "%.0f\n",(a[NR/2]+a[NR/2+1])/2}'; }

say "环境"
note "内核      $(uname -r)"
note "核数      $NCPU"
note "压测线程  $THREADS（= $((THREADS / NCPU)) 倍核数）"
note "每轮请求  $REQUESTS 个，间隔 ${INTERVAL}ms"
note "重复轮数  $REPS"

if [ ! -d /sys/kernel/sched_ext ]; then
	echo "!! /sys/kernel/sched_ext 不存在：这个内核没有 sched_ext" >&2
	exit 1
fi

say "编译"
make -C "$HERE" -s || exit 1
make -C "$SCHED_DIR" -s ai_prio ai_prio.bpf.o || exit 1
note "tiny_infer / loadgen / ai_prio 就绪"

run_phase() {
	local name="$1" kick="$2" out="$3"
	local sched_pid="" lg_pid="" eng_pid="" target

	rm -f /tmp/ai.go /tmp/ai.pid

	./loadgen "$THREADS" "$DURATION" >/dev/null 2>&1 &
	lg_pid=$!
	sleep 0.5

	./tiny_infer --requests "$REQUESTS" --interval-ms "$INTERVAL" \
		--pidfile /tmp/ai.pid --wait /tmp/ai.go --json >"$out" 2>/tmp/eng-$name.err &
	eng_pid=$!

	for _ in $(seq 1 300); do
		[ -s /tmp/ai.pid ] && break
		sleep 0.02
	done
	if [ ! -s /tmp/ai.pid ]; then
		echo "!! $name: 拿不到引擎 pid" >&2
		kill "$eng_pid" "$lg_pid" 2>/dev/null
		return 1
	fi
	target="$(cat /tmp/ai.pid)"

	if [ "$kick" != "off" ]; then
		$SUDO "$SCHED_DIR/ai_prio" "$SCHED_DIR/ai_prio.bpf.o" "$target" \
			--kick "$kick" --quiet >"/tmp/sched-$name.log" 2>&1 &
		sched_pid=$!
		for _ in $(seq 1 150); do
			grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null && break
			sleep 0.02
		done
		if ! grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null; then
			echo "!! $name: 调度器没挂上，日志：" >&2
			cat "/tmp/sched-$name.log" >&2
			kill "$eng_pid" "$lg_pid" 2>/dev/null
			return 1
		fi
	fi

	touch /tmp/ai.go
	wait "$eng_pid" 2>/dev/null

	if [ -n "$sched_pid" ]; then
		kill -INT "$sched_pid" 2>/dev/null
		wait "$sched_pid" 2>/dev/null
	fi
	kill "$lg_pid" 2>/dev/null
	wait "$lg_pid" 2>/dev/null
	return 0
}

for r in $(seq 1 "$REPS"); do
	say "第 $r/$REPS 轮"
	run_phase cfs    off "/tmp/res-cfs-$r.json"    || exit 1
	run_phase nokick 0   "/tmp/res-nokick-$r.json" || exit 1
	run_phase kick   1   "/tmp/res-kick-$r.json"   || exit 1
	printf '   cfs=%s nokick=%s kick=%s (p99, 微秒)\n' \
		"$(jget "/tmp/res-cfs-$r.json" sched_p99_us)" \
		"$(jget "/tmp/res-nokick-$r.json" sched_p99_us)" \
		"$(jget "/tmp/res-kick-$r.json" sched_p99_us)"
done

say "调度延迟 p99（微秒，每一行是一轮）"
printf '%-8s %10s %10s %10s\n' "轮次" "A CFS" "B 不踢" "C 踢"
for r in $(seq 1 "$REPS"); do
	printf '%-8s %10s %10s %10s\n' "#$r" \
		"$(jget "/tmp/res-cfs-$r.json" sched_p99_us)" \
		"$(jget "/tmp/res-nokick-$r.json" sched_p99_us)" \
		"$(jget "/tmp/res-kick-$r.json" sched_p99_us)"
done
printf '%-8s %10s %10s %10s\n' "中位数" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-cfs-$r.json" sched_p99_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-nokick-$r.json" sched_p99_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-kick-$r.json" sched_p99_us; done | median)"

say "调度延迟 p50（微秒，中位数）"
printf '%-8s %10s %10s %10s\n' "" "A CFS" "B 不踢" "C 踢"
printf '%-8s %10s %10s %10s\n' "p50" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-cfs-$r.json" sched_p50_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-nokick-$r.json" sched_p50_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-kick-$r.json" sched_p50_us; done | median)"
printf '%-8s %10s %10s %10s\n' "max" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-cfs-$r.json" sched_max_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-nokick-$r.json" sched_max_us; done | median)" \
	"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-kick-$r.json" sched_max_us; done | median)"

say "计算耗时 p50（三组应接近，说明变量只有调度）"
for g in cfs nokick kick; do
	printf '   %-8s %s us\n' "$g" \
		"$(for r in $(seq 1 "$REPS"); do jget "/tmp/res-$g-$r.json" work_p50_us; done | median)"
done

echo
note "看 p99 不看平均值：平均值被大量准点样本拉平，影响体验的是尾巴。"
note "max 单次最坏值，波动大，只看量级。"
note "三组算力相同（work_p50 接近）才说明差别来自调度，不是来自算得更快。"
