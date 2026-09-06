CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.

TARGETS = test_virtual_board test_dataflow_memory

all: $(TARGETS)

test_virtual_board: virtual_board.c test_virtual_board.c
	$(CC) $(CFLAGS) -o $@ $^

test_dataflow_memory: virtual_board.c test_dataflow_memory.c
	$(CC) $(CFLAGS) -o $@ $^

run: all
	./test_virtual_board
	./test_dataflow_memory

test: run

clean:
	rm -f $(TARGETS)

.PHONY: all run test clean
