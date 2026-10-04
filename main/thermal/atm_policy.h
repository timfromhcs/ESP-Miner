#ifndef ATM_POLICY_H
#define ATM_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Advanced Thermal Management policy — dependency-free decision logic.
 *
 * The previous implementation was a two-state latch: cross a threshold and the
 * miner halts hashing, cools for >= 30 s, then restarts a full 100 MHz /
 * 100 mV lower *and persists that reduction permanently*, so every further
 * trigger lowers it again. Three concrete defects:
 *
 *  1. 1000 mV - 100 mV = 900 mV is below the TPS546/TPS40305 operating floor on
 *     some boards, which latches a bogus "Power Fault" with a perfectly healthy
 *     PSU (fixed upstream only in the unreleased PR #1790).
 *  2. The reduction is written to NVS, so a transient thermal event permanently
 *     costs hashrate.
 *  3. There is no anti-hunting memory, so the controller can oscillate around
 *     the threshold and shed a full step on every dip.
 *
 * This module implements the design that LuxOS (Advanced Thermal Management),
 * Braiins OS (Dynamic Performance Scaling) and Vnish all converged on:
 *
 *   - an explicit hysteresis buffer, so the controller only steps *up* when
 *     there is real headroom instead of creeping towards the threshold;
 *   - frequency and voltage always move *together* and in the same direction,
 *     because raising voltage while lowering frequency is what pushes a BM1366
 *     past its own frequency ceiling (at 1300 mV the best stable frequency on
 *     the chip drops from 475 to 450 MHz);
 *   - a voltage floor tied to the regulator, not to the current setting;
 *   - anti-hunting memory: after a downshift, stepping back up requires extra
 *     margin (Braiins' "DPS Memory");
 *   - a minimum hold time after every change, so a settling transient is never
 *     mistaken for steady state.
 *
 * The hard shutdown path stays in power_management_task.c as the last-resort
 * backstop; this module runs *before* it and should keep it from ever firing.
 */

typedef struct {
    float freq_min_mhz;      /* absolute floor, usually the OC minimum        */
    float freq_ceiling_mhz;  /* the user's configured frequency — never exceed */
    float freq_step_mhz;     /* one step, e.g. 25                              */
    uint16_t voltage_floor_mv;  /* regulator floor, e.g. 1000 — never undercut */
    uint16_t voltage_ceiling_mv; /* user's configured voltage                   */
    uint16_t voltage_step_mv; /* one step, e.g. 25                             */

    double hot_temp_c;       /* step down at/above, e.g. 68                   */
    double shutdown_temp_c;  /* hard backstop, e.g. 75                       */
    double buffer_temp_c;     /* hysteresis band, e.g. 8                      */

    double vr_hot_temp_c;    /* TPS546 throttle, e.g. 100                    */
    double vr_shutdown_temp_c;

    /* How much better the fan duty cycle must be, relative to the moment we
     * last derated, before that alone justifies undoing the derate without
     * waiting for the full extra temperature margin. 0 disables the shortcut. */
    double fan_relief_percent;

    /* Health gate: below this ratio of expected hashrate (or above this error
     * rate) the silicon is not stable at the current setting, so voltage must
     * go back up instead of down. See vf_tuner.h for the same criteria applied
     * to the proactive case. */
    double min_hashrate_ratio;
    double max_error_percent;

    uint32_t min_hold_s;     /* minimum time between changes, e.g. 90        */
    uint32_t recheck_margin_c;/* extra °C required to undo a downshift        */
} atm_config_t;

typedef struct {
    double chip_temp_c;      /* hottest available ASIC sensor                 */
    double vr_temp_c;
    double fan_percent;
    double hashrate_ratio;   /* measured / expected                           */
    double error_percent;
    uint32_t uptime_s;
} atm_inputs_t;

typedef enum {
    ATM_ACTION_NONE = 0,
    ATM_ACTION_STEP_DOWN,
    ATM_ACTION_STEP_UP,
    ATM_ACTION_SHUTDOWN,     /* hand over to the hard backstop                */
    ATM_ACTION_FLOOR_REACHED /* cannot reduce any further                     */
} atm_action_t;

typedef struct {
    float frequency_mhz;
    uint16_t voltage_mv;
    uint32_t last_change_s;
    bool ever_downscaled;        /* anti-hunting memory                     */
    float downscale_fan_percent; /* fan duty at the moment we derated        */
    bool shutdown_requested;
} atm_state_t;

/** @brief Initialise the policy state from the user's configured settings. */
void atm_init(atm_state_t * state, float frequency_mhz, uint16_t voltage_mv);

/**
 * @brief Clear a latched shutdown request.
 *
 * atm_step() latches on a critical reading so that a single dip cannot resume
 * hashing. The latch is sticky by design and MUST be cleared explicitly once
 * the caller has actually completed a cooldown and restarted the ASIC —
 * otherwise one thermal excursion disables the thermal policy for the whole
 * remaining uptime of the task.
 */
void atm_clear_shutdown(atm_state_t * state);

/**
 * @brief Re-pin the policy onto a new user baseline after a settings change.
 *
 * Resets the derate state to the new configured pair and drops the shutdown
 * latch. Use this when the user edits frequency/voltage in AxeOS so the ceiling
 * tracks the user's decision instead of a value captured at task start.
 */
void atm_set_baseline(atm_state_t * state, float frequency_mhz, uint16_t voltage_mv);

/**
 * @brief Decide the next thermal action.
 *
 * @param cfg    tuning; must outlive the call.
 * @param state  in/out; frequency_mhz and voltage_mv are updated in place when
 *               the return value is STEP_DOWN / STEP_UP.
 * @param in     current measurements.
 *
 * @return the action. STEP_DOWN/STEP_UP mean frequency and voltage have both
 *         been moved by one step, in the same direction, with the regulator
 *         floor respected.
 */
atm_action_t atm_step(const atm_config_t * cfg, atm_state_t * state, const atm_inputs_t * in);

/** @brief Human-readable action name for logging. */
const char *atm_action_name(atm_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* ATM_POLICY_H */