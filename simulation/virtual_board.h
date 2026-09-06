#ifndef VIRTUAL_BOARD_H
#define VIRTUAL_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Canonical ASIC States (GEMINI.md Section 22)
 * ------------------------------------------------------------------------- */
typedef enum {
    ASIC_STATE_NOT_PRESENT = 0,
    ASIC_STATE_DETECTED,
    ASIC_STATE_SUPPORTED,
    ASIC_STATE_READY,
    ASIC_STATE_RUNNING,
    ASIC_STATE_FAILED,
    ASIC_STATE_UNSUPPORTED
} sim_asic_state_t;

const char *sim_asic_state_to_string(sim_asic_state_t state);

/* -------------------------------------------------------------------------
 * Explicit Network State Machine (GEMINI.md Section 12)
 * ------------------------------------------------------------------------- */
typedef enum {
    NET_STATE_BOOT = 0,
    NET_STATE_WIFI_INIT,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_CONNECTED,
    NET_STATE_DHCP,
    NET_STATE_IP_READY,
    NET_STATE_DNS_READY,
    NET_STATE_STRATUM_CONNECTING,
    NET_STATE_STRATUM_READY,
    NET_STATE_MINING,
    // Recovery states
    NET_STATE_WIFI_LOST,
    NET_STATE_FAST_RECONNECT,
    NET_STATE_DHCP_RETRY,
    NET_STATE_DNS_RETRY,
    NET_STATE_STRATUM_RECONNECT,
    NET_STATE_JOB_SYNC,
    NET_STATE_RECOVERY_VERIFY,
    NET_STATE_MAX
} sim_net_state_t;

const char *sim_net_state_to_string(sim_net_state_t state);

/* -------------------------------------------------------------------------
 * Stratum Protocol & Pool States
 * ------------------------------------------------------------------------- */
typedef enum {
    SIM_STRATUM_V1 = 0,
    SIM_STRATUM_V2
} sim_stratum_proto_t;

typedef enum {
    POOL_STATE_DISCONNECTED = 0,
    POOL_STATE_CONNECTING,
    POOL_STATE_CONNECTED,
    POOL_STATE_SUBSCRIBED,
    POOL_STATE_AUTHORIZED,
    POOL_STATE_MINING,
    POOL_STATE_FAILED,
    POOL_STATE_RECONNECTING
} sim_pool_state_t;

/* -------------------------------------------------------------------------
 * Canonical Job & Work Structures (GEMINI.md Section 15 & Phase 4)
 * ------------------------------------------------------------------------- */
#define SIM_MAX_MERKLE_BRANCHES 32
#define SIM_MAX_JOB_ID_LEN      64
#define SIM_MAX_EXTRANONCE_LEN   32
#define SIM_MAX_ACTIVE_JOBS     128
#define SIM_QUEUE_CAPACITY       64

typedef struct {
    uint32_t job_epoch;                         // Monotonically increasing job sequence ID
    char job_id[SIM_MAX_JOB_ID_LEN];            // Pool-provided job identifier
    uint8_t prev_block_hash[32];
    uint8_t coinbase_1[128];
    size_t coinbase_1_len;
    uint8_t coinbase_2[128];
    size_t coinbase_2_len;
    uint8_t merkle_branches[SIM_MAX_MERKLE_BRANCHES][32];
    size_t n_merkle_branches;
    uint32_t version;
    uint32_t nbits;
    uint32_t ntime;
    bool clean_jobs;
    double pool_difficulty;
    uint32_t version_mask;
    uint64_t created_time_us;
} sim_pool_job_t;

typedef struct {
    uint32_t work_generation;                   // Work epoch (tied to job_epoch)
    uint32_t job_epoch;
    uint8_t job_slot;                           // Index in active_jobs table [0..127]
    char job_id[SIM_MAX_JOB_ID_LEN];
    uint64_t extranonce_2;
    char extranonce_2_str[SIM_MAX_EXTRANONCE_LEN];
    uint8_t midstate[32];                       // Word-swapped BMXX midstate
    uint8_t merkle_root[32];
    uint32_t ntime;
    uint32_t target;
    uint32_t version;
    uint32_t version_mask;
    double pool_diff;
    bool is_stale;
    uint64_t dispatch_time_us;
} sim_work_item_t;

typedef struct {
    uint8_t job_slot;
    uint32_t nonce;
    uint32_t rolled_version;
    uint8_t core_id;
    uint8_t small_core_id;
    uint8_t asic_nr;
    uint8_t crc5;
    bool is_valid_crc;
    uint64_t timestamp_us;
} sim_asic_result_t;

/* -------------------------------------------------------------------------
 * Single Source of Truth / Canonical State (GEMINI.md Phase 3)
 * ------------------------------------------------------------------------- */
typedef struct {
    sim_net_state_t net_state;
    sim_pool_state_t pool_state;
    sim_asic_state_t asic_state;
    uint32_t current_job_epoch;
    uint32_t current_work_generation;
    char current_job_id[SIM_MAX_JOB_ID_LEN];
    double active_difficulty;
    uint32_t version_mask;
    
    // Telemetry - canonical (never 0 for unmeasured, labeled N/A if invalid)
    bool telemetry_valid;
    double hashrate_ghs;
    double effective_hashrate_ghs;
    double core_voltage_mv;
    double board_power_w;
    double temperature_c;
    double fan_speed_pct;
    uint32_t fan_rpm;
    uint32_t frequency_mhz;
    
    // Nonce / Share metrics
    uint64_t nonces_found;
    uint64_t shares_submitted;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint64_t shares_stale;
    uint64_t shares_duplicate_filtered;
    uint64_t duplicate_work_prevented;
} sim_canonical_state_t;

/* -------------------------------------------------------------------------
 * Virtual Board Model Architecture (GEMINI.md Phase 1)
 * ------------------------------------------------------------------------- */
typedef struct {
    // Virtual Clocks & FreeRTOS Emulation
    uint64_t sim_time_us;
    uint64_t tick_count;
    
    // Memory and Allocator Tracking (Phase 8)
    size_t internal_sram_used;
    size_t internal_sram_total;
    size_t psram_used;
    size_t psram_total;
    size_t total_allocations;
    size_t total_frees;
    size_t peak_memory_used;
    size_t memcpy_count;
    size_t hash_calc_count;
    
    // Hardware Simulation (BM1366, EMC2101, INA260, DS4432U)
    struct {
        uint16_t chip_id;                       // 0x1366
        uint8_t core_count;                     // 112
        uint8_t small_core_count;               // 8 per core (894 total active)
        uint32_t reg_0xa4_version_mask;         // 0x1FFFE000
        double clock_mhz;                       // 485.0
        bool is_powered;
        bool is_communicating;
        uint32_t error_counter;
        uint8_t uart_rx_buf[256];
        size_t uart_rx_head;
        size_t uart_rx_tail;
        uint8_t uart_tx_buf[256];
        size_t uart_tx_len;
    } virtual_bm1366;
    
    struct {
        double ext_temp_c;
        double target_temp_c;
        double fan_pwm_duty;
        uint32_t tach_rpm;
        bool sensor_fault;
    } virtual_emc2101;
    
    struct {
        double bus_voltage_mv;
        double shunt_current_ma;
        double power_w;
    } virtual_ina260;
    
    struct {
        uint8_t dac_register;
        double output_voltage_mv;
    } virtual_ds4432;
    
    // Network & DHCP & DNS Simulation
    struct {
        bool ap_available;
        bool auth_success;
        bool dhcp_server_responding;
        bool dns_server_responding;
        bool stratum_server_responding;
        uint32_t dhcp_lease_duration_sec;
        uint64_t dhcp_lease_expiry_us;
        char assigned_ip[16];
        char resolved_pool_ip[16];
        uint32_t connect_retries;
        uint32_t fast_reconnect_count;
    } virtual_network;
    
    // Work Queues & Job Tables (Phase 4, 15)
    sim_work_item_t *active_jobs[SIM_MAX_ACTIVE_JOBS];
    bool valid_jobs[SIM_MAX_ACTIVE_JOBS];
    
    // Bounded Work Queue
    sim_work_item_t work_queue[SIM_QUEUE_CAPACITY];
    size_t work_queue_head;
    size_t work_queue_tail;
    size_t work_queue_count;
    size_t work_queue_peak;
    
    // Duplicate Share Submission Filter Cache (Phase 5)
    #define SIM_DUP_CACHE_SIZE 64
    struct {
        uint32_t nonce;
        uint32_t ntime;
        uint32_t version_bits;
        char job_id[SIM_MAX_JOB_ID_LEN];
        uint64_t en2;
        bool valid;
    } dup_filter_cache[SIM_DUP_CACHE_SIZE];
    size_t dup_filter_idx;
    
    // Canonical State (Phase 3)
    sim_canonical_state_t state;
    
    // Invariant Failure Tracking (Phase 12)
    uint32_t invariant_violations;
    char last_violation_desc[256];
} sim_board_t;

/* -------------------------------------------------------------------------
 * Function Declarations
 * ------------------------------------------------------------------------- */
void sim_board_init(sim_board_t *b);
void sim_board_reset(sim_board_t *b);
void sim_advance_time(sim_board_t *b, uint64_t delta_us);

// Network state machine transitions
bool sim_net_transition(sim_board_t *b, sim_net_state_t new_state);
void sim_net_process_events(sim_board_t *b);
void sim_net_inject_fault(sim_board_t *b, const char *fault_type);

// Job planning & work scheduling
bool sim_job_receive_notify(sim_board_t *b, const sim_pool_job_t *notify);
bool sim_work_enqueue(sim_board_t *b, const sim_work_item_t *work);
bool sim_work_dequeue(sim_board_t *b, sim_work_item_t *out_work);
void sim_work_clean_stale(sim_board_t *b, uint32_t keep_job_epoch);

// Virtual BM1366 ASIC
void sim_bm1366_init(sim_board_t *b);
void sim_bm1366_send_work(sim_board_t *b, const sim_work_item_t *work);
bool sim_bm1366_poll_result(sim_board_t *b, sim_asic_result_t *out_result);
void sim_bm1366_inject_failure(sim_board_t *b, const char *failure_mode);

// Share processing & duplicate filter
bool sim_process_asic_result(sim_board_t *b, const sim_asic_result_t *res);
bool sim_is_duplicate_share(sim_board_t *b, const char *job_id, uint64_t en2, uint32_t ntime, uint32_t nonce, uint32_t ver_bits);
void sim_record_share(sim_board_t *b, const char *job_id, uint64_t en2, uint32_t ntime, uint32_t nonce, uint32_t ver_bits);

// Telemetry & Single Source of Truth
void sim_telemetry_update(sim_board_t *b);

// Invariant Validation (GEMINI.md Phase 12)
bool sim_verify_all_invariants(sim_board_t *b);

// CRC5 calculation helper (exact BM1366 polynomial)
uint8_t sim_crc5(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // VIRTUAL_BOARD_H
