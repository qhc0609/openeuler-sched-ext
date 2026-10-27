# CPU 如何执行 AI 计算 —— 五组演示程序
#
# 每个目标用各自的编译选项，因为"快多少"必须归因到具体某一个改动：
# 循环顺序、自动向量化、手写向量指令，三个因素要分开看。
#
# 用法：
#   make            全部编译
#   make check      编译并核对向量版和标量版算的是同一个数
#   make clean

CC      ?= gcc
CLANG   ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
BIN     := bin

# 这台机器是 Kaby Lake：AVX2 + FMA，没有 AVX-512，也没有 VNNI
SIMD    := -mavx2 -mfma

.PHONY: all check clean

all: $(BIN)/oneline $(BIN)/stage5 $(BIN)/dtype $(BIN)/gemm $(BIN)/bottleneck $(BIN)/hotloop $(BIN)/neon.aarch64

$(BIN):
	mkdir -p $(BIN)

# ---------------- 第 1 篇：五个阶段 ----------------

# 起点：三个函数，一个加法、一个求和、一个点积。
# 关掉向量化，让一行 C 对应一条指令。
$(BIN)/oneline: src/oneline.c common/bench.h | $(BIN)
	$(CC) $(CFLAGS) -fno-tree-vectorize -o $@ src/oneline.c

$(BIN)/stage5: src/stage5.c common/bench.h | $(BIN)
	$(CC) $(CFLAGS) -fno-tree-vectorize -o $@ src/stage5.c

# ---------------- 第 2 篇：数据类型 ----------------

$(BIN)/dtype: src/dtype.c common/bench.h | $(BIN)
	$(CC) $(CFLAGS) $(SIMD) -fno-tree-vectorize -o $@ src/dtype.c -lm

# ---------------- 第 3 篇：SIMD ----------------
# 四个版本分别编译，选项只差在"顺序"和"向量化"这两件事上

$(BIN)/gemm_ijk.o: src/gemm_ijk.c src/gemm.h | $(BIN)
	$(CC) $(CFLAGS) -fno-tree-vectorize -c -o $@ src/gemm_ijk.c

$(BIN)/gemm_ikj_scalar.o: src/gemm_ikj_scalar.c src/gemm_ikj_body.h src/gemm.h | $(BIN)
	$(CC) $(CFLAGS) -fno-tree-vectorize -c -o $@ src/gemm_ikj_scalar.c

$(BIN)/gemm_ikj_auto.o: src/gemm_ikj_auto.c src/gemm_ikj_body.h src/gemm.h | $(BIN)
	$(CC) -O3 -march=native -Wall -Wextra -std=gnu11 -c -o $@ src/gemm_ikj_auto.c

$(BIN)/gemm_avx2.o: src/gemm_avx2.c src/gemm.h | $(BIN)
	$(CC) $(CFLAGS) $(SIMD) -fno-tree-vectorize -c -o $@ src/gemm_avx2.c

$(BIN)/gemm_avx2_ikj.o: src/gemm_avx2_ikj.c src/gemm.h | $(BIN)
	$(CC) $(CFLAGS) $(SIMD) -fno-tree-vectorize -c -o $@ src/gemm_avx2_ikj.c

$(BIN)/gemm_main.o: src/gemm_main.c src/gemm.h common/bench.h | $(BIN)
	$(CC) $(CFLAGS) -c -o $@ src/gemm_main.c

$(BIN)/gemm: $(BIN)/gemm_main.o $(BIN)/gemm_ijk.o $(BIN)/gemm_ikj_scalar.o \
             $(BIN)/gemm_ikj_auto.o $(BIN)/gemm_avx2.o $(BIN)/gemm_avx2_ikj.o
	$(CC) -o $@ $^ -lm

# ARM64 版：交叉编译成静态裸程序，用 qemu 跑数值校验。
# 只验证"算得对"，不验证"跑得快"——qemu 是逐条翻译，时间没有意义。
#
# -fuse-ld=lld 不能省：x86 上的 GNU ld 只会链 x86，遇到 aarch64 目标直接
# 报"无法辨认的仿真模式"。lld 是 LLVM 自带的多架构链接器，能出 aarch64。
$(BIN)/neon.aarch64: src/neon.c | $(BIN)
	$(CLANG) --target=aarch64-linux-gnu -O2 -march=armv8-a+simd \
		-ffreestanding -nostdlib -static -fuse-ld=lld -Wall -Wextra \
		-o $@ src/neon.c

# ---------------- 第 4 篇：瓶颈 ----------------
# 必须关掉 if-conversion，否则 GCC 把 if 变成 cmov，就没有分支可预测了

$(BIN)/bottleneck: src/bottleneck.c common/bench.h | $(BIN)
	$(CC) $(CFLAGS) $(SIMD) -fno-tree-vectorize \
		-fno-if-conversion -fno-if-conversion2 -o $@ src/bottleneck.c

# ---------------- 第 5 篇：观察 ----------------

$(BIN)/hotloop: src/hotloop.c common/bench.h | $(BIN)
	$(CC) $(CFLAGS) -mfma -fno-tree-vectorize -o $@ src/hotloop.c

# ---------------- 核对 ----------------

check: all
	@echo "=== 向量版与标量版是否算同一个数 ==="
	@$(BIN)/dtype | sed -n '/先验证/,/^$$/p'
	@echo "=== ARM64 NEON 与标量是否算同一个数 ==="
	@qemu-aarch64-static $(BIN)/neon.aarch64

clean:
	rm -rf $(BIN)
