#!/bin/bash
# 第 5 篇：用 perf 观察实际执行。
#
# 这台虚拟机的 PMU 没被透传，cycles / instructions / branches 全部返回
# <not supported>，所以这里分成两段：
#   一、把"硬件计数器不可用"这件事本身记录下来
#   二、改用软件事件 task-clock 做采样，靠指令地址归属找热点
#
# 用法：bash test/run-perf.sh [绑定的核号，默认 2]
set -u

cd "$(dirname "$0")/.." || exit 1

CPU=${1:-2}
OUT=results
BIN=bin

mkdir -p "$OUT"

make -s || { echo "编译失败"; exit 1; }

{
    echo "== 一、硬件计数器：逐个试 =="
    for e in cycles instructions branches branch-misses cache-misses; do
        printf '%-16s ' "$e"
        perf stat -e "$e" -- /bin/true 2>&1 | grep -qE '<not supported>' &&
            echo "不可用" || echo "可用"
    done
    echo
    echo "perf_event_paranoid = $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 读不到)"
    echo
    echo "== 二、软件事件：这些是可用的 =="
    # 只留六行：task-clock、context-switches、page-faults、elapsed、user、sys。
    # 用 tail -8 会把 task-clock 那行切掉，文章里就引不到"用了多少 CPU"了。
    taskset -c "$CPU" perf stat -e task-clock,context-switches,page-faults \
        -- "$BIN/hotloop" 1500 2>&1 | grep -vE '^[[:space:]]*$' | tail -6
} > "$OUT/06-perf-stat.txt" 2>&1
echo "-> $OUT/06-perf-stat.txt"

{
    echo "== 三、采样做热点分析 =="
    echo "被观察的程序：三段，heavy 约占 70%、branchy 约 20%、light 约 10%"
    echo

    rm -f perf.data
    taskset -c "$CPU" perf record -e task-clock -g -o perf.data \
        -- "$BIN/hotloop" 3000 2>&1 | tail -3
    echo

    echo "--- 按函数汇总 ---"
    perf report --stdio -i perf.data --no-children --percent-limit 1 2>&1 |
        grep -vE '^#|^$' | grep -vE '^\s*\|' | grep -vE '^\s*---' | head -10

    echo
    echo "--- 进到 phase_heavy 里，按指令归属 ---"
    perf annotate -s phase_heavy -i perf.data --stdio 2>&1 |
        grep -E '^[[:space:]]*[0-9]+\.[0-9]+' | sort -rn | head -6
} > "$OUT/07-perf-record.txt" 2>&1
echo "-> $OUT/07-perf-record.txt"

{
    echo "== 四、反汇编：热点函数到底在做什么 =="
    objdump -d --no-show-raw-insn "$BIN/hotloop" |
        sed -n '/<phase_heavy>:/,/ret/p'
} > "$OUT/08-objdump.txt" 2>&1
echo "-> $OUT/08-objdump.txt"

rm -f perf.data
echo "完成。"
