CC	?= cc
CFLAGS	?= -O2 -std=gnu11 -Wall -Wextra
LDLIBS	?= -lm

SRC	:= sensor.c sim.c
BINS	:= bin/demo bin/drift bin/delay bin/bench

all: $(BINS)

bin/%: %.c $(SRC) sensor.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LDLIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
