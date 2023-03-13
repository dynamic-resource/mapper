CC=gcc
CFLAGS=-Wall -Wextra -O3 $(shell pkg-config --cflags hwloc)

LDFLAGS=-lpthread $(shell pkg-config --libs hwloc)

all: mapper

mapper: mapper.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@
