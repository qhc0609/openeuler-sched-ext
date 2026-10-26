# SPDX-License-Identifier: GPL-2.0
#
# 构建推理引擎、压测工具和睡眠探针。
#
#   make          编译 tiny_infer、loadgen、sleep-over
#   make clean    清理

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra
LDFLAGS :=

all: tiny_infer loadgen sleep-over

tiny_infer: tiny_infer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

loadgen: loadgen.c
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDFLAGS)

# 只量一件事：绝对睡眠醒来比目标晚多少微秒。
# 用来证明「空载量到的调度延迟是虚拟化成本，不是调度成本」。
sleep-over: sleep-over.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f tiny_infer loadgen sleep-over

.PHONY: all clean
