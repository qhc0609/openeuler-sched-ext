CC	?= cc
CFLAGS	?= -O2 -std=gnu11 -Wall -Wextra
LDLIBS	?= -lm

SRC	:= pose.c cmd.c
BINS	:= bin/drive bin/bench

all: $(BINS)

bin/%: %.c $(SRC) pose.h cmd.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LDLIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
