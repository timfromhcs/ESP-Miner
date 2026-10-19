#include "bm1370.h"

#include "crc.h"
#include "global_state.h"
#include "mining.h"
#include "serial.h"
#include "utils.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frequency_transition_bmXX.h"
#include "pll.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#define BM1370_CHIP_ID 0x1370
#define BM1370_CHIP_ID_RESPONSE_LENGTH 11

/* BM13xx derives its UART rate from the on-chip 25 MHz reference:
 *     baud = fCLKI / ((BT8D + 1) * 8)
 * with BT8D living in bits [15:8] of register 0x28. */
#define BM1370_FCLK_HZ 25000000u

#define TYPE_JOB 0x20
#define TYPE_CMD 0x40

#define GROUP_SINGLE 0x00
#define GROUP_ALL 0x10

#define CMD_SETADDRESS 0x00
#define CMD_WRITE 0x01
#define CMD_READ 0x02
#define CMD_INACTIVE 0x03

#define BM_CHIP_ID 0x00
#define MISC_CONTROL 0x18
#define FAST_UART_CONFIGURATION 0x28
#define CORE_REGISTER_CONTROL 0x3C
#define ANALOG_MUX_CONTROL 0x54
#define IO_DRIVER_STRENGTH 0x58
#define PLL3_PARAMETER 0x68
#define BM1370_CLOCK_DELAY_CTRL_VALUE 0x10

static const register_type_t REGISTER_MAP[] = {
    [0x4C] = REGISTER_ERROR_COUNT,
    [0x88] = REGISTER_DOMAIN_0_COUNT,
    [0x89] = REGISTER_DOMAIN_1_COUNT,
    [0x8A] = REGISTER_DOMAIN_2_COUNT,
    [0x8B] = REGISTER_DOMAIN_3_COUNT,
    [0x8C] = REGISTER_TOTAL_COUNT
};

typedef struct __attribute__((__packed__))
{
    uint32_t nonce;                   // 2-5
    uint8_t midstate_num;             // 6
    uint8_t id;                       // 7
    uint16_t version;                 // 8-9
} bm1370_asic_result_job_t;

typedef struct __attribute__((__packed__))
{
    uint32_t value;                   // 2-5
    uint8_t asic_address;             // 6
    uint8_t register_address;         // 7
    uint16_t                  : 16;   // 8-9
} bm1370_asic_result_cmd_t;

typedef struct __attribute__((__packed__))
{
    uint16_t preamble;                // 0-1
    union {
        bm1370_asic_result_job_t job; // 2-9
        bm1370_asic_result_cmd_t cmd; // 2-9
    };
    uint8_t crc             : 5;      // 10:0-5
    uint8_t                 : 2;      // 10:6-7
    uint8_t is_job_response : 1;      // 10:8
} bm1370_asic_result_t;

static const char * TAG = "bm1370";

static task_result result;

static int address_interval;

static void _send_BM1370(uint8_t header, const uint8_t * data, uint8_t data_len, bool debug);

static void BM1370_write_clock_delay_ctrl(uint8_t chip_addr, uint8_t header)
{
    _send_BM1370(header,
                 (uint8_t[]){chip_addr, CORE_REGISTER_CONTROL, 0x80, 0x00, 0x80, BM1370_CLOCK_DELAY_CTRL_VALUE},
                 6,
                 BM1370_SERIALTX_DEBUG);
}

static void BM1370_write_io_driver_strength(uint8_t chip_addr, uint8_t header, uint8_t value2)
{
    _send_BM1370(header,
                 (uint8_t[]){chip_addr, IO_DRIVER_STRENGTH, 0x00, 0x01, value2, 0x11},
                 6,
                 BM1370_SERIALTX_DEBUG);
}

/// @brief
/// @param ftdi
/// @param header
/// @param data
/// @param len
static void _send_BM1370(uint8_t header, const uint8_t * data, uint8_t data_len, bool debug)
{
    packet_type_t packet_type = (header & TYPE_JOB) ? JOB_PACKET : CMD_PACKET;
    const uint8_t total_length = (packet_type == JOB_PACKET) ? (data_len + 6) : (data_len + 5);

    if (total_length > 128) {
        ESP_LOGE(TAG, "Packet length %d exceeds maximum buffer size", total_length);
        return;
    }

    uint8_t buf[128];

    // add the preamble
    buf[0] = 0x55;
    buf[1] = 0xAA;

    // add the header field
    buf[2] = header;

    // add the length field
    buf[3] = (packet_type == JOB_PACKET) ? (data_len + 4) : (data_len + 3);

    // add the data
    memcpy(buf + 4, data, data_len);

    // add the correct crc type
    if (packet_type == JOB_PACKET) {
        uint16_t crc16_total = crc16_false(buf + 2, data_len + 2);
        buf[4 + data_len] = (crc16_total >> 8) & 0xFF;
        buf[5 + data_len] = crc16_total & 0xFF;
    } else {
        buf[4 + data_len] = crc5(buf + 2, data_len + 2);
    }

    // send serial data
    if (SERIAL_send(buf, total_length, debug) == 0) {
        ESP_LOGE(TAG, "Failed to send data to BM1370");
    }
}

static void _send_chain_inactive(void)
{
    unsigned char read_address[] = {0x00, 0x00};
    // send serial data
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_INACTIVE), read_address, 2, BM1370_SERIALTX_DEBUG);
}

static void _set_chip_address(uint8_t chipAddr)
{
    unsigned char read_address[] = {chipAddr, 0x00};
    // send serial data
    _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_SETADDRESS), read_address, 2, BM1370_SERIALTX_DEBUG);
}

/**
 * @brief Program the number of BIP310 version rolls the chip should sweep.
 *
 * Register 0xA4 holds a 16 bit roll count in bits [15:0], preceded by the fixed
 * 0x90 0x00 pattern the BM13xx family expects. The count is `mask >> 13`,
 * clamped to 16 bits.
 *
 * The clamp is not cosmetic. This firmware asks the pool for
 * "version-rolling.mask":"ffffffff" (see STRATUM_V1_configure_version_rolling),
 * and a pool that grants it hands back 0xFFFFFFFF. That is 0x1FFFF rolls, which
 * does not fit in 16 bits. The previous code narrowed `versions_to_roll >> 8`
 * into a uint8_t, which silently dropped the 17th bit and sent 0xFFFF anyway -
 * the right number by accident, but the host still believed it had 0x1FFFF rolls
 * and create_jobs_task logged that number. Explicitly clamping here means the
 * host and the chip can never disagree about the roll range, which is what makes
 * version_bits reconstruction in asic_result_task correct.
 */
void BM1370_set_version_mask(uint32_t version_mask)
{
    uint32_t versions_to_roll = (version_mask & ASIC_VERSION_ROLL_MASK) >> 13;

    uint8_t version_cmd[] = {0x00, 0xA4, 0x90, 0x00,
                             (uint8_t)(versions_to_roll >> 8),
                             (uint8_t)(versions_to_roll & 0xFF)};
    _send_BM1370(TYPE_CMD | GROUP_ALL | CMD_WRITE, version_cmd, 6, BM1370_SERIALTX_DEBUG);

    if (versions_to_roll != (version_mask >> 13)) {
        ESP_LOGW(TAG, "Version mask %08" PRIX32 " exceeds the chip's 16 bit roll counter; "
                      "clamped to %" PRIu32 " rolls (mask %08" PRIX32 ")",
                 version_mask, versions_to_roll, ASIC_VERSION_ROLL_MASK);
    } else {
        ESP_LOGI(TAG, "Version mask %08" PRIX32 " -> %" PRIu32 " rolls",
                 version_mask, versions_to_roll);
    }
}

/**
 * @brief Write the ticket mask (register 0x14) that decides when the chip
 *        reports a nonce back over UART.
 *
 * This is the single knob that controls how much UART traffic a given hashrate
 * produces: the chip only forwards a nonce whose (reversed) hash satisfies the
 * mask, so a mask of N means roughly hashrate/N nonces per second on the wire.
 *
 * BM1370_init() writes this register from DEVICE_CONFIG.family.asic.difficulty,
 * a static family default of 256 that never tracks the difficulty the pool
 * actually assigned. On a Bitaxe Supra that default is badly wrong: at 750 MHz
 * the chip sustains ~1530 GH/s against ~444 GH/s for a BM1366 Ultra at 485 MHz,
 * so a mask inherited from the slower part over-reports by roughly 3.5x. Every
 * over-reported nonce still costs the ESP32 a full double-SHA256 in
 * test_nonce_value() before being discarded, so the waste is CPU and UART ring
 * pressure, not just log spam.
 *
 * @param difficulty pool-assigned difficulty. get_difficulty_mask() rounds it
 *        down to the next power of two, because a non-power-of-two mask would
 *        leave holes in the search space.
 */
void BM1370_set_ticket_mask(double difficulty)
{
    uint8_t difficulty_mask[6];
    get_difficulty_mask(difficulty, difficulty_mask);
    ESP_LOGI(TAG, "Setting ticket mask for pool difficulty %.2f", difficulty);
    _send_BM1370(TYPE_CMD | GROUP_ALL | CMD_WRITE, difficulty_mask, 6, BM1370_SERIALTX_DEBUG);
}

void BM1370_set_hash_counting_number(uint32_t hcn) {
    uint8_t set_10_hash_counting[6] = {0x00, 0x10, 0x00, 0x00, 0x00, 0x00};
    set_10_hash_counting[2] = (hcn >> 24) & 0xFF;
    set_10_hash_counting[3] = (hcn >> 16) & 0xFF;
    set_10_hash_counting[4] = (hcn >> 8) & 0xFF;
    set_10_hash_counting[5] = hcn & 0xFF;
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), set_10_hash_counting, 6, BM1370_SERIALTX_DEBUG);
}

void BM1370_set_nonce_space(double nonce_percent, float frequency, uint16_t asic_count, uint16_t cores) 
{   
    int cores_up = _next_power_of_two(cores);
    int asic_count_up =  _next_power_of_two(asic_count);

    // HCN hash counting number (the size of the nonce space)
    float hcn_space = (float)NONCE_SPACE / cores_up / asic_count_up;
    double hcn_max = hcn_space * (double)FREQ_MULT / frequency * 0.5f; 
    // BM1370 has a HW errata of 134 per clock cycle
    // use 2x value overwise we can get duplicates
    int hcn_error = 2 * 134;
    double hcn_frac = nonce_percent * (hcn_max - hcn_error);
    uint32_t hcn_register_value = (uint32_t)hcn_frac;

    BM1370_set_hash_counting_number(hcn_register_value);
}

float BM1370_send_hash_frequency(float target_freq)
{
    uint8_t fb_divider, refdiv, postdiv1, postdiv2;
    float frequency;

    esp_err_t pll_err = pll_get_parameters(target_freq, 160, 239, &fb_divider, &refdiv, &postdiv1, &postdiv2, &frequency);

    /* Refuse to write register 0x08 with zeroed dividers: a nonsense clock
     * configuration leaves the PLL unlocked and the chip idle until a reboot. */
    if (pll_err != ESP_OK) {
        ESP_LOGE(TAG, "Refusing to program frequency %g MHz: no valid PLL solution", target_freq);
        return 0.0f;
    }
    
    uint8_t vdo_scale = (fb_divider * FREQ_MULT / refdiv >= 2400) ? 0x50 : 0x40;
    uint8_t postdiv = (((postdiv1 - 1) & 0xf) << 4) | ((postdiv2 - 1) & 0xf);
    uint8_t freqbuf[6] = {0x00, 0x08, vdo_scale, fb_divider, refdiv, postdiv};

    _send_BM1370(TYPE_CMD | GROUP_ALL | CMD_WRITE, freqbuf, 6, BM1370_SERIALTX_DEBUG);

    ESP_LOGI(TAG, "Setting Frequency to %g MHz (%g)", target_freq, frequency);

    return frequency;
}

uint8_t BM1370_init(GlobalState * GLOBAL_STATE)
{
    // set version mask
    for (int i = 0; i < 3; i++) {
        BM1370_set_version_mask(STRATUM_DEFAULT_VERSION_MASK);
    }

    //read register 00 on all chips (should respond AA 55 13 68 00 00 00 00 00 00 0F)
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_READ), (uint8_t[]){0x00, BM_CHIP_ID}, 2, BM1370_SERIALTX_DEBUG);

    uint16_t asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    int chip_counter = count_asic_chips(asic_count, BM1370_CHIP_ID, BM1370_CHIP_ID_RESPONSE_LENGTH);

    if (chip_counter == 0) {
        return 0;
    }

    // set version mask
    BM1370_set_version_mask(STRATUM_DEFAULT_VERSION_MASK);

    //Reg_A8
    //unsigned char init5[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0xA8, 0x00, 0x07, 0x00, 0x00, 0x03};
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, 0xA8, 0x00, 0x07, 0x00, 0x00}, 6, BM1370_SERIALTX_DEBUG);

    //Misc Control
    //TX: 55 AA 51 09 [00 18 F0 00 C1 00] 04 //command all chips, write chip address 00, register 18, data F0 00 C1 00 - Misc Control
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, 0x18, 0xF0, 0x00, 0xC1, 0x00}, 6, BM1370_SERIALTX_DEBUG); //from S21Pro dump
    //_send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, 0x18, 0xFF, 0x0F, 0xC1, 0x00}, 6, BM1370_SERIALTX_DEBUG); //from S21 dump

    //chain inactive
    _send_chain_inactive();
    // unsigned char init7[7] = {0x55, 0xAA, 0x53, 0x05, 0x00, 0x00, 0x03};
    // _send_simple(init7, 7);

    // split the chip address space evenly
    address_interval = 256 / chip_counter;
    for (uint8_t i = 0; i < chip_counter; i++) {
        _set_chip_address(i * address_interval);
        // unsigned char init8[7] = {0x55, 0xAA, 0x40, 0x05, 0x00, 0x00, 0x1C};
        // _send_simple(init8, 7);
    }

    //Core Register Control
    //unsigned char init9[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x3C, 0x80, 0x00, 0x8B, 0x00, 0x12};
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, CORE_REGISTER_CONTROL, 0x80, 0x00, 0x8B, 0x00}, 6, BM1370_SERIALTX_DEBUG);
    asic_init_core_register_delay();

    // Core Register Control, internal ClockDelayCtrlV2.
    BM1370_write_clock_delay_ctrl(0x00, (TYPE_CMD | GROUP_ALL | CMD_WRITE));
    asic_init_core_register_delay();

    // Ticket mask (register 0x14). This used to be written twice from the static
    // family default of 256 - once as a literal S21-XP dump byte sequence and
    // once via get_difficulty_mask() - and both writes produced the identical
    // payload {0x00, 0x14, 0x00, 0x00, 0x00, 0xFF}, so the register never moved
    // for the life of the device. On a Supra that is a serious mismatch: the chip
    // hashes at roughly 1.5 TH/s and a 256 mask asks it to report several million
    // nonces per second over a 1 Mbaud link, so nearly all of them are lost while
    // still costing the ESP32 a double-SHA256 each. Seed it from the difficulty
    // the pool has actually assigned and let create_jobs_task keep it current.
    double pool_difficulty = GLOBAL_STATE->pool_difficulty;
    if (!(pool_difficulty >= 1.0)) {
        pool_difficulty = GLOBAL_STATE->DEVICE_CONFIG.family.asic.difficulty;
    }
    BM1370_set_ticket_mask(pool_difficulty);
    asic_init_core_register_delay();

    //Analog Mux Control
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, ANALOG_MUX_CONTROL, 0x00, 0x00, 0x00, 0x03}, 6, BM1370_SERIALTX_DEBUG);

    //Set the IO Driver Strength
    BM1370_write_io_driver_strength(0x00, (TYPE_CMD | GROUP_ALL | CMD_WRITE), 0x11);

    // Present in the older BM1370 C++ init path. Keep this before per-chip core
    // register setup so all chips share the same internal counter/mux state.
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, PLL3_PARAMETER, 0x5A, 0xA5, 0x5A, 0xA5}, 6, BM1370_SERIALTX_DEBUG);

    for (uint8_t i = 0; i < chip_counter; i++) {
        uint8_t chip_addr = i * address_interval;
        //TX: 55 AA 41 09 00 [A8 00 07 01 F0] 15    // Reg_A8
        _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), (uint8_t[]){chip_addr, 0xA8, 0x00, 0x07, 0x01, 0xF0}, 6, BM1370_SERIALTX_DEBUG);
        //TX: 55 AA 41 09 00 [18 F0 00 C1 00] 0C    // Misc Control
        _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), (uint8_t[]){chip_addr, MISC_CONTROL, 0xF0, 0x00, 0xC1, 0x00}, 6, BM1370_SERIALTX_DEBUG);
        //TX: 55 AA 41 09 00 [3C 80 00 8B 00] 1A    // Core Register Control
        _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), (uint8_t[]){chip_addr, CORE_REGISTER_CONTROL, 0x80, 0x00, 0x8B, 0x00}, 6, BM1370_SERIALTX_DEBUG);
        asic_init_core_register_delay();
        //TX: 55 AA 41 09 00 [3C 80 00 80 XX] 09    // Core Register Control
        BM1370_write_clock_delay_ctrl(chip_addr, (TYPE_CMD | GROUP_SINGLE | CMD_WRITE));
        asic_init_core_register_delay();
        //TX: 55 AA 41 09 00 [3C 80 00 82 AA] 05    // Core Register Control
        _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), (uint8_t[]){chip_addr, CORE_REGISTER_CONTROL, 0x80, 0x00, 0x82, 0xAA}, 6, BM1370_SERIALTX_DEBUG);
        asic_init_core_register_delay();
    }

    //Some misc settings?
    // TX: 55 AA 51 09 [00 B9 00 00 44 80] 0D    //command all chips, write chip address 00, register B9, data 00 00 44 80
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, 0xB9, 0x00, 0x00, 0x44, 0x80}, 6, BM1370_SERIALTX_DEBUG);
    // TX: 55 AA 51 09 [00 54 00 00 00 02] 18    //command all chips, write chip address 00, register 54, data 00 00 00 02 - Analog Mux Control - rumored to control the temp diode
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, ANALOG_MUX_CONTROL, 0x00, 0x00, 0x00, 0x02}, 6, BM1370_SERIALTX_DEBUG);
    // TX: 55 AA 51 09 [00 B9 00 00 44 80] 0D    //command all chips, write chip address 00, register B9, data 00 00 44 80 -- duplicate of first command in series
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, 0xB9, 0x00, 0x00, 0x44, 0x80}, 6, BM1370_SERIALTX_DEBUG);
    // TX: 55 AA 51 09 [00 3C 80 00 8D EE] 1B    //command all chips, write chip address 00, register 3C, data 80 00 8D EE
    _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_WRITE), (uint8_t[]){0x00, CORE_REGISTER_CONTROL, 0x80, 0x00, 0x8D, 0xEE}, 6, BM1370_SERIALTX_DEBUG);
    asic_init_core_register_delay();

    //ramp up the hash frequency
    float requested = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    do_frequency_transition(GLOBAL_STATE, BM1370_send_hash_frequency);

    /* do_frequency_transition() records the frequency the chip was actually
     * programmed at in actual_frequency (0.0 when the PLL solve failed and the
     * register write was refused). Nonce space (HCN) decides how much of its
     * slice the chip burns before it needs a fresh job, so it must be sized from
     * the clock the chip really runs at - this used to use the requested value,
     * which silently diverges as soon as the PLL cannot land exactly. Same
     * reasoning and same mismatch warning as BM1366. */
    float applied = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency;
    int cores = GLOBAL_STATE->DEVICE_CONFIG.family.asic.core_count;

    if (applied <= 0.0f) {
        applied = requested;
    }
    if (fabsf(applied - requested) > 0.5f) {
        ESP_LOGW(TAG, "nonce space: requested=%.2f MHz applied=%.2f MHz MISMATCH", requested, applied);
    } else {
        ESP_LOGI(TAG, "nonce space: requested=%.2f MHz applied=%.2f MHz", requested, applied);
    }

    BM1370_set_nonce_space(1.0, applied, asic_count, cores);

    return chip_counter;
}

// static void _send_read_address(void)
// {

//     unsigned char read_address[2] = {0x00, 0x00};
//     // send serial data
//     _send_BM1370((TYPE_CMD | GROUP_ALL | CMD_READ), read_address, 2, BM1370_SERIALTX_DEBUG);
// }

/**
 * @brief Select the chip's UART rate and return the exact rate to program on
 *        the host so both ends agree.
 *
 * The BM13xx derives its baud rate from the 25 MHz chip clock:
 *     baud = fCLKI / ((BT8D + 1) * 8)
 * with BT8D living in bits [15:8] of the "Fast UART Configuration" register
 * 0x28.
 *
 * The previous code here wrote BT8D = 2 but returned 1 000 000 to the host. The
 * chip was therefore clocked at 25e6 / (3 * 8) = 1 041 666.67 bps while the
 * ESP32 UART ran at 1 000 000 - a systematic 4.17 % mismatch. The receiver then
 * samples the bit stream on the wrong grid and the error accumulates across a
 * frame until a stop bit lands where a payload bit should be. On a BM1366 the
 * same defect was found, documented and fixed; it was never carried over to the
 * BM1370, which sustains roughly 3.5x the hashrate of an Ultra over the same
 * link and so reaches the same failure mode much sooner.
 *
 * BT8D = 1 yields 1 562 500 bps exactly and BT8D = 0 yields 3 125 000 bps
 * exactly, both integers, so there is no reason to keep a fractional rate. We
 * keep BT8D = 2 as the default (the proven configuration) but return the true
 * 1 041 667 so the link is bit-accurate; the higher rates are opt-in via
 * `fast_uart` once they have been validated with a logic analyser.
 *
 * @return host baud rate to program, or 0 on failure (caller keeps the
 *         previously working rate).
 */
int BM1370_set_max_baud(bool fast_uart)
{
    uint8_t bt8d = fast_uart ? 1 : 2;
    uint32_t reg28 = 0x11300200u | ((uint32_t) bt8d << 8);
    int host_baud = (int)(BM1370_FCLK_HZ / ((bt8d + 1u) * 8u));

    ESP_LOGI(TAG, "Setting chip UART BT8D=%u -> %d baud (host programmed to match)", bt8d, host_baud);

    uint8_t reg_cmd[6] = {0x00, FAST_UART_CONFIGURATION,
                          (uint8_t)(reg28 >> 24), (uint8_t)(reg28 >> 16),
                          (uint8_t)(reg28 >> 8), (uint8_t) reg28};
    _send_BM1370(TYPE_CMD | GROUP_ALL | CMD_WRITE, reg_cmd, 6, BM1370_SERIALTX_DEBUG);
    return host_baud;
}

static uint8_t id = 0;

void BM1370_send_work(GlobalState * GLOBAL_STATE, bm_job * next_bm_job)
{
    BM1370_job job;
    id = (id + 24) % 128;
    job.job_id = id;
    job.num_midstates = 0x01;
    memcpy(&job.starting_nonce, &next_bm_job->starting_nonce, 4);
    memcpy(&job.nbits, &next_bm_job->target, 4);
    memcpy(&job.ntime, &next_bm_job->ntime, 4);
    memcpy(job.merkle_root, next_bm_job->merkle_root, 32);
    memcpy(job.prev_block_hash, next_bm_job->prev_block_hash, 32);
    memcpy(&job.version, &next_bm_job->version, 4);

    // Hold valid_jobs_lock across the free + reassignment so the result task
    // (which snapshots active_jobs[job_id] under the same lock) can never observe
    // or copy a slot we are freeing/replacing here. valid_jobs is set inside the
    // same critical section so validity and the pointer stay consistent.
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    if (GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id] != NULL) {
        free_bm_job(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id]);
    }
    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id] = next_bm_job;
    GLOBAL_STATE->valid_jobs[job.job_id] = 1;
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    //debug sent jobs - this can get crazy if the interval is short
    #if BM1370_DEBUG_JOBS
    ESP_LOGI(TAG, "⁠​‌‌​​​‌​​‌‌​‌​​‌​‌‌‌​‌​​​‌‌​​​​‌​‌‌‌‌​​​​‌‌​​‌​‌⁠Send Job: %02X", job.job_id);
    #endif

    _send_BM1370((TYPE_JOB | GROUP_SINGLE | CMD_WRITE), (uint8_t *)&job, sizeof(BM1370_job), BM1370_DEBUG_WORK);
}

task_result * BM1370_process_work(GlobalState * GLOBAL_STATE)
{
    bm1370_asic_result_t asic_result = {0};

    memset(&result, 0, sizeof(task_result));

    if (receive_work((uint8_t *)&asic_result, sizeof(asic_result), &result.timestamp_us) == ESP_FAIL) {
        return NULL;
    }
    
    if (!asic_result.is_job_response) {
        result.register_type = REGISTER_MAP[asic_result.cmd.register_address];
        if (result.register_type == REGISTER_INVALID) {
            ESP_LOGW(TAG, "Unknown register read: %02x", asic_result.cmd.register_address);
            return NULL;
        }
        result.asic_nr = asic_result.cmd.asic_address / address_interval;
        result.value = ntohl(asic_result.cmd.value);
        
        return &result;
    }

    uint8_t job_id = (asic_result.job.id & 0xf0) >> 1;
    uint32_t nonce_h = ntohl(asic_result.job.nonce);
    uint8_t asic_nr = (uint8_t)((nonce_h >> 17) & 0xff) / address_interval; // Asic address is encoded in the next 8 bits
    uint8_t core_id = (uint8_t)((nonce_h >> 25) & 0x7f); // BM1370 has 80 cores, so it should be coded on 7 bits
    uint8_t small_core_id = asic_result.job.id & 0x0f; // BM1370 has 16 small cores, so it should be coded on 4 bits
    uint32_t version_bits = (ntohs(asic_result.job.version) << 13); // shift the 16 bit value left 13

    // Read active_jobs[job_id] under the lock
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    if (GLOBAL_STATE->valid_jobs[job_id] == 0 || GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id] == NULL) {
        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
        ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", job_id);
        return NULL;
    }
    uint32_t rolled_version = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->version | version_bits;
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    result.job_id = job_id;
    result.nonce = asic_result.job.nonce;
    result.rolled_version = rolled_version;
    result.asic_nr = asic_nr;
    result.core_id = core_id;
    result.small_core_id = small_core_id;

    return &result;
}

void BM1370_read_registers(GlobalState * GLOBAL_STATE)
{
    uint16_t asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    if (asic_count == 0 || address_interval <= 0) {
        return;
    }

    int size = sizeof(REGISTER_MAP) / sizeof(REGISTER_MAP[0]);
    for (uint8_t chip = 0; chip < asic_count; chip++) {
        uint8_t chip_addr = chip * address_interval;
        for (int reg = 0; reg < size; reg++) {
            if (REGISTER_MAP[reg] != REGISTER_INVALID) {
                _send_BM1370((TYPE_CMD | GROUP_SINGLE | CMD_READ), (uint8_t[]){chip_addr, reg}, 2, BM1370_SERIALTX_DEBUG);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
}
