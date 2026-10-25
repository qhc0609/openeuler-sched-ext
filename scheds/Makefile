# SPDX-License-Identifier: GPL-2.0
#
# 构建最小 sched_ext 调度器。
#
#   make            编译出 ./minimal 和 ./minimal.bpf.o
#   make vmlinux.h  只重新生成 BTF 头（换内核后需要）
#   make check      检查依赖是否齐备
#   make clean      清理产物
#
# 依赖（openEuler）：
#   sudo dnf install -y clang llvm libbpf-devel bpftool
#
# 另外需要内核源码树里的 sched_ext 头文件。
# 默认路径按当前内核版本推出来：
#   ~/src/linux-<主>.<次>/tools/sched_ext/include
# 路径不一样就显式指定：
#   make SCX_INCLUDE=/path/to/linux-6.12/tools/sched_ext/include

CLANG    ?= clang
CC       ?= cc
BPFTOOL  ?= bpftool
ARCH     := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
KREL_MM  := $(shell uname -r | cut -d. -f1-2)

# sched_ext 的 BPF 侧头文件来自内核源码树，不来自 libbpf。
# libbpf 1.2 的 bpf_tracing.h 里没有 BPF_STRUCT_OPS 宏，
# 而 SCX_OPS_DEFINE、kfunc 声明也都在那边。
SCX_INCLUDE ?= $(HOME)/src/linux-$(KREL_MM)/tools/sched_ext/include

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -Wall
CFLAGS     := -g -O2 -Wall

# 两个产物都要，但彼此不链接：
#   minimal.bpf.o 是 BPF 字节码，运行时被读取
#   minimal       是用户态加载器
all: minimal minimal.bpf.o ai_prio ai_prio.bpf.o

# vmlinux.h 从当前运行内核的 BTF 导出，不要手工编辑。
# 换内核（比如 6.6 → 6.12）后必须删掉重新生成，
# 否则结构体布局对不上：编译能过，加载会失败。
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# -g 是必须的：libbpf 靠 BTF 校验 struct_ops 的函数签名。
# 去掉 -g 就没有 BTF，加载时类型检查必然失败。
# -I. 让 scx/common.bpf.h 里的 #include "vmlinux.h" 找到本目录这份。
minimal.bpf.o: minimal.bpf.c vmlinux.h
	@test -d "$(SCX_INCLUDE)" || { \
		echo "找不到 $(SCX_INCLUDE)"; \
		echo "指定内核源码树路径：make SCX_INCLUDE=/path/to/linux-6.12/tools/sched_ext/include"; \
		exit 1; }
	$(CLANG) $(BPF_CFLAGS) -I. -I$(SCX_INCLUDE) -c $< -o $@

# 用户态加载器，链接 libbpf。
#
# 注意：minimal.bpf.o 不能参与链接。
# 它是 BPF 字节码（ELF 里的 e_machine = 247 = EM_BPF），
# 交给系统链接器会报 "Relocations in generic ELF (EM: 247)" 然后失败。
# 它是运行时被 bpf_object__open_file() 读进去的，不是编译期链进来的。
minimal: minimal.c
	$(CC) $(CFLAGS) -o $@ minimal.c -lbpf -lelf -lz

# 优先调度器：多一个 cfg map 和一条踢核路径，构建方式完全一样。
ai_prio.bpf.o: ai_prio.bpf.c vmlinux.h
	@test -d "$(SCX_INCLUDE)" || { \
		echo "找不到 $(SCX_INCLUDE)"; \
		echo "指定内核源码树路径：make SCX_INCLUDE=/path/to/linux-6.12/tools/sched_ext/include"; \
		exit 1; }
	$(CLANG) $(BPF_CFLAGS) -I. -I$(SCX_INCLUDE) -c $< -o $@

ai_prio: ai_prio.c
	$(CC) $(CFLAGS) -o $@ ai_prio.c -lbpf -lelf -lz

check:
	@echo "内核版本      : $(KREL_MM)"
	@echo "sched_ext 头  : $(SCX_INCLUDE)"
	@test -d "$(SCX_INCLUDE)" && echo "  -> 存在" || echo "  -> 缺失"
	@echo -n "clang         : "; $(CLANG) --version | head -1
	@echo -n "bpftool       : "; $(BPFTOOL) version | head -1
	@test -e /sys/kernel/sched_ext && echo "sched_ext     : 已启用" || echo "sched_ext     : 未启用"

clean:
	rm -f minimal minimal.bpf.o ai_prio ai_prio.bpf.o vmlinux.h

.PHONY: all check clean
