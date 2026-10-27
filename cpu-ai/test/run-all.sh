#!/bin/bash
# 依次跑五组演示，原始输出存到 results/，作为文章里每个数字的出处。
#
# 用法：bash test/run-all.sh [绑定的核号，默认 2]
set -u

cd "$(dirname "$0")/.." || exit 1

CPU=${1:-2}
OUT=results
BIN=bin

mkdir -p "$OUT"

{
    echo "== 环境 =="
    uname -srm
    grep -m1 'model name' /proc/cpuinfo
    echo "vCPU: $(nproc)"
    gcc --version | head -1
    clang --version | head -1
    qemu-aarch64-static --version | head -1
    echo "perf: $(perf --version 2>/dev/null || echo 未安装)"
    echo "绑定核: $CPU"
} > "$OUT/00-env.txt" 2>&1

echo "环境已记录到 $OUT/00-env.txt"

make -s || { echo "编译失败"; exit 1; }

run() {
    local name=$1
    shift
    echo "== $name =="
    taskset -c "$CPU" "$@" > "$OUT/$name.txt" 2>&1
    echo "  -> $OUT/$name.txt"
}

run 01-stage5     "$BIN/stage5"
run 02-dtype      "$BIN/dtype"
run 03-gemm       "$BIN/gemm"
run 04-bottleneck "$BIN/bottleneck"

echo "== 05-neon =="
{
    echo "== qemu-aarch64 下的数值校验（时间无意义，只看对不对） =="
    qemu-aarch64-static "$BIN/neon.aarch64"
    echo "退出码: $?"
    echo
    echo "== NEON 内层循环的指令 =="
    llvm-objdump -d --mattr=+neon "$BIN/neon.aarch64" |
        sed -n '/fmla/,+8p' | head -10
} > "$OUT/05-neon.txt" 2>&1
echo "  -> $OUT/05-neon.txt"

echo "全部完成。"
