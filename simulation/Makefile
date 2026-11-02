CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -I.
STRATUM_DIR = ../components/stratum
CONNECT_DIR = ../components/connect
MAIN_DIR = ../main

TARGETS = test_virtual_board test_dataflow_memory test_notify_validate test_power_policy test_net_recovery_policy test_nbits_reference test_ota_guard test_research_sha test_research_guard test_research_console

all: $(TARGETS)

test_virtual_board: virtual_board.c test_virtual_board.c
	$(CC) $(CFLAGS) -o $@ $^

test_dataflow_memory: virtual_board.c test_dataflow_memory.c
	$(CC) $(CFLAGS) -o $@ $^

# Host-only build: no ESP-IDF, no hardware. Runs on the developer machine and in
# the "host-tests" CI job so the mining.notify validation cannot regress.
test_notify_validate: $(STRATUM_DIR)/notify_validate.c test_notify_validate.c
	$(CC) $(CFLAGS) -I$(STRATUM_DIR)/include -std=c11 -o $@ $^

# Thermal (ATM), voltage (V/F) and power-target policies are dependency-free
# decision logic, so they are fully testable on the host as well.
test_power_policy: $(MAIN_DIR)/thermal/atm_policy.c $(MAIN_DIR)/power/vf_tuner.c $(MAIN_DIR)/power/power_target.c test_power_policy.c
	$(CC) $(CFLAGS) -I$(MAIN_DIR)/thermal -I$(MAIN_DIR)/power -std=c11 -o $@ $^

# The network recovery ladder is likewise pure policy. It is testable on the host
# only because it was lifted out of connect.c, where every branch needed a
# misbehaving router to reach.
test_net_recovery_policy: $(CONNECT_DIR)/net_recovery_policy.c test_net_recovery_policy.c
	$(CC) $(CFLAGS) -I$(CONNECT_DIR)/include -std=c11 -o $@ $^

# Differential test of the shipped compact-target (nBits) validator against an
# independent reference decoder. The reference is deliberately a second
# implementation written from the Bitcoin consensus definition, not a wrapper
# around the code under test.
test_nbits_reference: $(STRATUM_DIR)/notify_validate.c test_nbits_reference.c
	$(CC) $(CFLAGS) -I$(STRATUM_DIR)/include -std=c11 -o $@ $^

# The OTA upload gate decides what POST /api/system/OTA may write into an app
# slot. Kept free of ESP-IDF so a factory image, a foreign app or a truncated
# upload can be rejected in a test instead of on the only device in the field.
test_ota_guard: $(MAIN_DIR)/http_server/ota_guard.c test_ota_guard.c
	$(CC) $(CFLAGS) -I$(MAIN_DIR)/http_server -std=c11 -o $@ $^

# The research oracle is the independent implementation a client uses to check
# the miner (and the miner uses to check a client). It must be the same code on
# both sides, so the host build compiles the exact firmware source file.
RESEARCH_DIR = $(MAIN_DIR)/research

test_research_sha: $(RESEARCH_DIR)/research_sha.c test_research_sha.c
	$(CC) $(CFLAGS) -I$(RESEARCH_DIR) -std=c11 -o $@ $^

# The authorisation guard decides whether research work may start at all. Every
# refusal path (pool, thermal, identity, token, budget) is cheap to prove here.
test_research_guard: $(RESEARCH_DIR)/research_guard.c test_research_guard.c
	$(CC) $(CFLAGS) -I$(RESEARCH_DIR) -std=c11 -o $@ $^

# The USB console parses the same commands the HTTP routes accept. A typo has to
# be refused with a reason instead of being guessed into a command, so every
# accepted and every refused shape is a case in this test.
test_research_console: $(RESEARCH_DIR)/research_console_parse.c $(RESEARCH_DIR)/research_sha.c test_research_console.c
	$(CC) $(CFLAGS) -I$(RESEARCH_DIR) -std=c11 -o $@ $^

run: all
	./test_virtual_board
	./test_dataflow_memory
	./test_notify_validate
	./test_power_policy
	./test_net_recovery_policy
	./test_nbits_reference
	./test_ota_guard
	./test_research_sha
	./test_research_guard
	./test_research_console

test: run

clean:
	rm -f $(TARGETS)

.PHONY: all run test clean
