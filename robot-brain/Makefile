CC      ?= cc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Iinclude
LDLIBS  := -lm
AR      ?= ar

TRANSPORT_OBJS := transport/transport.o transport/transport_shm.o \
                  transport/transport_ivshmem.o

BINS := bin/brain bin/cerebellum bin/spin

all: $(BINS)

libtransport.a: $(TRANSPORT_OBJS)
	$(AR) rcs $@ $^

bin:
	mkdir -p bin

bin/brain: brain/brain.o libtransport.a | bin
	$(CC) $(CFLAGS) -o $@ brain/brain.o libtransport.a $(LDLIBS)

bin/cerebellum: cerebellum/cerebellum.o libtransport.a | bin
	$(CC) $(CFLAGS) -o $@ cerebellum/cerebellum.o libtransport.a $(LDLIBS)

bin/spin: test/spin.c | bin
	$(CC) $(CFLAGS) -o $@ test/spin.c

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f transport/*.o brain/*.o cerebellum/*.o libtransport.a
	rm -rf bin

.PHONY: all clean
