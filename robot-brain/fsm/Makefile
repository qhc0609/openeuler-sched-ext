CC	?= cc
CFLAGS	?= -O2 -std=gnu11 -Wall -Wextra
LDLIBS	?= -lm

SRC	:= fsm.c
BINS	:= bin/chatter bin/illegal bin/hold bin/bench

all: $(BINS)

bin/%: %.c $(SRC) fsm.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LDLIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
