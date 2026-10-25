#!/usr/bin/env bash
# 原型：用 ftrace 的 sched_switch 量「单次连续占核时长」
#
# 只关心一个 pid：它被换上来时记 t_in，被换下去时记 t_out，差值就是一次占核。
# 这个量和调度延迟是两个东西：
#   调度延迟 = 该跑 -> 真跑      （等的时间，越小越好）
#   占核时长 = 真跑 -> 被换下    （占的时间，同核的别人要等这么久）
#
# 用法：
#   ./slice-probe.sh '' cfs
#   ./slice-probe.sh 'nice -n -20' nice
#   ./slice-probe.sh 'chrt -f 50' fifo
#   ./slice-probe.sh '' scx1 '--kick 1 --burst 1 --wake-boost 1 --ai-slice-us 1000 --other-slice-us 3000'
set -uo pipefail
T=/sys/kernel/debug/tracing
cd "$(dirname "$0")" || exit 1

PRE="${1:-}"
LABEL="${2:-cfs}"
SCHED_ARGS="${3:-}"

PIDFILE=/tmp/slice.pid
GO=/tmp/slice.go
OUT=/tmp/slice.trace
DUR=/tmp/slice.dur
rm -f "$PIDFILE" "$GO" "$OUT" "$DUR"

./loadgen 8 12 >/dev/null 2>&1 &
LG=$!
sleep 0.5

# shellcheck disable=SC2086
$PRE ./tiny_infer --requests 40 --interval-ms 150 --passes 1300 \
	--pidfile "$PIDFILE" --wait "$GO" --json >"/tmp/slice-eng-$LABEL.json" 2>&1 &
ENG=$!

for _ in $(seq 1 200); do [ -s "$PIDFILE" ] && break; sleep 0.02; done
PID=$(cat "$PIDFILE")

SCHED_PID=""
if [ -n "$SCHED_ARGS" ]; then
	# shellcheck disable=SC2086
	../scheds/ai_prio ../scheds/ai_prio.bpf.o "$PID" $SCHED_ARGS \
		--quiet >"/tmp/slice-sched-$LABEL.log" 2>&1 &
	SCHED_PID=$!
	for _ in $(seq 1 150); do
		grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null && break
		sleep 0.02
	done
	if ! grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null; then
		echo "调度器没挂上：" >&2
		cat "/tmp/slice-sched-$LABEL.log" >&2
		exit 1
	fi
fi

echo 0 > "$T/tracing_on"
echo > "$T/trace"
echo 16384 > "$T/buffer_size_kb"
echo "prev_pid == $PID || next_pid == $PID" > "$T/events/sched/sched_switch/filter"
echo 1 > "$T/events/sched/sched_switch/enable"
echo 1 > "$T/tracing_on"

touch "$GO"
sleep 4

echo 0 > "$T/tracing_on"
echo 0 > "$T/events/sched/sched_switch/enable"
cat "$T/trace" > "$OUT"
echo 0 > "$T/events/sched/sched_switch/filter"

if [ -n "$SCHED_PID" ]; then
	kill -INT "$SCHED_PID" 2>/dev/null
	wait "$SCHED_PID" 2>/dev/null
fi

kill "$LG" 2>/dev/null
wait "$LG" 2>/dev/null

awk -v pid="$PID" '
	/sched_switch:/ {
		ts = ""
		for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+:$/) { ts = $i; sub(":", "", ts); break }
		if (ts == "") next
		pv = ""; nx = ""
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^prev_pid=/) { pv = $i; sub("prev_pid=", "", pv) }
			if ($i ~ /^next_pid=/) { nx = $i; sub("next_pid=", "", nx) }
		}
		if (nx == pid) t_in = ts
		if (pv == pid && t_in > 0) {
			d = (ts - t_in) * 1000000
			if (d >= 0) print int(d)
			t_in = 0
		}
	}
' "$OUT" > "$DUR"

N=$(wc -l < "$DUR")
if [ "$N" -eq 0 ]; then echo "$LABEL 没有采到样本"; exit 1; fi
pick() { sort -n "$DUR" | sed -n "$1p"; }
printf '%-8s 样本 %-5s p50 %-6s p90 %-6s p99 %-6s max %s\n' "$LABEL" "$N" \
	"$(pick $((N * 50 / 100 + 1)))" \
	"$(pick $((N * 90 / 100 + 1)))" \
	"$(pick $((N * 99 / 100 + 1)))" \
	"$(pick "$N")"
