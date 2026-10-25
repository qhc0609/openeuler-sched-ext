# SPDX-License-Identifier: GPL-2.0
#
# 构建推理引擎和压测工具。
#
#   make          编译 tiny_infer 和 loadgen
#   make clean    清理

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra
LDFLAGS :=

all: tiny_infer loadgen

tiny_infer: tiny_infer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

loadgen: loadgen.c
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDFLAGS)

clean:
	rm -f tiny_infer loadgen

.PHONY: all clean
