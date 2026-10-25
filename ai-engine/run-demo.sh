#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-demo.sh — 五组对照：不同调度手段下，AI 引擎和整个系统各自付出什么
#
# 五组：
#   cfs     CFS 默认           什么都不做，基线
#   nice    nice -n -20        CFS 内部给高权重，一条 nice 命令
#   fifo    chrt -f 50         SCHED_FIFO 实时类，一条 chrt 命令
#   nokick  ai_prio --kick 0   自定义调度器，只分道不抢占
#   kick    ai_prio --kick 1   自定义调度器，分道 + 唤醒抢占
#
# 为什么不是只比 cfs 和 kick：
#   那样只能证明"自定义调度器比默认好"，证明不了"它值得写"。
#   内核里现成的 nice 和 SCHED_FIFO 也是解法，而且各只要一条命令。
#   把它们放进同一张表，才能回答"到底该用哪个"。
#
# 每组同时采四个量：
#   AI 引擎延迟      —— 要优化的目标
#   探针延迟         —— 普通任务被拖慢了多少（系统侧）
#   压测吞吐         —— 被抢走的算力有多少（系统侧）
#   上下文切换次数    —— 调度本身的开销
#
# 用法：
#   sudo ./run-demo.sh
#   REPS=5 REQUESTS=1200 ./run-demo.sh
#
# 环境变量：
#   REPS        每组重复轮数（默认 3）
#   THREADS     压测线程数（默认 2×核数）
#   REQUESTS    引擎每轮请求数（默认 800）
#   INTERVAL    引擎请求间隔毫秒（默认 10）
#   PROBE_REQ   探针请求数（默认 200）
#   PROBE_MS    探针间隔毫秒（默认 40）
#   DURATION    压测最长秒数（默认 120，引擎跑完就 kill）

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
SCHED_DIR="$REPO/scheds"
cd "$HERE" || exit 1

NCPU="$(nproc)"
REPS="${REPS:-3}"
THREADS="${THREADS:-$((NCPU * 2))}"
REQUESTS="${REQUESTS:-800}"
INTERVAL="${INTERVAL:-10}"
PROBE_REQ="${PROBE_REQ:-200}"
PROBE_MS="${PROBE_MS:-40}"
DURATION="${DURATION:-120}"

# 不要叫 GROUPS：那是 bash 的保留数组（当前用户的组 ID），赋值会被忽略，
# 展开成 gid。踩过一次，五组变成一组，名字还叫 "0"。
PHASES="cfs nice fifo nokick kick"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

say()  { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }

# jget <文件> <字段>：从一行 JSON 里抠出某个整数字段。
# 不引 jq —— 目标机器上不一定装了，这个脚本要能直接跑。
jget() {
	grep -o "\"$2\":[0-9]*" "$1" 2>/dev/null |
		head -1 | cut -d: -f2
}

# median：读一列数字，输出中位数。偶数个取中间两个的平均。
median() {
	sort -n | awk '
		{ a[NR] = $1 }
		END {
			if (NR == 0) { print "-"; exit }
			if (NR % 2) print a[(NR + 1) / 2]
			else printf "%.0f\n", (a[NR / 2] + a[NR / 2 + 1]) / 2
		}'
}

# col <组> <文件模板> <字段>：该组各轮某个 JSON 字段的中位数
col() {
	local g="$1" tpl="$2" k="$3" r
	for r in $(seq 1 "$REPS"); do
		jget "$(printf "$tpl" "$g" "$r")" "$k"
	done | median
}

# grepc <组> <文件模板> <sed 表达式>：该组各轮某个文本文件里一个数字的中位数
# 表达式要自己锚定，别写 .*[0-9]* —— 贪婪匹配会只留下最后一位
grepc() {
	local g="$1" tpl="$2" expr="$3" r
	for r in $(seq 1 "$REPS"); do
		sed -n "$expr" "$(printf "$tpl" "$g" "$r")" | head -1
	done | median
}

ctxt_now() { awk '/^ctxt/ {print $2}' /proc/stat; }

say "环境"
note "内核      $(uname -r)"
note "核数      $NCPU"
note "压测线程  $THREADS（= $((THREADS / NCPU)) 倍核数）"
note "引擎      $REQUESTS 个请求，间隔 ${INTERVAL}ms"
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
	local pre="" ctxt0 ctxt1

	rm -f /tmp/ai.go /tmp/ai.pid
	ctxt0=$(ctxt_now)

	./loadgen "$THREADS" "$DURATION" >/dev/null 2>"/tmp/lg-$name-$r.log" &
	lg_pid=$!
	sleep 0.5

	# 旁观探针：不交给任何调度器，就是一个普通任务。
	# 它测出来的延迟，等于"别的交互式任务在这个策略下有多难受"。
	./tiny_infer --requests "$PROBE_REQ" --interval-ms "$PROBE_MS" \
		--passes 1 --warmup 5 --wait /tmp/ai.go \
		--json >"/tmp/probe-$name-$r.json" &
	probe_pid=$!

	[ "$name" = nice ] && pre="nice -n -20"
	[ "$name" = fifo ] && pre="chrt -f 50"
	# shellcheck disable=SC2086
	$pre ./tiny_infer --requests "$REQUESTS" --interval-ms "$INTERVAL" \
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

	# 记下引擎真实的调度策略和 nice 值，用来证明 nice/chrt 真的生效了
	# /proc/<pid>/stat 第 41 字段是 policy（0=OTHER 1=FIFO 2=RR），第 19 是 nice
	{
		awk '{print $41}' "/proc/$target/stat" 2>/dev/null
		awk '{print $19}' "/proc/$target/stat" 2>/dev/null
	} | paste -sd' ' - >"/tmp/pol-$name-$r.txt"

	case "$name" in
	nokick|kick)
		local karg=0
		[ "$name" = kick ] && karg=1
		$SUDO "$SCHED_DIR/ai_prio" "$SCHED_DIR/ai_prio.bpf.o" "$target" \
			--kick "$karg" --quiet >"/tmp/sched-$name-$r.log" 2>&1 &
		sched_pid=$!
		# 等 sysfs 里真的挂上，而不是等进程起来
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
	kill "$lg_pid" 2>/dev/null     # SIGTERM：loadgen 收到后会打印吞吐
	wait "$lg_pid" 2>/dev/null

	ctxt1=$(ctxt_now)
	echo $((ctxt1 - ctxt0)) >"/tmp/ctxt-$name-$r.txt"
	return 0
}

for r in $(seq 1 "$REPS"); do
	say "第 $r/$REPS 轮"
	for g in $PHASES; do
		run_phase "$g" "$r" "/tmp/res-$g-$r.json" || exit 1
		printf '   %-7s ai_p50=%-6s ai_p99=%-8s rate=%s\n' \
			"$g" \
			"$(jget "/tmp/res-$g-$r.json" sched_p50_us)" \
			"$(jget "/tmp/res-$g-$r.json" sched_p99_us)" \
			"$(sed -n 's/.*rate=\([0-9]*\)\/s.*/\1/p' "/tmp/lg-$g-$r.log")"
	done
done

say "一、AI 引擎调度延迟（微秒，各轮中位数）"
printf '%-8s %8s %8s %8s %9s\n' "组" "p50" "p90" "p99" "max"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %9s\n' "$g" \
		"$(col "$g" "/tmp/res-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/res-%s-%s.json" sched_p90_us)" \
		"$(col "$g" "/tmp/res-%s-%s.json" sched_p99_us)" \
		"$(col "$g" "/tmp/res-%s-%s.json" sched_max_us)"
done

say "二、普通任务（旁观探针）调度延迟（微秒）"
printf '%-8s %8s %8s %8s\n' "组" "p50" "p99" "max"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s\n' "$g" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p99_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_max_us)"
done

say "三、压测吞吐与调度开销"
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

say "四、算力对照（三组 work_p50 应接近，否则测的是算得快）"
for g in $PHASES; do
	printf '   %-8s %s us\n' "$g" \
		"$(col "$g" "/tmp/res-%s-%s.json" work_p50_us)"
done

echo
note "一、二两张表要一起看：AI 变快的代价，写在第二张表里。"
note "rate 相对 CFS 的比例，就是被拿走的算力。"
note "ctxt 是每轮的上下文切换增量，包含压测、引擎、探针三方。"
note "pol：0=SCHED_OTHER 1=SCHED_FIFO，用来确认 nice/chrt 真的生效。"
