CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.
STRATUM_DIR = ../components/stratum
MAIN_DIR = ../main

TARGETS = test_virtual_board test_dataflow_memory test_notify_validate test_power_policy

all: $(TARGETS)

test_virtual_board: virtual_board.c test_virtual_board.c
	$(CC) $(CFLAGS) -o $@ $^

test_dataflow_memory: virtual_board.c test_dataflow_memory.c
	$(CC) $(CFLAGS) -o $@ $^

# Host-only build: no ESP-IDF, no hardware. Runs on the developer machine and in
# the "host-tests" CI job so the mining.notify validation cannot regress.
test_notify_validate: $(STRATUM_DIR)/notify_validate.c test_notify_validate.c
	$(CC) $(CFLAGS) -I$(STRATUM_DIR)/include -std=c11 -o $@ $^

# Thermal (ATM) and voltage (V/F) policies are dependency-free decision logic,
# so they are fully testable on the host as well.
test_power_policy: $(MAIN_DIR)/thermal/atm_policy.c $(MAIN_DIR)/power/vf_tuner.c test_power_policy.c
	$(CC) $(CFLAGS) -I$(MAIN_DIR)/thermal -I$(MAIN_DIR)/power -std=c11 -o $@ $^

run: all
	./test_virtual_board
	./test_dataflow_memory
	./test_notify_validate
	./test_power_policy

test: run

clean:
	rm -f $(TARGETS)

.PHONY: all run test clean
