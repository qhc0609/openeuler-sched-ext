#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-sla.sh — 五组对照：既要快，又不许霸占
#
# 和 run-long.sh 的区别只有一个：多采一个量 —— 单次连续占核时长。
#
#   调度延迟 = 该跑 -> 真跑      引擎自己的体感，越小越好
#   占核时长 = 真跑 -> 被换下    同核上别人的体感，越小越好
#
# 这两个量是互相拉扯的。把引擎的优先级往上推，调度延迟降下来，
# 占核时长就涨上去：优先级越高，内核越不愿意把引擎换下去。
# 五组对照就是看谁能同时压住两个。
#
# 组：
#   cfs    什么都不做
#   nice   nice -n -20
#   fifo   chrt -f 50
#   scx20  ai_prio 切片 20ms，不限连续片数
#   scxbw  ai_prio 切片  1ms，占满 1 片让一次，唤醒插队头并踢核
#
# 占核时长怎么量：ftrace 的 sched_switch，只过滤引擎 pid。
# 引擎被换上来记 t_in，被换下去记 t_out，差值就是一次连续占核。
# 这个量没法从引擎自己的计时里拿到 —— 引擎只知道自己什么时候在算，
# 不知道内核什么时候把它换下去。
#
# 用法：
#   sudo ./run-sla.sh
#   REPS=5 ./run-sla.sh
#
# 环境变量：
#   REPS       每组轮数（默认 3）
#   THREADS    压测线程数（默认 2×核数）
#   REQUESTS   引擎请求数（默认 120）
#   INTERVAL   请求间隔毫秒（默认 150）
#   PASSES     每请求前向次数（默认 1300 ≈ 40 ms）
#   PROBE_REQ  探针请求数（默认 400）
#   PROBE_MS   探针间隔毫秒（默认 40）
#   SLA_US     引擎调度延迟阈值，微秒（默认 5000）
#   PSLA_US    探针调度延迟阈值，微秒（默认 10000）
#   DURATION   压测最长秒数（默认 60）

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
SCHED_DIR="$REPO/scheds"
TRACE=/sys/kernel/debug/tracing
cd "$HERE" || exit 1

NCPU="$(nproc)"
REPS="${REPS:-3}"
THREADS="${THREADS:-$((NCPU * 2))}"
REQUESTS="${REQUESTS:-120}"
INTERVAL="${INTERVAL:-150}"
PASSES="${PASSES:-1300}"
PROBE_REQ="${PROBE_REQ:-400}"
PROBE_MS="${PROBE_MS:-40}"
SLA_US="${SLA_US:-5000}"
PSLA_US="${PSLA_US:-10000}"
DURATION="${DURATION:-60}"

# 不要叫 GROUPS：bash 的保留数组，赋值会被静默忽略
PHASES="cfs nice fifo scx20 scxbw"

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

# miss <组> <模板>：各轮 sla_miss 与 requests 分别累加，输出 "超时/总数 百分比"
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

# 占核时长的第 N 百分位：各轮分别取分位数，再取中位数
hold() {
	local g="$1" p="$2" r
	for r in $(seq 1 "$REPS"); do
		sort -n "/tmp/hold-$g-$r.txt" 2>/dev/null |
			awk -v p="$p" '
				{ a[NR] = $1 }
				END {
					if (!NR) exit
					i = int(NR * p / 100) + 1
					if (i > NR) i = NR
					print a[i]
				}'
	done | median
}

# 占核超过阈值的次数 / 总次数
hold_miss() {
	local g="$1" lim="$2" r n s=0 t=0
	for r in $(seq 1 "$REPS"); do
		n=$(wc -l <"/tmp/hold-$g-$r.txt" 2>/dev/null || echo 0)
		s=$((s + $(awk -v l="$lim" '$1 > l' "/tmp/hold-$g-$r.txt" 2>/dev/null | wc -l)))
		t=$((t + n))
	done
	if [ "$t" -le 0 ]; then echo "-"; return; fi
	awk -v s="$s" -v t="$t" 'BEGIN { printf "%d/%d %.1f%%", s, t, s * 100.0 / t }'
}

# 各轮合起来取最大值
hold_max() {
	local g="$1" r
	for r in $(seq 1 "$REPS"); do
		sort -n "/tmp/hold-$g-$r.txt" 2>/dev/null | tail -1
	done | sort -n | tail -1
}

ctxt_now() { awk '/^ctxt/ {print $2}' /proc/stat; }

# 从 trace 里算出引擎每次连续占核的微秒数，写进 /tmp/hold-<组>-<轮>.txt
hold_dump() {
	local pid="$1" out="$2"
	awk -v pid="$pid" '
		/sched_switch:/ {
			ts = ""
			for (i = 1; i <= NF; i++)
				if ($i ~ /^[0-9]+\.[0-9]+:$/) { ts = $i; sub(":", "", ts); break }
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
	' "$TRACE/trace" >"$out"
}

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
if [ ! -d "$TRACE/events/sched/sched_switch" ]; then
	echo "!! $TRACE 不可用：挂上 debugfs 或者换一台有 ftrace 的机器" >&2
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
	local pre="" sargs="" ctxt0 ctxt1

	rm -f /tmp/ai.go /tmp/ai.pid
	ctxt0=$(ctxt_now)

	./loadgen "$THREADS" "$DURATION" >/dev/null 2>"/tmp/lg-$name-$r.log" &
	lg_pid=$!
	sleep 0.5

	# 旁观探针：普通任务，每次只算 32 微秒，每 40 毫秒醒一次
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
	scx20)
		sargs="--kick 1 --burst 0 --wake-boost 0 --ai-slice-us 20000 --other-slice-us 3000"
		;;
	scxbw)
		sargs="--kick 1 --burst 1 --wake-boost 1 --ai-slice-us 1000 --other-slice-us 3000"
		;;
	esac

	if [ -n "$sargs" ]; then
		# shellcheck disable=SC2086
		$SUDO "$SCHED_DIR/ai_prio" "$SCHED_DIR/ai_prio.bpf.o" "$target" $sargs \
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
	fi

	# 只在这个引擎身上开 trace：filter 用 prev/next 两个字段，
	# 只写 prev_pid 会漏掉"被换上来"的那一半。
	# NOTRACE=1 用来做对照：确认 trace 本身有没有把延迟数据带偏。
	if [ "${NOTRACE:-0}" = 1 ]; then
		touch /tmp/ai.go
		wait "$eng_pid" 2>/dev/null
		: > "/tmp/hold-$name-$r.txt"
	else
		echo 0 > "$TRACE/tracing_on"
		echo > "$TRACE/trace"
		echo 16384 > "$TRACE/buffer_size_kb"
		echo "prev_pid == $target || next_pid == $target" \
			> "$TRACE/events/sched/sched_switch/filter"
		echo 1 > "$TRACE/events/sched/sched_switch/enable"
		echo 1 > "$TRACE/tracing_on"

		touch /tmp/ai.go
		wait "$eng_pid" 2>/dev/null

		echo 0 > "$TRACE/tracing_on"
		echo 0 > "$TRACE/events/sched/sched_switch/enable"
		hold_dump "$target" "/tmp/hold-$name-$r.txt"
		echo 0 > "$TRACE/events/sched/sched_switch/filter"
	fi

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
		run_phase "$g" "$r" "/tmp/sres-$g-$r.json" || exit 1
		printf '   %-7s ai_p50=%-6s ai_miss=%-7s hold_p90=%-7s probe_miss=%s\n' \
			"$g" \
			"$(jget "/tmp/sres-$g-$r.json" sched_p50_us)" \
			"$(jget "/tmp/sres-$g-$r.json" sla_miss)" \
			"$(hold "$g" 90)" \
			"$(jget "/tmp/probe-$g-$r.json" sla_miss)"
	done
done

say "一、引擎侧：调度延迟（微秒，各轮中位数）"
printf '%-8s %8s %8s %8s %16s\n' "组" "p50" "p90" "max" "超过${SLA_US}us"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %16s\n' "$g" \
		"$(col "$g" "/tmp/sres-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/sres-%s-%s.json" sched_p90_us)" \
		"$(col "$g" "/tmp/sres-%s-%s.json" sched_max_us)" \
		"$(miss "$g" "/tmp/sres-%s-%s.json")"
done

say "二、引擎侧：单次连续占核时长（微秒，ftrace sched_switch）"
printf '%-8s %8s %8s %8s %8s %16s\n' "组" "p50" "p90" "p99" "max" "超过 5000us"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %8s %16s\n' "$g" \
		"$(hold "$g" 50)" \
		"$(hold "$g" 90)" \
		"$(hold "$g" 99)" \
		"$(hold_max "$g")" \
		"$(hold_miss "$g" 5000)"
done

say "三、系统侧：旁观探针（微秒）"
printf '%-8s %8s %8s %8s %16s\n' "组" "p50" "p90" "max" "超过${PSLA_US}us"
for g in $PHASES; do
	printf '%-8s %8s %8s %8s %16s\n' "$g" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p50_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_p90_us)" \
		"$(col "$g" "/tmp/probe-%s-%s.json" sched_max_us)" \
		"$(miss "$g" "/tmp/probe-%s-%s.json")"
done

say "四、吞吐与调度开销"
printf '%-8s %12s %10s %8s %6s %10s\n' "组" "iters/秒" "相对CFS" "ctxt" "pol" "work_p50"
RATE_RE='s/.*rate=\([0-9]*\)\/s.*/\1/p'
base=$(grepc cfs "/tmp/lg-%s-%s.log" "$RATE_RE")
for g in $PHASES; do
	rate=$(grepc "$g" "/tmp/lg-%s-%s.log" "$RATE_RE")
	rel="-"
	if [ -n "$rate" ] && [ -n "$base" ] && [ "$base" -gt 0 ]; then
		rel="$((rate * 100 / base))%"
	fi
	printf '%-8s %12s %10s %8s %6s %10s\n' "$g" "$rate" "$rel" \
		"$(grepc "$g" "/tmp/ctxt-%s-%s.txt" \
			's/^[^0-9]*\([0-9][0-9]*\).*/\1/p')" \
		"$(awk '{print $1}' "/tmp/pol-$g-1.txt" 2>/dev/null)" \
		"$(col "$g" "/tmp/sres-%s-%s.json" work_p50_us)"
done

say "五、应用自己动手会怎样（非 root，RLIMIT_NICE=0）"
note "容器里的默认状态：没有 CAP_SYS_NICE，RLIMIT_NICE 是 0"
# nice 设不上时不会报错退出，它照旧把命令跑起来，只是优先级没变。
# 所以要同时看两样：stderr 那句话，和进程自己的 nice 值 / policy。
# /proc/self/stat 第 19 字段是 nice，第 41 字段是 policy（0=OTHER 1=FIFO）。
if id nobody >/dev/null 2>&1; then
	r_nice=$(nice -n -20 cat /proc/self/stat 2>/dev/null | awk '{print $19}')
	r_pol=$(chrt -f 50 cat /proc/self/stat 2>/dev/null | awk '{print $41}')
	n_nice=$(su -s /bin/sh nobody -c 'ulimit -e 0; nice -n -20 cat /proc/self/stat' \
		2>/dev/null | awk '{print $19}')
	n_msg=$(su -s /bin/sh nobody -c 'ulimit -e 0; nice -n -20 true' 2>&1 | tr '\n' ' ')
	c_msg=$(su -s /bin/sh nobody -c 'ulimit -e 0; chrt -f 50 true' 2>&1 | tr '\n' ' ')
	note "root    nice -n -20   -> nice=${r_nice:-?}"
	note "root    chrt -f 50    -> policy=${r_pol:-?}"
	note "nobody  nice -n -20   -> nice=${n_nice:-?}   ${n_msg}"
	note "nobody  chrt -f 50    -> ${c_msg}"
else
	note "没有 nobody 用户，跳过"
fi

echo
note "一、二两张表必须一起看：延迟降了多少，占核涨了多少。"
note "占核时长来自 ftrace，只统计引擎自己的 switch_in -> switch_out。"
note "pol：0=SCHED_OTHER 1=SCHED_FIFO，用来确认 nice/chrt 真的生效。"
