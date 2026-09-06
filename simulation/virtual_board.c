#include "virtual_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

const char *sim_asic_state_to_string(sim_asic_state_t state) {
    switch (state) {
        case ASIC_STATE_NOT_PRESENT: return "NOT_PRESENT";
        case ASIC_STATE_DETECTED:    return "DETECTED";
        case ASIC_STATE_SUPPORTED:   return "SUPPORTED";
        case ASIC_STATE_READY:       return "READY";
        case ASIC_STATE_RUNNING:     return "RUNNING";
        case ASIC_STATE_FAILED:      return "FAILED";
        case ASIC_STATE_UNSUPPORTED: return "UNSUPPORTED";
        default:                     return "UNKNOWN";
    }
}

const char *sim_net_state_to_string(sim_net_state_t state) {
    switch (state) {
        case NET_STATE_BOOT:               return "BOOT";
        case NET_STATE_WIFI_INIT:          return "WIFI_INIT";
        case NET_STATE_WIFI_CONNECTING:    return "WIFI_CONNECTING";
        case NET_STATE_WIFI_CONNECTED:     return "WIFI_CONNECTED";
        case NET_STATE_WIFI_CONNECTED_NO_IP: return "WIFI_CONNECTED_NO_IP";
        case NET_STATE_DHCP:               return "DHCP";
        case NET_STATE_DHCP_RETRY:         return "DHCP_RETRY";
        case NET_STATE_IP_READY:           return "IP_READY";
        case NET_STATE_IP_ACQUIRED:        return "IP_ACQUIRED";
        case NET_STATE_ROUTE_CHECK:        return "ROUTE_CHECK";
        case NET_STATE_DNS_READY:          return "DNS_READY";
        case NET_STATE_INTERNET_READY:     return "INTERNET_READY";
        case NET_STATE_STRATUM_CONNECTING: return "STRATUM_CONNECTING";
        case NET_STATE_STRATUM_READY:      return "STRATUM_READY";
        case NET_STATE_MINING:             return "MINING";
        case NET_STATE_WIFI_AUTH_FAILED:   return "WIFI_AUTH_FAILED";
        case NET_STATE_WIFI_AP_UNAVAILABLE:return "WIFI_AP_UNAVAILABLE";
        case NET_STATE_DHCP_FAILED:        return "DHCP_FAILED";
        case NET_STATE_DNS_FAILED:         return "DNS_FAILED";
        case NET_STATE_ROUTE_FAILED:       return "ROUTE_FAILED";
        case NET_STATE_STRATUM_FAILED:     return "STRATUM_FAILED";
        case NET_STATE_WIFI_LOST:          return "WIFI_LOST";
        case NET_STATE_WIFI_RECOVERY:      return "WIFI_RECOVERY";
        case NET_STATE_DHCP_RECOVERY:      return "DHCP_RECOVERY";
        case NET_STATE_DNS_RECOVERY:       return "DNS_RECOVERY";
        case NET_STATE_SOCKET_RECOVERY:    return "SOCKET_RECOVERY";
        case NET_STATE_STRATUM_RECOVERY:   return "STRATUM_RECOVERY";
        case NET_STATE_FAST_RECONNECT:     return "FAST_RECONNECT";
        case NET_STATE_DNS_RETRY:          return "DNS_RETRY";
        case NET_STATE_STRATUM_RECONNECT:  return "STRATUM_RECONNECT";
        case NET_STATE_JOB_SYNC:           return "JOB_SYNC";
        case NET_STATE_RECOVERY_VERIFY:    return "RECOVERY_VERIFY";
        default:                           return "INVALID_STATE";
    }
}

/* -------------------------------------------------------------------------
 * Exact CRC5 calculation matching components/asic/crc.c
 * ------------------------------------------------------------------------- */
uint8_t sim_crc5(const uint8_t *data, size_t length) {
    uint8_t crc = 0x1F;
    for (size_t byte_counter = 0; byte_counter < length; byte_counter++) {
        uint8_t byte = data[byte_counter];
        for (uint8_t bit_counter = 0; bit_counter < 8; bit_counter++) {
            uint8_t bit = (byte >> 7) & 1;
            byte <<= 1;

            uint8_t new_bit = ((crc >> 4) ^ bit) & 1;
            crc = ((crc << 1) | new_bit) ^ (new_bit << 2);
            crc &= 0x1F;
        }
    }
    return crc;
}

/* -------------------------------------------------------------------------
 * Board Initialization & Reset
 * ------------------------------------------------------------------------- */
void sim_board_init(sim_board_t *b) {
    memset(b, 0, sizeof(sim_board_t));
    
    b->internal_sram_total = 512 * 1024;
    b->psram_total = 8 * 1024 * 1024;
    b->internal_sram_used = 128 * 1024; // Baseline system footprint
    b->psram_used = 512 * 1024;        // Baseline Web UI footprint
    
    b->virtual_bm1366.chip_id = 0x1366;
    b->virtual_bm1366.core_count = 112;
    b->virtual_bm1366.small_core_count = 8;
    b->virtual_bm1366.reg_0xa4_version_mask = 0x1FFFE000;
    b->virtual_bm1366.clock_mhz = 485.0;
    b->virtual_bm1366.is_powered = false;
    b->virtual_bm1366.is_communicating = false;
    
    b->virtual_emc2101.ext_temp_c = 59.0;
    b->virtual_emc2101.target_temp_c = 60.0;
    b->virtual_emc2101.fan_pwm_duty = 29.8;
    b->virtual_emc2101.tach_rpm = 3590;
    
    b->virtual_ina260.bus_voltage_mv = 5048.0;
    b->virtual_ina260.shunt_current_ma = 2460.0;
    b->virtual_ina260.power_w = 12.44;
    
    b->virtual_ds4432.output_voltage_mv = 1206.0;
    
    b->virtual_network.ap_available = true;
    b->virtual_network.auth_success = true;
    b->virtual_network.dhcp_server_responding = true;
    b->virtual_network.dns_server_responding = true;
    b->virtual_network.stratum_server_responding = true;
    b->virtual_network.dhcp_lease_duration_sec = 86400;
    snprintf(b->virtual_network.assigned_ip, sizeof(b->virtual_network.assigned_ip), "192.168.178.66");
    snprintf(b->virtual_network.resolved_pool_ip, sizeof(b->virtual_network.resolved_pool_ip), "188.165.214.18");
    
    b->state.net_state = NET_STATE_BOOT;
    b->state.pool_state = POOL_STATE_DISCONNECTED;
    b->state.asic_state = ASIC_STATE_NOT_PRESENT;
    b->state.active_difficulty = 1000.0;
    b->state.version_mask = 0x1FFFE000;
    b->state.telemetry_valid = true;
    b->state.core_voltage_mv = 1206.0;
    b->state.board_power_w = 12.44;
    b->state.temperature_c = 59.0;
    b->state.fan_speed_pct = 29.8;
    b->state.fan_rpm = 3590;
    b->state.frequency_mhz = 485;
    b->state.hashrate_ghs = 435.9;
    b->state.effective_hashrate_ghs = 434.5;
}

void sim_board_reset(sim_board_t *b) {
    sim_board_init(b);
}

void sim_advance_time(sim_board_t *b, uint64_t delta_us) {
    b->sim_time_us += delta_us;
    b->tick_count += (delta_us / 1000); // 1 ms FreeRTOS tick
    
    // Check DHCP lease expiry
    if (b->state.net_state >= NET_STATE_IP_READY && b->virtual_network.dhcp_lease_expiry_us > 0) {
        if (b->sim_time_us >= b->virtual_network.dhcp_lease_expiry_us) {
            // Initiate non-destructive renewal
            b->virtual_network.dhcp_lease_expiry_us = b->sim_time_us + ((uint64_t)b->virtual_network.dhcp_lease_duration_sec * 1000000ULL);
        }
    }
}

/* -------------------------------------------------------------------------
 * Explicit Network State Machine (GEMINI.md Section 12 & Phase 6)
 * ------------------------------------------------------------------------- */
bool sim_net_transition(sim_board_t *b, sim_net_state_t new_state) {
    sim_net_state_t current = b->state.net_state;
    bool valid = false;

    switch (current) {
        case NET_STATE_BOOT:
            valid = (new_state == NET_STATE_WIFI_INIT);
            break;
        case NET_STATE_WIFI_INIT:
            valid = (new_state == NET_STATE_WIFI_CONNECTING);
            break;
        case NET_STATE_WIFI_CONNECTING:
            valid = (new_state == NET_STATE_WIFI_CONNECTED || new_state == NET_STATE_WIFI_CONNECTED_NO_IP || new_state == NET_STATE_WIFI_LOST || new_state == NET_STATE_WIFI_AUTH_FAILED || new_state == NET_STATE_FAST_RECONNECT);
            break;
        case NET_STATE_WIFI_CONNECTED:
            valid = (new_state == NET_STATE_DHCP || new_state == NET_STATE_WIFI_CONNECTED_NO_IP || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_WIFI_CONNECTED_NO_IP:
            valid = (new_state == NET_STATE_DHCP || new_state == NET_STATE_DHCP_RETRY || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DHCP:
            valid = (new_state == NET_STATE_IP_READY || new_state == NET_STATE_IP_ACQUIRED || new_state == NET_STATE_DHCP_RETRY || new_state == NET_STATE_DHCP_FAILED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_IP_READY:
        case NET_STATE_IP_ACQUIRED:
            valid = (new_state == NET_STATE_ROUTE_CHECK || new_state == NET_STATE_DNS_READY || new_state == NET_STATE_DNS_RETRY || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_ROUTE_CHECK:
            valid = (new_state == NET_STATE_DNS_READY || new_state == NET_STATE_ROUTE_FAILED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DNS_READY:
        case NET_STATE_INTERNET_READY:
            valid = (new_state == NET_STATE_STRATUM_CONNECTING || new_state == NET_STATE_DNS_RETRY || new_state == NET_STATE_DNS_FAILED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_STRATUM_CONNECTING:
            valid = (new_state == NET_STATE_STRATUM_READY || new_state == NET_STATE_STRATUM_RECONNECT || new_state == NET_STATE_STRATUM_FAILED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_STRATUM_READY:
            valid = (new_state == NET_STATE_MINING || new_state == NET_STATE_STRATUM_RECONNECT || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_MINING:
            valid = (new_state == NET_STATE_WIFI_LOST || new_state == NET_STATE_STRATUM_RECONNECT || new_state == NET_STATE_JOB_SYNC || new_state == NET_STATE_DHCP_RECOVERY);
            break;
        case NET_STATE_WIFI_LOST:
            valid = (new_state == NET_STATE_WIFI_RECOVERY || new_state == NET_STATE_FAST_RECONNECT || new_state == NET_STATE_BOOT);
            break;
        case NET_STATE_WIFI_RECOVERY:
            valid = (new_state == NET_STATE_WIFI_CONNECTING || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DHCP_FAILED:
            valid = (new_state == NET_STATE_DHCP_RECOVERY || new_state == NET_STATE_WIFI_RECOVERY || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DHCP_RECOVERY:
            valid = (new_state == NET_STATE_DHCP || new_state == NET_STATE_WIFI_CONNECTING || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_FAST_RECONNECT:
            valid = (new_state == NET_STATE_WIFI_CONNECTING || new_state == NET_STATE_WIFI_CONNECTED || new_state == NET_STATE_WIFI_CONNECTED_NO_IP || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DHCP_RETRY:
            valid = (new_state == NET_STATE_DHCP || new_state == NET_STATE_DHCP_FAILED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_DNS_RETRY:
        case NET_STATE_DNS_RECOVERY:
            valid = (new_state == NET_STATE_DNS_READY || new_state == NET_STATE_IP_READY || new_state == NET_STATE_IP_ACQUIRED || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_SOCKET_RECOVERY:
            valid = (new_state == NET_STATE_STRATUM_CONNECTING || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_STRATUM_RECOVERY:
        case NET_STATE_STRATUM_RECONNECT:
            valid = (new_state == NET_STATE_STRATUM_CONNECTING || new_state == NET_STATE_JOB_SYNC || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_JOB_SYNC:
            valid = (new_state == NET_STATE_MINING || new_state == NET_STATE_STRATUM_RECONNECT || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_RECOVERY_VERIFY:
            valid = (new_state == NET_STATE_MINING || new_state == NET_STATE_WIFI_LOST);
            break;
        case NET_STATE_WIFI_AUTH_FAILED:
        case NET_STATE_WIFI_AP_UNAVAILABLE:
        case NET_STATE_DNS_FAILED:
        case NET_STATE_ROUTE_FAILED:
        case NET_STATE_STRATUM_FAILED:
            valid = (new_state == NET_STATE_WIFI_RECOVERY || new_state == NET_STATE_DHCP_RECOVERY || new_state == NET_STATE_BOOT);
            break;
        default:
            valid = false;
            break;
    }

    if (valid) {
        b->state.net_state = new_state;
        if (new_state == NET_STATE_WIFI_LOST) {
            b->state.pool_state = POOL_STATE_DISCONNECTED;
        } else if (new_state == NET_STATE_STRATUM_RECONNECT) {
            b->state.pool_state = POOL_STATE_RECONNECTING;
        } else if (new_state == NET_STATE_MINING) {
            b->state.pool_state = POOL_STATE_MINING;
        }
        return true;
    } else {
        b->invariant_violations++;
        snprintf(b->last_violation_desc, sizeof(b->last_violation_desc),
                 "Illegal net transition from %s to %s",
                 sim_net_state_to_string(current), sim_net_state_to_string(new_state));
        return false;
    }
}

void sim_net_process_events(sim_board_t *b) {
    switch (b->state.net_state) {
        case NET_STATE_BOOT:
            sim_net_transition(b, NET_STATE_WIFI_INIT);
            break;
        case NET_STATE_WIFI_INIT:
            sim_net_transition(b, NET_STATE_WIFI_CONNECTING);
            break;
        case NET_STATE_WIFI_CONNECTING:
            if (b->virtual_network.ap_available && b->virtual_network.auth_success) {
                sim_net_transition(b, NET_STATE_WIFI_CONNECTED);
            } else {
                sim_net_transition(b, NET_STATE_WIFI_LOST);
            }
            break;
        case NET_STATE_WIFI_CONNECTED:
            sim_net_transition(b, NET_STATE_DHCP);
            break;
        case NET_STATE_DHCP:
            if (b->virtual_network.dhcp_server_responding) {
                b->virtual_network.dhcp_lease_expiry_us = b->sim_time_us + ((uint64_t)b->virtual_network.dhcp_lease_duration_sec * 1000000ULL);
                sim_net_transition(b, NET_STATE_IP_READY);
            } else {
                sim_net_transition(b, NET_STATE_DHCP_RETRY);
            }
            break;
        case NET_STATE_DHCP_RETRY:
            b->virtual_network.connect_retries++;
            sim_net_transition(b, NET_STATE_DHCP);
            break;
        case NET_STATE_IP_READY:
            if (b->virtual_network.dns_server_responding) {
                sim_net_transition(b, NET_STATE_DNS_READY);
            } else {
                sim_net_transition(b, NET_STATE_DNS_RETRY);
            }
            break;
        case NET_STATE_DNS_RETRY:
            b->virtual_network.connect_retries++;
            sim_net_transition(b, NET_STATE_IP_READY);
            break;
        case NET_STATE_DNS_READY:
            sim_net_transition(b, NET_STATE_STRATUM_CONNECTING);
            break;
        case NET_STATE_STRATUM_CONNECTING:
            if (b->virtual_network.stratum_server_responding) {
                sim_net_transition(b, NET_STATE_STRATUM_READY);
            } else {
                sim_net_transition(b, NET_STATE_STRATUM_RECONNECT);
            }
            break;
        case NET_STATE_STRATUM_READY:
            sim_net_transition(b, NET_STATE_MINING);
            break;
        case NET_STATE_WIFI_LOST:
            b->virtual_network.fast_reconnect_count++;
            sim_net_transition(b, NET_STATE_FAST_RECONNECT);
            break;
        case NET_STATE_FAST_RECONNECT:
            sim_net_transition(b, NET_STATE_WIFI_CONNECTING);
            break;
        case NET_STATE_STRATUM_RECONNECT:
            b->virtual_network.connect_retries++;
            sim_net_transition(b, NET_STATE_STRATUM_CONNECTING);
            break;
        case NET_STATE_JOB_SYNC:
            sim_net_transition(b, NET_STATE_MINING);
            break;
        default:
            break;
    }
}

void sim_net_inject_fault(sim_board_t *b, const char *fault_type) {
    if (strcmp(fault_type, "ap_loss") == 0) {
        b->virtual_network.ap_available = false;
        sim_net_transition(b, NET_STATE_WIFI_LOST);
    } else if (strcmp(fault_type, "dhcp_timeout") == 0) {
        b->virtual_network.dhcp_server_responding = false;
        if (b->state.net_state == NET_STATE_DHCP) {
            sim_net_transition(b, NET_STATE_DHCP_RETRY);
        }
    } else if (strcmp(fault_type, "dns_failure") == 0) {
        b->virtual_network.dns_server_responding = false;
        if (b->state.net_state == NET_STATE_IP_READY) {
            sim_net_transition(b, NET_STATE_DNS_RETRY);
        }
    } else if (strcmp(fault_type, "tcp_reset") == 0 || strcmp(fault_type, "stratum_disconnect") == 0) {
        b->virtual_network.stratum_server_responding = false;
        sim_net_transition(b, NET_STATE_STRATUM_RECONNECT);
    }
}

/* -------------------------------------------------------------------------
 * Job Planning & Work Scheduling (GEMINI.md Phase 4 & 5)
 * ------------------------------------------------------------------------- */
bool sim_job_receive_notify(sim_board_t *b, const sim_pool_job_t *notify) {
    assert(b != NULL && notify != NULL);

    b->state.current_job_epoch++;
    snprintf(b->state.current_job_id, sizeof(b->state.current_job_id), "%s", notify->job_id);
    b->state.active_difficulty = notify->pool_difficulty;

    if (notify->clean_jobs) {
        sim_work_clean_stale(b, b->state.current_job_epoch);
    }

    return true;
}

bool sim_work_enqueue(sim_board_t *b, const sim_work_item_t *work) {
    if (work->job_epoch == 0) {
        b->invariant_violations++;
        snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "Cannot enqueue work with invalid job epoch 0");
        return false;
    }

    if (b->work_queue_count >= SIM_QUEUE_CAPACITY) {
        b->invariant_violations++;
        snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "Work queue overflow beyond capacity %d", SIM_QUEUE_CAPACITY);
        return false;
    }

    b->work_queue[b->work_queue_tail] = *work;
    b->work_queue_tail = (b->work_queue_tail + 1) % SIM_QUEUE_CAPACITY;
    b->work_queue_count++;
    if (b->work_queue_count > b->work_queue_peak) {
        b->work_queue_peak = b->work_queue_count;
    }

    // Register into active_jobs table slot
    uint8_t slot = work->job_slot;
    if (slot < SIM_MAX_ACTIVE_JOBS) {
        if (b->active_jobs[slot] == NULL) {
            b->active_jobs[slot] = malloc(sizeof(sim_work_item_t));
            b->total_allocations++;
        }
        *b->active_jobs[slot] = *work;
        b->valid_jobs[slot] = true;
    }

    return true;
}

bool sim_work_dequeue(sim_board_t *b, sim_work_item_t *out_work) {
    if (b->work_queue_count == 0) {
        return false;
    }

    *out_work = b->work_queue[b->work_queue_head];
    b->work_queue_head = (b->work_queue_head + 1) % SIM_QUEUE_CAPACITY;
    b->work_queue_count--;

    return true;
}

void sim_work_clean_stale(sim_board_t *b, uint32_t keep_job_epoch) {
    // Purge queue items belonging to older epochs
    size_t count = b->work_queue_count;
    size_t new_count = 0;
    size_t head = b->work_queue_head;

    for (size_t i = 0; i < count; i++) {
        size_t idx = (head + i) % SIM_QUEUE_CAPACITY;
        if (b->work_queue[idx].job_epoch == keep_job_epoch) {
            if (new_count != i) {
                b->work_queue[(head + new_count) % SIM_QUEUE_CAPACITY] = b->work_queue[idx];
            }
            new_count++;
        } else {
            b->work_queue[idx].is_stale = true;
        }
    }
    b->work_queue_count = new_count;
    b->work_queue_tail = (head + new_count) % SIM_QUEUE_CAPACITY;

    // Invalidate active_jobs slots for older epochs
    for (size_t slot = 0; slot < SIM_MAX_ACTIVE_JOBS; slot++) {
        if (b->valid_jobs[slot] && b->active_jobs[slot] != NULL) {
            if (b->active_jobs[slot]->job_epoch != keep_job_epoch) {
                b->valid_jobs[slot] = false;
                b->active_jobs[slot]->is_stale = true;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Virtual BM1366 ASIC (GEMINI.md Phase 11)
 * ------------------------------------------------------------------------- */
void sim_bm1366_init(sim_board_t *b) {
    b->virtual_bm1366.is_powered = true;
    b->virtual_bm1366.is_communicating = true;
    b->state.asic_state = ASIC_STATE_READY;
}

void sim_bm1366_send_work(sim_board_t *b, const sim_work_item_t *work) {
    (void)work;
    if (!b->virtual_bm1366.is_powered || !b->virtual_bm1366.is_communicating) {
        b->state.asic_state = ASIC_STATE_FAILED;
        return;
    }
    b->state.asic_state = ASIC_STATE_RUNNING;
    b->virtual_bm1366.uart_tx_len = 80; // Standard BM1366 work frame length
}

bool sim_bm1366_poll_result(sim_board_t *b, sim_asic_result_t *out_result) {
    if (b->state.asic_state != ASIC_STATE_RUNNING) {
        return false;
    }

    // Synthesize 11-byte frame response with valid CRC5
    uint8_t payload[8] = {0x12, 0x34, 0x56, 0x78, 0x00, 0x01, 0x00, 0x00};
    uint8_t crc = sim_crc5(payload, 8);

    out_result->job_slot = 0;
    out_result->nonce = 0x78563412;
    out_result->rolled_version = b->state.version_mask & 0x00020000;
    out_result->core_id = 42;
    out_result->small_core_id = 3;
    out_result->asic_nr = 0;
    out_result->crc5 = crc;
    out_result->is_valid_crc = true;
    out_result->timestamp_us = b->sim_time_us;

    return true;
}

void sim_bm1366_inject_failure(sim_board_t *b, const char *failure_mode) {
    if (strcmp(failure_mode, "disconnect") == 0) {
        b->virtual_bm1366.is_communicating = false;
        b->state.asic_state = ASIC_STATE_FAILED;
    } else if (strcmp(failure_mode, "power_loss") == 0) {
        b->virtual_bm1366.is_powered = false;
        b->virtual_bm1366.is_communicating = false;
        b->state.asic_state = ASIC_STATE_NOT_PRESENT;
    }
}

/* -------------------------------------------------------------------------
 * Share Processing & Duplicate Submission Filter (Phase 5)
 * ------------------------------------------------------------------------- */
bool sim_is_duplicate_share(sim_board_t *b, const char *job_id, uint64_t en2, uint32_t ntime, uint32_t nonce, uint32_t ver_bits) {
    for (size_t i = 0; i < SIM_DUP_CACHE_SIZE; i++) {
        if (b->dup_filter_cache[i].valid &&
            b->dup_filter_cache[i].nonce == nonce &&
            b->dup_filter_cache[i].ntime == ntime &&
            b->dup_filter_cache[i].version_bits == ver_bits &&
            b->dup_filter_cache[i].en2 == en2 &&
            strcmp(b->dup_filter_cache[i].job_id, job_id) == 0) {
            return true;
        }
    }
    return false;
}

void sim_record_share(sim_board_t *b, const char *job_id, uint64_t en2, uint32_t ntime, uint32_t nonce, uint32_t ver_bits) {
    size_t idx = b->dup_filter_idx;
    b->dup_filter_cache[idx].valid = true;
    b->dup_filter_cache[idx].nonce = nonce;
    b->dup_filter_cache[idx].ntime = ntime;
    b->dup_filter_cache[idx].version_bits = ver_bits;
    b->dup_filter_cache[idx].en2 = en2;
    snprintf(b->dup_filter_cache[idx].job_id, sizeof(b->dup_filter_cache[idx].job_id), "%s", job_id);
    
    b->dup_filter_idx = (idx + 1) % SIM_DUP_CACHE_SIZE;
}

bool sim_process_asic_result(sim_board_t *b, const sim_asic_result_t *res) {
    if (!res->is_valid_crc) {
        b->virtual_bm1366.error_counter++;
        return false;
    }

    uint8_t slot = res->job_slot;
    if (slot >= SIM_MAX_ACTIVE_JOBS || !b->valid_jobs[slot] || b->active_jobs[slot] == NULL) {
        b->state.shares_stale++;
        return false;
    }

    sim_work_item_t *work = b->active_jobs[slot];
    if (work->job_epoch != b->state.current_job_epoch) {
        b->state.shares_stale++;
        return false;
    }

    uint32_t ver_bits = res->rolled_version;
    if (sim_is_duplicate_share(b, work->job_id, work->extranonce_2, work->ntime, res->nonce, ver_bits)) {
        b->state.shares_duplicate_filtered++;
        b->state.duplicate_work_prevented++;
        return false;
    }

    sim_record_share(b, work->job_id, work->extranonce_2, work->ntime, res->nonce, ver_bits);
    b->state.shares_submitted++;
    b->state.shares_accepted++;
    b->state.nonces_found++;

    return true;
}

/* -------------------------------------------------------------------------
 * Telemetry & Single Source of Truth
 * ------------------------------------------------------------------------- */
void sim_telemetry_update(sim_board_t *b) {
    // Only update from single authoritative sources
    b->state.temperature_c = b->virtual_emc2101.ext_temp_c;
    b->state.fan_speed_pct = b->virtual_emc2101.fan_pwm_duty;
    b->state.fan_rpm = b->virtual_emc2101.tach_rpm;
    b->state.core_voltage_mv = b->virtual_ds4432.output_voltage_mv;
    b->state.board_power_w = b->virtual_ina260.power_w;
    b->state.frequency_mhz = (uint32_t)b->virtual_bm1366.clock_mhz;
}

/* -------------------------------------------------------------------------
 * Master Invariant Verification (GEMINI.md Phase 12)
 * ------------------------------------------------------------------------- */
bool sim_verify_all_invariants(sim_board_t *b) {
    // INV-1: kein Work item ohne gültige Job Epoch
    for (size_t i = 0; i < b->work_queue_count; i++) {
        size_t idx = (b->work_queue_head + i) % SIM_QUEUE_CAPACITY;
        if (b->work_queue[idx].job_epoch == 0) {
            b->invariant_violations++;
            snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-1: Work item in queue has invalid job epoch 0");
            return false;
        }
    }

    // INV-3: stale result wird verworfen
    for (size_t slot = 0; slot < SIM_MAX_ACTIVE_JOBS; slot++) {
        if (b->valid_jobs[slot] && b->active_jobs[slot] != NULL) {
            if (b->active_jobs[slot]->is_stale) {
                b->invariant_violations++;
                snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-3: Stale job still marked valid in slot %zu", slot);
                return false;
            }
        }
    }

    // INV-5: ASIC not present wird nicht als running gemeldet
    if (!b->virtual_bm1366.is_powered || !b->virtual_bm1366.is_communicating) {
        if (b->state.asic_state == ASIC_STATE_RUNNING || b->state.asic_state == ASIC_STATE_READY) {
            b->invariant_violations++;
            snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-5: Unpowered/offline ASIC reported as %s", sim_asic_state_to_string(b->state.asic_state));
            return false;
        }
    }

    // INV-6: unknown value ist nicht gleich zero
    if (!b->state.telemetry_valid) {
        if (b->state.temperature_c == 0.0 || b->state.board_power_w == 0.0) {
            b->invariant_violations++;
            snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-6: Unknown telemetry mapped to 0.0 instead of N/A marker");
            return false;
        }
    }

    // INV-10: Queue bleibt innerhalb definierter Grenzen
    if (b->work_queue_count > SIM_QUEUE_CAPACITY) {
        b->invariant_violations++;
        snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-10: Queue size %zu exceeds capacity %d", b->work_queue_count, SIM_QUEUE_CAPACITY);
        return false;
    }

    // INV-12: State machine kann keinen ungültigen Zustand erreichen
    if (b->state.net_state >= NET_STATE_MAX) {
        b->invariant_violations++;
        snprintf(b->last_violation_desc, sizeof(b->last_violation_desc), "INV-12: Network state %d out of bounds", b->state.net_state);
        return false;
    }

    return true;
}
