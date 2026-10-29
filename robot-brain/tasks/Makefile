# robot-brain/tasks —— 1.3 的代码：把任务拆成可以单独跑、单独测的东西
#
# mission / naive 是同一个场景的两种写法，放在一起才好比。
# naive 只链 world.c —— 它根本用不到 task.c 和 behaviors.c，这本身就是论点。

CC     ?= cc
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra
LDLIBS := -lm

BINS := bin/mission bin/naive bin/test-tasks bin/bench-tick

all: $(BINS)

bin:
	mkdir -p bin

bin/%: %.c world.c task.c behaviors.c | bin
	$(CC) $(CFLAGS) -o $@ $< world.c task.c behaviors.c $(LDLIBS)

bin/naive: naive.c world.c | bin
	$(CC) $(CFLAGS) -o $@ naive.c world.c $(LDLIBS)

clean:
	rm -rf bin

.PHONY: all clean
