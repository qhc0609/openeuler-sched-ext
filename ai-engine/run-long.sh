#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-long.sh — 长请求实验：每个请求 40 毫秒，机器被 8 个线程占满
#
# 和 run-demo.sh 的区别只有一个：请求变长了。
#   短请求（446 微秒）测的是"唤醒后多久拿到 CPU"；
#   长请求（40 毫秒）测的是"拿到 CPU 之后，别人还要等多久"。
# 后者才是线上真正的问题：一个 40 毫秒的推理请求会把一个核按住 40 毫秒，
# 这期间同一核上的其它任务一步都动不了。
#
# 六组：
#   cfs     CFS 默认                      什么都不做
#   nice    nice -n -20                   CFS 内部给 87 倍权重
#   fifo    chrt -f 50                    SCHED_FIFO，实时类，没有时间片
#   kick20  ai_prio 切片 20 ms，不限连续片数，普通任务唤醒不踢
#   kick1   ai_prio 切片  1 ms，不限连续片数，普通任务唤醒不踢
#   kickb   ai_prio 切片  1 ms，占满 1 片必须让一次，普通任务唤醒不踢
#   kickbw  ai_prio 切片  1 ms，占满 1 片必须让一次，普通任务唤醒插队头并踢核
#
# 后四组是同一个调度器、同一份代码，每一级只改一个旋钮：
#   kick20 → kick1    只把时间片从 20 ms 缩到 1 ms
#   kick1  → kickb    只加"连续占满就要让一次"这条规则
#   kickb  → kickbw   只加"普通任务唤醒时插队头并踢一脚"
# 每一列数字都能归因到一个旋钮，而不是"反正我改了代码就变好了"。
#
# 用法：
#   sudo ./run-long.sh
#   REPS=5 ./run-long.sh
#
# 环境变量：
#   REPS       每组轮数（默认 3）
#   THREADS    压测线程数（默认 2×核数）
#   REQUESTS   引擎请求数（默认 120）
#   SLA_US     引擎调度延迟阈值，微秒（默认 5000）
#   PSLA_US    探针调度延迟阈值，微秒（默认 10000）
#   INTERVAL   请求间隔毫秒（默认 150）
#   PASSES     每请求前向次数（默认 1300 ≈ 40 ms）
#   PROBE_REQ  探针请求数（默认 400）
#   PROBE_MS   探针间隔毫秒（默认 40）
#   DURATION   压测最长秒数（默认 60）

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
SCHED_DIR="$REPO/scheds"
cd "$HERE" || exit 1

NCPU="$(nproc)"
REPS="${REPS:-3}"
THREADS="${THREADS:-$((NCPU * 2))}"
REQUESTS="${REQUESTS:-120}"
INTERVAL="${INTERVAL:-150}"
PASSES="${PASSES:-1300}"
PROBE_REQ="${PROBE_REQ:-400}"
PROBE_MS="${PROBE_MS:-40}"
DURATION="${DURATION:-60}"
SLA_US="${SLA_US:-5000}"
PSLA_US="${PSLA_US:-10000}"

# 不要叫 GROUPS：bash 的保留数组，赋值会被静默忽略
PHASES="cfs nice fifo kick20 kick1 kickb kickbw"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

say()  { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }

jget() {
	grep -o "\"$2\":[0-9]*" "$1" 2>/dev/null |
		head -1 | cut -d: -f2
}

median() {
	sort -n | awk '
		{ a[NR] = $1 }
		END {
			if (NR == 0) { print "-"; exit }
			if (NR % 2) print a[(NR + 1) / 2]
			else printf "%.0f\n", (a[NR / 2] + a[NR / 2 + 1]) / 2
		}'
}

col() {
	local g="$1" tpl="$2" k="$3" r
	for r in $(seq 1 "$REPS"); do
		jget "$(printf "$tpl" "$g" "$r")" "$k"
	done | median
}

# 表达式必须自己锚定：.*[0-9]* 是贪婪的，只会留下最后一位
grepc() {
	local g="$1" tpl="$2" expr="$3" r
	for r in $(seq 1 "$REPS"); do
		sed -n "$expr" "$(printf "$tpl" "$g" "$r")" | head -1
	done | median
}

# miss <组> <模板>：把各轮的 sla_miss 和 requests 分别累加，输出 "超时/总数 百分比"
miss() {
	local g="$1" tpl="$2" r m n s=0 t=0
	for r in $(seq 1 "$REPS"); do
		m=$(jget "$(printf "$tpl" "$g" "$r")" sla_miss)
		n=$(jget "$(printf "$tpl" "$g" "$r")" requests)
		s=$((s + ${m:-0}))
		t=$((t + ${n:-0}))
	done
	if [ "$t" -le 0 ]; then echo "-"; return; fi
	awk -v s="$s" -v t="$t" 'BEGIN { printf "%d/%d %.1f%%", s, t, s * 100.0 / t }'
}

ctxt_now() { awk '/^ctxt/ {print $2}' /proc/stat; }

say "环境"
note "内核      $(uname -r)"
note "核数      $NCPU"
note "压测线程  $THREADS（= $((THREADS / NCPU)) 倍核数）"
note "引擎      $REQUESTS 个请求，间隔 ${INTERVAL}ms，每请求 $PASSES 次前向（≈40ms）"
note "探针      $PROBE_REQ 个请求，间隔 ${PROBE_MS}ms"
note "重复轮数  $REPS"

if [ ! -d /sys/kernel/sched_ext ]; then
	echo "!! /sys/kernel/sched_ext 不存在：这个内核没有 sched_ext" >&2
	exit 1
fi
command -v chrt >/dev/null || { echo "!! 缺 chrt（util-linux）" >&2; exit 1; }

say "编译"
make -C "$HERE" -s || exit 1
make -C "$SCHED_DIR" -s ai_prio ai_prio.bpf.o || exit 1
note "tiny_infer / loadgen / ai_prio 就绪"

# run_phase <组名> <轮次> <引擎输出文件>
run_phase() {
	local name="$1" r="$2" out="$3"
	local sched_pid="" lg_pid="" eng_pid="" probe_pid="" target
	local pre="" slice="" burst=0 kall=0 ctxt0 ctxt1

	rm -f /tmp/ai.go /tmp/ai.pid
	ctxt0=$(ctxt_now)

	./loadgen "$THREADS" "$DURATION" >/dev/null 2>"/tmp/lg-$name-$r.log" &
	lg_pid=$!
	sleep 0.5

	# 旁观探针：一个普通任务，每次只算 32 微秒，每 40 毫秒醒一次。
	# 它测的是"这台机器上别的活人有多难受"。
	./tiny_infer --requests "$PROBE_REQ" --interval-ms "$PROBE_MS" \
		--passes 1 --warmup 5 --sla-us "$PSLA_US" --wait /tmp/ai.go \
		--json >"/tmp/probe-$name-$r.json" &
	probe_pid=$!

	[ "$name" = nice ] && pre="nice -n -20"
	[ "$name" = fifo ] && pre="chrt -f 50"
	# shellcheck disable=SC2086
	$pre ./tiny_infer --requests "$REQUESTS" --interval-ms "$INTERVAL" \
		--passes "$PASSES" --sla-us "$SLA_US" \
		--pidfile /tmp/ai.pid --wait /tmp/ai.go --json \
		>"$out" 2>/tmp/eng-$name-$r.err &
	eng_pid=$!

	for _ in $(seq 1 300); do
		[ -s /tmp/ai.pid ] && break
		sleep 0.02
	done
	if [ ! -s /tmp/ai.pid ]; then
		echo "!! $name: 拿不到引擎 pid" >&2
		kill "$eng_pid" "$probe_pid" "$lg_pid" 2>/dev/null
		return 1
	fi
	target="$(cat /tmp/ai.pid)"

	# /proc/<pid>/stat 第 41 字段 policy（0=OTHER 1=FIFO），第 19 字段 nice
	{
		awk '{print $41}' "/proc/$target/stat" 2>/dev/null
		awk '{print $19}' "/proc/$target/stat" 2>/dev/null
	} | paste -sd' ' - >"/tmp/pol-$name-$r.txt"

	case "$name" in
	kick20|kick1|kickb|kickbw)
		slice=1000; burst=1; kall=0
		[ "$name" = kick20 ] && { slice=20000; burst=0; }
		[ "$name" = kick1 ]  && { slice=1000;  burst=0; }
		[ "$name" = kickbw ] && kall=1
		$SUDO "$SCHED_DIR/ai_prio" "$SCHED_DIR/ai_prio.bpf.o" "$target" \
			--kick 1 --burst "$burst" --wake-boost "$kall" \
			--ai-slice-us "$slice" --other-slice-us 3000 \
			--quiet >"/tmp/sched-$name-$r.log" 2>&1 &
		sched_pid=$!
		for _ in $(seq 1 150); do
			grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null && break
			sleep 0.02
		done
		if ! grep -q ai_prio /sys/kernel/sched_ext/root/ops 2>/dev/null; then
			echo "!! $name: 调度器没挂上，日志：" >&2
			cat "/tmp/sched-$name-$r.log" >&2
			kill "$eng_pid" "$probe_pid" "$lg_pid" 2>/dev/null
			return 1
		fi
		;;
	esac

	touch /tmp/ai.go
	wait "$eng_pid" 2>/dev/null

	if [ -n "$sched_pid" ]; then
		kill -INT "$sched_pid" 2>/dev/null
		wait "$sched_pid" 2>/dev/null
	fi

	wait "$probe_pid" 2>/dev/null
	kill "$lg_pid" 2>/dev/null
	wait "$lg_pid" 2>/dev/null

	ctxt1=$(ctxt_now)
	echo $((ctxt1 - ctxt0)) >"/tmp/ctxt-$name-$r.txt"
	return 0
}

for r in $(seq 1 "$REPS"); do
	say "第 $r/$REPS 轮"
	for g in $PHASES; do
		run_phase "$g" "$r" "/tmp/lres-$g-$r.json" || exit 1
		printf '   %-7s ai_p50=%-6s ai_miss=%-7s work_p50=%-7s probe_miss=%s\n' \
			"$g" \
			"$(jget "/tmp/lres-$g-$r.json" sched_p50_us)" \
			"$(jget "/tmp/lres-$g-$r.json" sla_miss)" \
			"$(jget "/tmp/lres-$g-$r.json" work_p50_us)" \
			"$(jget "/tmp/probe-$g-$r.json" sla_miss)"
	done
done

say "一、AI 引擎：调度延迟与超时比例（微秒，各轮中位数）"
printf '%-8s %8s %8s %8s %16s\n' "组" "p50" "p90" "max" "超过${SLA_US}us"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %16s\n' "$g" \
		"$(col "$g" "/tmp/lres-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/lres-%s-%s.json" sched_p90_us)" \
		"$(col "$g" "/tmp/lres-%s-%s.json" sched_max_us)" \
		"$(miss "$g" "/tmp/lres-%s-%s.json")"
done

say "二、普通任务（旁观探针）：调度延迟与超时比例（微秒）"
printf '%-8s %8s %8s %8s %16s\n' "组" "p50" "p90" "max" "超过${PSLA_US}us"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %16s\n' "$g" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p90_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_max_us)" \
		"$(miss "$g" "/tmp/probe-%s-%s.json")"
done

say "三、算力对照：每请求的计算耗时（微秒，五组应接近）"
printf '%-8s %10s %10s\n' "组" "work_p50" "work_p99"
for g in $PHASES; do
	printf '%-8s %10s %10s\n' "$g" \
		"$(col "$g" "/tmp/lres-%s-%s.json" work_p50_us)" \
		"$(col "$g" "/tmp/lres-%s-%s.json" work_p99_us)"
done

say "四、压测吞吐与调度开销"
printf '%-8s %12s %10s %8s %6s\n' "组" "iters/秒" "相对CFS" "ctxt" "pol"
RATE_RE='s/.*rate=\([0-9]*\)\/s.*/\1/p'
base=$(grepc cfs "/tmp/lg-%s-%s.log" "$RATE_RE")
for g in $PHASES; do
	rate=$(grepc "$g" "/tmp/lg-%s-%s.log" "$RATE_RE")
	rel="-"
	if [ -n "$rate" ] && [ -n "$base" ] && [ "$base" -gt 0 ]; then
		rel="$((rate * 100 / base))%"
	fi
	printf '%-8s %12s %10s %8s %6s\n' "$g" "$rate" "$rel" \
		"$(grepc "$g" "/tmp/ctxt-%s-%s.txt" \
			's/^[^0-9]*\([0-9][0-9]*\).*/\1/p')" \
		"$(awk '{print $1}' "/tmp/pol-$g-1.txt" 2>/dev/null)"
done

echo
note "一、二两张表必须一起看：AI 快了多少，代价写在第二张表里。"
note "超时比例是各轮累加：分子是超阈值的请求数，分母是总请求数。"
note "pol：0=SCHED_OTHER 1=SCHED_FIFO，用来确认 nice/chrt 真的生效。"
