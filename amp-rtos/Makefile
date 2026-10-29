# SPDX-License-Identifier: BSD-3-Clause
#
# amp-rtos —— 在单台 PC 上跑 Linux + RTOS 的 AMP
#
# 两个目标：
#   bin/amp_host   Linux 侧，链接上游 open-amp
#   bin/amp_rtos   RTOS 侧，FreeRTOS POSIX 端口 + 上游 open-amp
#
# 两边用的是同一份 common/amp_transport.c —— 就像 virtio-mmio 的代码
# 两侧共用一样，区别只在角色和 notify 的实现。

CC       ?= gcc
OPENAMP  ?= /root/amp/open-amp
FREERTOS ?= /root/amp/FreeRTOS-Kernel

INC := -Iinclude -Icommon -I$(OPENAMP)/lib/include -I/usr/local/include

OPENAMP_SRCS := \
	$(OPENAMP)/lib/rpmsg/rpmsg.c \
	$(OPENAMP)/lib/rpmsg/rpmsg_virtio.c \
	$(OPENAMP)/lib/virtio/virtio.c \
	$(OPENAMP)/lib/virtio/virtqueue.c \
	$(OPENAMP)/lib/utils/utilities.c

FREERTOS_SRCS := \
	$(FREERTOS)/tasks.c \
	$(FREERTOS)/queue.c \
	$(FREERTOS)/list.c \
	$(FREERTOS)/timers.c \
	$(FREERTOS)/portable/MemMang/heap_4.c \
	$(FREERTOS)/portable/ThirdParty/GCC/Posix/port.c \
	$(FREERTOS)/portable/ThirdParty/GCC/Posix/utils/wait_for_event.c

FREERTOS_INC := -Irtos -I$(FREERTOS)/include \
		-I$(FREERTOS)/portable/ThirdParty/GCC/Posix

CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
	    -DVQ_RX_EMPTY_NOTIFY=0 -DVIRTIO_DRIVER_SUPPORT=1 \
	    -DVIRTIO_DEVICE_SUPPORT=1 $(INC)
# libmetal 用源码编的那份（版本和 open-amp 对得上），静态链接省掉 LD_LIBRARY_PATH
LDLIBS   := /usr/local/lib/libmetal.a -lsysfs -lpthread -lrt -lm

KDIR ?= /lib/modules/$(shell uname -r)/build

KVER    ?= $(shell uname -r)
MODDIR   = /lib/modules/$(KVER)/extra
MODCONF  = /etc/modules-load.d/amp-shm.conf

.PHONY: all clean host rtos tools module load unload install uninstall \
	patch-rtos-kernel

all: module host rtos tools

host: bin/amp_host
rtos: patch-rtos-kernel bin/amp_rtos
tools: bin/tick_floor bin/amp_shm_test

# FreeRTOS 官方 POSIX 端口的 tick 用 usleep 相对睡眠，会累积漂移
# （这台机器上 1 kHz 实际只有 ~700 Hz）。打一个单点补丁改成绝对时刻。
patch-rtos-kernel:
	@python3 rtos/patch-freertos-tick.py $(FREERTOS)

bin/amp_host: host/amp_host.c common/amp_transport.c $(OPENAMP_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

bin/amp_rtos: rtos/main.c common/amp_transport.c \
	      $(OPENAMP_SRCS) $(FREERTOS_SRCS) | bin
	$(CC) $(CFLAGS) $(FREERTOS_INC) -o $@ $^ $(LDLIBS)

# 两个小工具：一个是共享内存通道的自检，一个是 1 kHz 定时器的抖动下限
bin/tick_floor: test/tick-floor.c | bin
	$(CC) $(CFLAGS) -o $@ $< -lpthread

bin/amp_shm_test: test/amp_shm_test.c | bin
	$(CC) $(CFLAGS) -o $@ $<

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel AMP_INC=$(CURDIR)/include modules

load: module
	insmod kernel/amp_shm.ko

unload:
	-rmmod amp_shm

# 装成开机自动加载。
#
# 不能直接写个 .service 去 insmod /root 下的 ko：/root 的 SELinux 标签是
# user_home_t，init_t 加载不了，报 "Permission denied"。装到 /lib/modules
# 下面标签才是 modules_object_t。这一步 2026-10-29 在这台机器上踩过。
install: module
	install -D -m 644 kernel/amp_shm.ko $(MODDIR)/amp_shm.ko
	depmod -a $(KVER)
	-restorecon -v $(MODDIR)/amp_shm.ko
	install -D -m 644 systemd/amp-shm.conf $(MODCONF)
	@echo "已安装。重启后 /dev/amp0 会自动出现；不重启的话：modprobe amp_shm"

uninstall:
	-rmmod amp_shm
	-rm -f $(MODCONF) $(MODDIR)/amp_shm.ko
	-depmod -a $(KVER)

bin:
	mkdir -p bin

clean:
	rm -rf bin
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
