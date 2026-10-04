#ifndef VF_TUNER_H
#define VF_TUNER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* NULL — <stdint.h> does not provide it on glibc/newlib */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Closed-loop voltage tuner: hold the frequency, minimise the voltage.
 *
 * This is the single largest efficiency lever available on a Bitaxe-class
 * board, and every serious firmware converges on the same shape: LuxOS
 * AutoTuner, Braiins AutoTune, ePIC PowerPlay-BMS and Vnish all search for the
 * *minimum stable voltage at a held frequency* rather than raising voltage to
 * chase frequency.
 *
 * The physics: power grows roughly with clock x voltage^2. Along a *fixed
 * voltage* line the energy per terahash improves with frequency, because fan,
 * ESP32 and conversion losses are amortised over more hashes. Moving *up* a
 * voltage line at fixed frequency makes J/TH worse, roughly quadratically.
 * So the rule is "clock first, voltage only as much as needed" — and "needed"
 * has to be measured, not guessed.
 *
 * Guessing goes measurably wrong in both directions, which is why this is a
 * closed loop and not a table:
 *   - raising voltage to raise frequency reduces hashrate, because self-heating
 *     pushes the silicon past its own frequency ceiling before the extra
 *     voltage margin pays for it (measured on BM1366: 1300 mV tops out at
 *     450 MHz / 464 GH/s while 1200 mV reaches 475 MHz / 673 GH/s);
 *   - lowering voltage too far silently starves the chip and shows up as a slow
 *     hashrate decay rather than an error, so it can go unnoticed for hours.
 *
 * Acceptance gate, matching the criteria used by the best-documented BM1366
 * tuning write-ups: the 10-minute average hashrate must stay at or above 95 % of
 * expected, and the error rate must stay at or below 0.1 %. Short spikes right
 * after a change are normal and are absorbed by the settle time.
 *
 * Like atm_policy this module is dependency-free decision logic so it can be
 * unit-tested on the host without ESP-IDF or hardware.
 */

typedef struct {
    uint16_t voltage_floor_mv;      /* regulator floor — never undercut        */
    uint16_t voltage_step_mv;

    double min_hashrate_ratio;       /* acceptance gate, e.g. 0.95             */
    double max_error_percent;        /* acceptance gate, e.g. 0.1              */
    double max_chip_temp_c;          /* abort tuning above this, e.g. 65       */
    uint32_t settle_s;              /* dwell after a change, e.g. 600         */
    uint32_t max_attempts;          /* bound the search, e.g. 8               */
} vf_config_t;

typedef struct {
    double hashrate_ratio;           /* hashrate_10m / expected                 */
    double error_percent;
    double chip_temp_c;
    uint32_t uptime_s;
} vf_inputs_t;

typedef enum {
    VF_ACTION_NONE = 0,
    VF_ACTION_LOWER_VOLTAGE,   /* candidate accepted, keep going            */
    VF_ACTION_RAISE_VOLTAGE,   /* last step was rejected, revert            */
    VF_ACTION_BASELINE_REACHED,/* already at the floor, stop searching       */
    VF_ACTION_ABORT_THERMAL,   /* too hot to keep tuning                    */
    VF_ACTION_EXHAUSTED        /* attempt budget used up                    */
} vf_action_t;

typedef struct {
    uint16_t voltage_mv;
    uint32_t pending_change_s;   /* uptime when the pending change was made */
    bool     change_pending;
    uint32_t baseline_mv;        /* highest voltage known to be stable       */
    uint32_t attempts;
    bool     finished;
} vf_state_t;

/** @brief Seed the tuner at the user's configured voltage. */
void vf_init(vf_state_t * state, uint16_t voltage_mv, uint16_t baseline_mv);

/**
 * @brief Decide the tuner's next action.
 *
 * Only acts once a change has had `cfg->settle_s` to take effect, so a single
 * call per poll is safe. Voltage is never raised above `baseline_mv`.
 */
vf_action_t vf_step(const vf_config_t * cfg, vf_state_t * state, const vf_inputs_t * in);

/** @brief Voltage the tuner wants applied right now. */
uint16_t vf_target_voltage(const vf_state_t * state);

const char *vf_action_name(vf_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* VF_TUNER_H */
