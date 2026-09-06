#include "virtual_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define SIM_TEST(name) \
    do { \
        tests_run++; \
        printf("[TEST %02d] %-55s ... ", tests_run, name); \
    } while(0)

#define SIM_PASS() \
    do { \
        tests_passed++; \
        printf("PASS\n"); \
    } while(0)

/* -------------------------------------------------------------------------
 * Test 1: Phase 1 Virtual Board Model Initialization & State Bounds
 * ------------------------------------------------------------------------- */
void test_phase1_board_model(void) {
    SIM_TEST("Phase 1: Board Architecture & Subsystem Setup");
    sim_board_t board;
    sim_board_init(&board);

    assert(board.virtual_bm1366.chip_id == 0x1366);
    assert(board.virtual_bm1366.core_count == 112);
    assert(board.state.net_state == NET_STATE_BOOT);
    assert(board.state.asic_state == ASIC_STATE_NOT_PRESENT);
    assert(board.work_queue_count == 0);
    assert(sim_verify_all_invariants(&board) == true);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 2: Phase 6 Network Emulation Full Boot & Handshake Pipeline
 * ------------------------------------------------------------------------- */
void test_phase6_network_pipeline(void) {
    SIM_TEST("Phase 6: Full Network Pipeline (Boot->DHCP->DNS->Stratum)");
    sim_board_t b;
    sim_board_init(&b);

    // Run event loop through all states
    while (b.state.net_state != NET_STATE_MINING) {
        sim_net_state_t prev = b.state.net_state;
        sim_net_process_events(&b);
        assert(b.state.net_state != prev); // Must advance monotonically
        assert(sim_verify_all_invariants(&b) == true);
    }

    assert(b.state.net_state == NET_STATE_MINING);
    assert(b.state.pool_state == POOL_STATE_MINING);
    assert(b.virtual_network.dhcp_lease_expiry_us > 0);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 3: Phase 6 Network Fault Injection & Fast Reconnect Recovery
 * ------------------------------------------------------------------------- */
void test_phase6_fault_recovery(void) {
    SIM_TEST("Phase 6: Network Fault Injections & Fast Reconnection");
    sim_board_t b;
    sim_board_init(&b);

    // Boot into MINING
    while (b.state.net_state != NET_STATE_MINING) {
        sim_net_process_events(&b);
    }

    // 1. Inject AP loss
    sim_net_inject_fault(&b, "ap_loss");
    assert(b.state.net_state == NET_STATE_WIFI_LOST);
    assert(b.state.pool_state == POOL_STATE_DISCONNECTED);

    // Process recovery events (fast reconnect backoff)
    sim_net_process_events(&b); // -> FAST_RECONNECT
    assert(b.state.net_state == NET_STATE_FAST_RECONNECT);
    sim_net_process_events(&b); // -> WIFI_CONNECTING
    assert(b.state.net_state == NET_STATE_WIFI_CONNECTING);

    // Restore AP
    b.virtual_network.ap_available = true;
    while (b.state.net_state != NET_STATE_MINING) {
        sim_net_process_events(&b);
    }
    assert(b.state.net_state == NET_STATE_MINING);

    // 2. Inject TCP reset on Stratum
    sim_net_inject_fault(&b, "tcp_reset");
    assert(b.state.net_state == NET_STATE_STRATUM_RECONNECT);
    b.virtual_network.stratum_server_responding = true;
    sim_net_process_events(&b); // -> STRATUM_CONNECTING
    sim_net_process_events(&b); // -> STRATUM_READY
    sim_net_process_events(&b); // -> MINING
    assert(b.state.net_state == NET_STATE_MINING);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 4: Phase 4 & 5 Job Flow, Stale Eviction & Nonce Deduplication
 * ------------------------------------------------------------------------- */
void test_phase4_5_job_flow_dedup(void) {
    SIM_TEST("Phase 4 & 5: Job Planning, Stale Eviction & Dedup");
    sim_board_t b;
    sim_board_init(&b);
    sim_bm1366_init(&b);

    // Simulate incoming mining.notify job 1
    sim_pool_job_t job1 = {
        .job_epoch = 1,
        .job_id = "job_alpha_01",
        .clean_jobs = true,
        .pool_difficulty = 1000.0,
        .version_mask = 0x1FFFE000
    };
    sim_job_receive_notify(&b, &job1);
    assert(b.state.current_job_epoch == 1);

    // Enqueue 3 work items for job 1
    for (uint64_t en2 = 0; en2 < 3; en2++) {
        sim_work_item_t work = {
            .work_generation = (uint32_t)en2,
            .job_epoch = 1,
            .job_slot = (uint8_t)en2,
            .extranonce_2 = en2,
            .ntime = 0x64658BD8,
            .target = 0x1705DD01,
            .pool_diff = 1000.0
        };
        snprintf(work.job_id, sizeof(work.job_id), "%s", job1.job_id);
        sim_work_enqueue(&b, &work);
    }
    assert(b.work_queue_count == 3);

    // Dequeue and dispatch to BM1366
    sim_work_item_t dispatch;
    sim_work_dequeue(&b, &dispatch);
    sim_bm1366_send_work(&b, &dispatch);
    assert(b.state.asic_state == ASIC_STATE_RUNNING);

    // ASIC produces a valid result
    sim_asic_result_t res;
    bool got_res = sim_bm1366_poll_result(&b, &res);
    assert(got_res == true);
    res.job_slot = dispatch.job_slot;

    // Process result -> valid accepted share
    bool accepted = sim_process_asic_result(&b, &res);
    assert(accepted == true);
    assert(b.state.shares_accepted == 1);

    // Re-submit identical result (simulating ASIC duplicate glitch)
    bool dup_accepted = sim_process_asic_result(&b, &res);
    assert(dup_accepted == false); // Must be filtered!
    assert(b.state.shares_duplicate_filtered == 1);
    assert(b.state.shares_accepted == 1); // Unchanged

    // Now clean_jobs=true arrives with job 2
    sim_pool_job_t job2 = {
        .job_epoch = 2,
        .job_id = "job_beta_02",
        .clean_jobs = true,
        .pool_difficulty = 1000.0,
        .version_mask = 0x1FFFE000
    };
    sim_job_receive_notify(&b, &job2);
    assert(b.state.current_job_epoch == 2);

    // Old slot 0 (belonging to job 1) must now be invalidated / stale
    assert(b.valid_jobs[0] == false);

    // Attempt to submit a late result for job 1 slot 0
    bool stale_accepted = sim_process_asic_result(&b, &res);
    assert(stale_accepted == false);
    assert(b.state.shares_stale == 1);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 5: Phase 11 Virtual BM1366 Protocol & CRC5 Validation
 * ------------------------------------------------------------------------- */
void test_phase11_bm1366_protocol(void) {
    SIM_TEST("Phase 11: BM1366 Framing, CRC5 & Error Injection");
    sim_board_t b;
    sim_board_init(&b);
    sim_bm1366_init(&b);

    // Verify CRC5 on standard test vectors
    uint8_t chip_id_req[4] = {0x52, 0x05, 0x00, 0x00};
    assert(sim_crc5(chip_id_req, 4) == 0x0A);

    uint8_t inactive_cmd[4] = {0x53, 0x05, 0x00, 0x00};
    assert(sim_crc5(inactive_cmd, 4) == 0x03);

    // Test CRC5 matches appended CRC byte
    uint8_t complete_cmd[5] = {0x52, 0x05, 0x00, 0x00, 0x0A};
    assert(sim_crc5(complete_cmd, 4) == complete_cmd[4]);

    // Test failure injection: disconnect
    sim_bm1366_inject_failure(&b, "disconnect");
    assert(b.state.asic_state == ASIC_STATE_FAILED);

    // Attempting work send while failed must not report running
    sim_work_item_t dummy = { .job_slot = 0 };
    sim_bm1366_send_work(&b, &dummy);
    assert(b.state.asic_state != ASIC_STATE_RUNNING);
    assert(sim_verify_all_invariants(&b) == true);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 6: Phase 12 Master Invariant Suite (All 12 Invariants)
 * ------------------------------------------------------------------------- */
void test_phase12_master_invariants(void) {
    SIM_TEST("Phase 12: Master Invariants Verification (INV 1-12)");
    sim_board_t b;
    sim_board_init(&b);

    // Baseline check
    assert(sim_verify_all_invariants(&b) == true);

    // Invariant 1 violation: Enqueue item with epoch 0
    sim_work_item_t bad_item = { .job_epoch = 0 };
    b.work_queue[0] = bad_item;
    b.work_queue_count = 1;
    assert(sim_verify_all_invariants(&b) == false);
    b.work_queue_count = 0; // restore

    // Invariant 5 violation: ASIC offline but marked RUNNING
    b.virtual_bm1366.is_powered = false;
    b.state.asic_state = ASIC_STATE_RUNNING;
    assert(sim_verify_all_invariants(&b) == false);
    b.state.asic_state = ASIC_STATE_NOT_PRESENT; // restore

    // Invariant 6 violation: Telemetry invalid but mapped to 0.0 instead of N/A
    b.state.telemetry_valid = false;
    b.state.temperature_c = 0.0;
    assert(sim_verify_all_invariants(&b) == false);
    b.state.temperature_c = -1.0; // N/A indicator
    b.state.telemetry_valid = true;

    // Invariant 10 violation: Queue count > capacity
    b.work_queue_count = SIM_QUEUE_CAPACITY + 1;
    assert(sim_verify_all_invariants(&b) == false);
    b.work_queue_count = 0;

    // Invariant 12 violation: Invalid net state
    b.state.net_state = NET_STATE_MAX;
    assert(sim_verify_all_invariants(&b) == false);
    b.state.net_state = NET_STATE_BOOT;

    assert(sim_verify_all_invariants(&b) == true);

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 7: Phase 13 Property-Based Chaotic Sequence Testing
 * ------------------------------------------------------------------------- */
void test_phase13_property_fuzzing(void) {
    SIM_TEST("Phase 13: Property-Based Fuzzing (10,000 Chaos Events)");
    sim_board_t b;
    sim_board_init(&b);
    sim_bm1366_init(&b);

    srand(1337); // Deterministic seed
    uint32_t epoch = 1;

    for (int step = 0; step < 10000; step++) {
        int action = rand() % 8;
        switch (action) {
            case 0: // Progress network
                sim_net_process_events(&b);
                break;
            case 1: // Inject Wi-Fi loss
                sim_net_inject_fault(&b, "ap_loss");
                break;
            case 2: // Restore Wi-Fi
                b.virtual_network.ap_available = true;
                break;
            case 3: // Inject Stratum reset
                sim_net_inject_fault(&b, "tcp_reset");
                break;
            case 4: // New Job notification
                {
                    epoch++;
                    sim_pool_job_t job = {
                        .job_epoch = epoch,
                        .clean_jobs = (rand() % 2 == 0),
                        .pool_difficulty = 1000.0,
                        .version_mask = 0x1FFFE000
                    };
                    snprintf(job.job_id, sizeof(job.job_id), "job_%u", epoch);
                    sim_job_receive_notify(&b, &job);
                }
                break;
            case 5: // Enqueue Work
                if (b.state.current_job_epoch > 0 && b.work_queue_count < SIM_QUEUE_CAPACITY) {
                    uint8_t slot = (uint8_t)(rand() % SIM_MAX_ACTIVE_JOBS);
                    sim_work_item_t work = {
                        .job_epoch = b.state.current_job_epoch,
                        .job_slot = slot,
                        .extranonce_2 = (uint64_t)step,
                        .pool_diff = 1000.0
                    };
                    snprintf(work.job_id, sizeof(work.job_id), "%s", b.state.current_job_id);
                    sim_work_enqueue(&b, &work);
                }
                break;
            case 6: // Dequeue and process
                {
                    sim_work_item_t work;
                    if (sim_work_dequeue(&b, &work)) {
                        sim_bm1366_send_work(&b, &work);
                        sim_asic_result_t res;
                        if (sim_bm1366_poll_result(&b, &res)) {
                            res.job_slot = work.job_slot;
                            sim_process_asic_result(&b, &res);
                        }
                    }
                }
                break;
            case 7: // Advance time
                sim_advance_time(&b, 100000ULL); // 100 ms
                break;
        }

        // Verify invariants on every step!
        if (!sim_verify_all_invariants(&b)) {
            printf("\n[FUZZ ERROR at step %d, action %d]: %s\n", step, action, b.last_violation_desc);
            fflush(stdout);
            assert(false);
        }
    }

    SIM_PASS();
}

/* -------------------------------------------------------------------------
 * Test 8: Phase 14 Performance Model & Virtual Baseline Benchmark
 * ------------------------------------------------------------------------- */
void test_phase14_performance_baseline(void) {
    SIM_TEST("Phase 14: Performance Model & Microbenchmarks");
    sim_board_t b;
    sim_board_init(&b);
    sim_bm1366_init(&b);

    clock_t start = clock();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        // Enqueue and dequeue work item
        sim_work_item_t w = {
            .job_epoch = 1,
            .job_slot = (uint8_t)(i % SIM_MAX_ACTIVE_JOBS),
            .extranonce_2 = (uint64_t)i,
            .pool_diff = 1000.0
        };
        sim_work_enqueue(&b, &w);
        sim_work_item_t out;
        sim_work_dequeue(&b, &out);

        // Duplicate share filter lookup
        sim_is_duplicate_share(&b, "test_job", (uint64_t)i, 0x64000000, 0x12345678, 0x00020000);
    }

    clock_t end = clock();
    double cpu_time = ((double)(end - start)) / CLOCKS_PER_SEC;
    double ops_per_sec = (iterations * 3.0) / cpu_time;

    printf("PASS (%.3f s, %.0f ops/sec)\n", cpu_time, ops_per_sec);
    tests_passed++;
}

/* -------------------------------------------------------------------------
 * Main Runner
 * ------------------------------------------------------------------------- */
int main(void) {
    printf("================================================================\n");
    printf("Bitaxe Ultra (Board 201) / BM1366 Virtual Board Emulation Suite\n");
    printf("Specification: GEMINI.md Phases 1 to 18\n");
    printf("Classification: SIMULATED / TESTED_LOCALLY / NOT_HARDWARE_VALIDATED\n");
    printf("================================================================\n");

    test_phase1_board_model();
    test_phase6_network_pipeline();
    test_phase6_fault_recovery();
    test_phase4_5_job_flow_dedup();
    test_phase11_bm1366_protocol();
    test_phase12_master_invariants();
    test_phase13_property_fuzzing();
    test_phase14_performance_baseline();

    printf("================================================================\n");
    printf("Results: %d of %d tests passed successfully (100%% GREEN).\n", tests_passed, tests_run);
    printf("================================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
