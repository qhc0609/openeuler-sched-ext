# robot-brain/linux —— 1.2 的代码：一个驱动 + 两个用户态程序
#
# 驱动走内核构建系统，用户态走普通 gcc。两者分开编，互不影响。

CC     ?= cc
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra
LDLIBS := -lm -lpthread
KDIR   ?= /lib/modules/$(shell uname -r)/build

BINS := bin/first-bot bin/syscall-cost

all: bin/first-bot bin/syscall-cost module

bin:
	mkdir -p bin

bin/first-bot: first-bot.c motor_abi.h | bin
	$(CC) $(CFLAGS) -o $@ first-bot.c $(LDLIBS)

bin/syscall-cost: syscall-cost.c motor_abi.h | bin
	$(CC) $(CFLAGS) -o $@ syscall-cost.c

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	rm -rf bin
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean module
