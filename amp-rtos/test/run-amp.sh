#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# run-amp.sh —— 把 Linux 侧和 RTOS 侧一起跑起来
#
# 用法：./test/run-amp.sh [消息条数]
#   MODE=isolated   RTOS 绑到 CPU3 并给 SCHED_FIFO（默认）
#   MODE=shared     RTOS 不绑核，和 Linux 抢（对照组）
#
# 启动顺序：先 Linux，后 RTOS。
#   host 先起来，初始化控制块、把 virtio status 推到 DRIVER_OK；
#   RTOS 后起来，读到 DRIVER_OK 才继续建队列和端点。
# 反过来也行，但 RTOS 会先卡在等 DRIVER_OK 上，日志不好读。

set -u
cd "$(dirname "$0")/.."

N=${1:-2000}
RTOS_CPU=${RTOS_CPU:-3}
HOST_CPUS=${HOST_CPUS:-0-2}
MODE=${MODE:-isolated}
RUNTIME=${RUNTIME:-4}

mkdir -p results

if [ ! -x bin/amp_host ] || [ ! -x bin/amp_rtos ] || [ ! -e kernel/amp_shm.ko ]; then
	echo "先编译"
	make -s all || exit 1
fi

# 每轮都重载模块。模块里的共享内存和门铃状态是全局的，上一轮如果被 kill
# 掉，残留的对侧指针会让第一脚门铃投到空处，现象是"两边都起来了但没消息"。
if lsmod | grep -q '^amp_shm'; then
	rmmod amp_shm || exit 1
fi
insmod kernel/amp_shm.ko || exit 1
sleep 0.2

# Linux 侧先起，它负责初始化共享内存和 virtio 状态
taskset -c "$HOST_CPUS" ./bin/amp_host "$N" \
	> "results/host-$MODE.log" 2>&1 &
HOST_PID=$!
sleep 0.5

if ! kill -0 "$HOST_PID" 2>/dev/null; then
	echo "Linux 侧启动就退出了："
	cat "results/host-$MODE.log"
	exit 1
fi

# RTOS 侧
if [ "$MODE" = "isolated" ]; then
	taskset -c "$RTOS_CPU" chrt -f 50 ./bin/amp_rtos \
		> "results/rtos-$MODE.log" 2>&1 &
else
	./bin/amp_rtos > "results/rtos-$MODE.log" 2>&1 &
fi
RTOS_PID=$!

wait "$HOST_PID"
RC=$?

sleep "$RUNTIME"
kill "$RTOS_PID" 2>/dev/null
wait "$RTOS_PID" 2>/dev/null

echo "########## Linux 侧 ##########"
cat "results/host-$MODE.log"
echo
echo "########## RTOS 侧 ##########"
cat "results/rtos-$MODE.log"

exit $RC
