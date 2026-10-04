#ifndef GLOBAL_STATE_H_
#define GLOBAL_STATE_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "power_management_task.h"
#include "hashrate_monitor_task.h"
#include "coinbase_decoder.h"
#include "work_queue.h"
#include "device_config.h"
#include "display.h"
#include "scoreboard.h"
#include "esp_transport.h"
#include "system.h"

typedef struct bm_job bm_job;
typedef struct sv2_conn sv2_conn;
typedef struct sv2_noise_ctx sv2_noise_ctx;

#define STRATUM_USER CONFIG_STRATUM_USER
#define FALLBACK_STRATUM_USER CONFIG_FALLBACK_STRATUM_USER

typedef struct PoolConfig
{
    char * url;
    uint16_t port;
    char * user;
    char * pass;
    stratum_protocol_t protocol;
    uint16_t difficulty;
    bool extranonce_subscribe;
    uint16_t tls;
    char * cert;
    bool decode_coinbase_tx;
    uint16_t sv2_channel_type;
    char * sv2_authority_pubkey;
    bool sv2_require_auth;
} PoolConfig;

#define HISTORY_LENGTH 100
#define DIFF_STRING_SIZE 10
#define MAX_BLOCK_SIGNALS 8
#define MAX_BLOCK_SIGNAL_LEN 16
#define MAX_POOLS 8

// Job id slots tracked per ASIC. Job ids are 7-bit (0..127) on all supported
// BM13xx parts, so both the active_jobs and valid_jobs tables are sized to 128.
#define MAX_ASIC_JOBS 128

typedef struct RejectedReasonStat
{
    char message[64];
    uint32_t count;
} RejectedReasonStat;

typedef struct {
    const esp_partition_t *part;
    char version[32];
    char compileDate[16];
    char compileTime[16];
    int usagePercent;
    bool isCurrent;
} cached_partition_t;

typedef struct SystemModule
{
    float current_hashrate;
    float hashrate_1m;
    float hashrate_10m;
    float hashrate_1h;
    float error_percentage;
    int64_t start_time_us;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint16_t shares_pending;
    uint64_t work_received;
    RejectedReasonStat rejected_reason_stats[10];
    int rejected_reason_stats_count;
    int screen_page;
    uint64_t best_nonce_diff;
    char best_diff_string[DIFF_STRING_SIZE];
    uint64_t best_session_nonce_diff;
    char best_session_diff_string[DIFF_STRING_SIZE];
    int block_found;
    bool show_new_block;
    char * ssid;
    char wifi_status[256];
    char ip_addr_str[16]; // IP4ADDR_STRLEN_MAX
    char ipv6_addr_str[64]; // IPv6 address string with zone identifier (INET6_ADDRSTRLEN=46 + % + interface=15)
    char ap_ssid[12];
    bool ap_enabled;
    bool is_connected;
    int identify_mode_time_ms;
    PoolConfig pools[MAX_POOLS];
    uint16_t primary_pool_index;
    uint16_t secondary_pool_index;
    bool use_fallback_stratum;
    bool is_using_fallback;
    float response_time;
    uint16_t response_share_batch;
    float process_time;
    float cpu_usage;
    char pool_connection_info[64];
    bool overheat_mode;
    bool mining_paused;
    bool pools_unavailable;
    uint16_t power_fault;
    uint32_t lastClockSync;
    bool is_screen_active;
    bool is_firmware_update;
    char firmware_update_filename[20];
    char firmware_update_status[20];
    bool hardware_fault;
    char hardware_fault_msg[64];
    const char * asic_status;
    char * version;
    char * axeOSVersion;
    Scoreboard scoreboard;
    uint64_t uptime_seconds;
    cached_partition_t cached_partitions[3];
    int cached_partitions_count;
    char mdns_hostname[64];
    char full_hostname[70];
} SystemModule;

typedef struct SelfTestNonceMeasurement
{
    bool is_active;
    uint64_t accepted_count;
    uint64_t rejected_count;
    double hashes;
    pthread_mutex_t lock;
} SelfTestNonceMeasurement;

typedef struct SelfTestModule
{
    bool is_active;
    bool is_factory;
    bool is_finished;
    SelfTestNonceMeasurement nonce_measurement;
    const char *message;
    char *result;
    char *finished;
    esp_err_t system_init_ret;
} SelfTestModule;

typedef struct AsicTaskModule
{
    // ASIC may not return the nonce in the same order as the jobs were sent
    // it also may return a previous nonce under some circumstances
    // so we keep a list of jobs indexed by the job id
    bm_job **active_jobs;
    // Current job to be processed (replaces ASIC_jobs_queue)
    bm_job *current_job;
    //semaphone
    SemaphoreHandle_t semaphore;
} AsicTaskModule;

typedef struct GlobalState
{
    work_queue stratum_queue;

    SystemModule SYSTEM_MODULE;
    DeviceConfig DEVICE_CONFIG;
    DisplayConfig DISPLAY_CONFIG;
    AsicTaskModule ASIC_TASK_MODULE;
    PowerManagementModule POWER_MANAGEMENT_MODULE;
    SelfTestModule SELF_TEST_MODULE;
    HashrateMonitorModule HASHRATE_MONITOR_MODULE;

    char * extranonce_str;
    int extranonce_2_len;

    uint8_t * valid_jobs;
    pthread_mutex_t valid_jobs_lock;

    double pool_difficulty;
    bool new_set_mining_difficulty_msg;
    uint32_t version_mask;
    bool new_stratum_version_rolling_msg;
    bool reset_extranonce2;

    esp_transport_handle_t transport;
    portMUX_TYPE stratum_mux;
    
    // A message ID that must be unique per request that expects a response.
    // For requests not expecting a response (called notifications), this is null.
    int send_uid;

    stratum_protocol_t stratum_protocol;
    struct sv2_conn *sv2_conn;
    struct sv2_noise_ctx *sv2_noise_ctx;

    bool ASIC_initalized;
    bool psram_is_available;
    bool filesystem_is_available;

    int block_height;
    char scriptsig[128];
    coinbase_output_t coinbase_outputs[MAX_COINBASE_TX_OUTPUTS];
    int coinbase_output_count;
    uint64_t coinbase_value_total_satoshis;
    uint64_t coinbase_value_user_satoshis;
    uint64_t network_nonce_diff;
    char network_diff_string[DIFF_STRING_SIZE];
    char block_signals[MAX_BLOCK_SIGNALS][MAX_BLOCK_SIGNAL_LEN];
    int block_signals_count;

    /* Stratum notify health, see system_api_json.c / openapi.yaml.
     * notify_dropped counts frames that were well-formed JSON but could not be
     * turned into valid chip work — every one of them was a full job interval
     * of wasted hashrate before P0 dropped them. */
    uint32_t notify_dropped;
    uint32_t notify_received;
    /* Why notifies were refused: [0]=none [1]=field validation [2]=undecodable
     * coinbase [3]=out of memory. Without this split "N dropped" is not
     * actionable — a pool protocol quirk and our own decoder bug need opposite
     * responses. Indexed by notify_drop_reason_t in stratum_v1_task.c. */
    uint32_t notify_drop_reason[4];
    uint32_t share_rejected_stale;   /* pool error 21 "Invalid job id"/"Stale" */
    uint32_t share_rejected_other;

    /* Set when a notify was refused, so the next dequeued work item must be
     * programmed into the ASIC regardless of its clean_jobs flag. Without this
     * the ASIC keeps hashing a job_id the pool retired, and every share in that
     * window is rejected as stale. See docs/PLAN_V2_17_STALE_AND_LIMITS.md §1.2. */
    bool force_clean_pending;

    /* Opt-in higher chip UART rate (BM1366: BT8D=1, 1 562 500 baud).
     * Off by default because the default rate is the proven configuration; the
     * reason the flag exists at all is that the previous code ran the host at
     * 1 000 000 while the chip was clocked at 1 041 667 (4.17 % mismatch). */
    bool asic_fast_uart;
} GlobalState;

#endif /* GLOBAL_STATE_H_ */
