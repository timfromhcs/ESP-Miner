#ifndef BM1370_H_
#define BM1370_H_

#include "asic_common.h"

typedef struct GlobalState GlobalState;
typedef struct bm_job bm_job;

#define BM1370_SERIALTX_DEBUG false
#define BM1370_SERIALRX_DEBUG false
#define BM1370_DEBUG_WORK false //causes insane amount of debug output
#define BM1370_DEBUG_JOBS false //causes insane amount of debug output

typedef struct __attribute__((__packed__))
{
    uint8_t job_id;
    uint8_t num_midstates;
    uint8_t starting_nonce[4];
    uint8_t nbits[4];
    uint8_t ntime[4];
    uint8_t merkle_root[32];
    uint8_t prev_block_hash[32];
    uint8_t version[4];
} BM1370_job;

uint8_t BM1370_init(GlobalState * GLOBAL_STATE);
void BM1370_send_work(GlobalState * GLOBAL_STATE, bm_job * next_bm_job);
void BM1370_set_version_mask(uint32_t version_mask);
void BM1370_set_ticket_mask(double difficulty);
/** Program the chip UART rate and return the exact rate the host must use.
 *  @param fast_uart false -> BT8D=2 (1 041 667 baud, proven), true -> BT8D=1
 *         (1 562 500 baud). Both are exact multiples of the 25 MHz chip clock. */
int BM1370_set_max_baud(bool fast_uart);
float BM1370_send_hash_frequency(float frequency);
task_result * BM1370_process_work(GlobalState * GLOBAL_STATE);
void BM1370_read_registers(GlobalState * GLOBAL_STATE);
void BM1370_set_nonce_space(double nonce_percent, float frequency, uint16_t asic_count, uint16_t cores);

#endif /* BM1370_H_ */
