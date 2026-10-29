CC	?= cc
CFLAGS	?= -O2 -std=gnu11 -Wall -Wextra
LDLIBS	?= -lm -lpthread

BINS	:= bin/blocked bin/split bin/bench

all: $(BINS)

bin/%: %.c common.h ring.h | bin
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
