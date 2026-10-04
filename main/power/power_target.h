#ifndef POWER_TARGET_H
#define POWER_TARGET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* NULL - <stdint.h> does not provide it on glibc/newlib */

/*
 * Power-targeting loop — dependency-free decision logic.
 *
 * Why this exists, and why it is not the temperature controller:
 *
 * LuxOS "Power Targeting" and Braiins both anchor their loops on a measurable
 * *outcome* — watts, or a power/hashrate target — rather than on a temperature
 * setpoint. The reason is measurable: LuxOS reports power draw varying by more
 * than 10 % through the day at identical settings, driven purely by ambient
 * temperature. That diurnal swing is the entire available prize, and a fan-PID
 * loop cannot even observe it, because it never looks at watts.
 *
 * So the operator states a power budget and this module decides, each step,
 * whether to shed or reclaim a little of it, based on measured power versus the
 * target. It deliberately does NOT decide temperature limits — that stays with
 * atm_policy.c, which owns the regulator floor and the safety backstops. The two
 * compose: ATM may veto or demand a step at any time, and this loop only
 * operates inside the envelope ATM has left.
 *
 * Units: milliwatts throughout, to match the INA260 telemetry the rest of the
 * firmware uses. Deliberately integer-only so it can be unit-tested on the host
 * with no ESP-IDF and no floating-point surprises.
 */

typedef enum {
    PT_ACTION_NONE = 0,
    PT_ACTION_SHED,     /* above budget: give some back                  */
    PT_ACTION_RECLAIM,  /* under budget and stable: take some back        */
    PT_ACTION_TARGET_INVALID
} pt_action_t;

typedef struct {
    uint32_t target_mw;          /* power budget the operator asked for   */
    uint32_t tolerance_mw;       /* deadband either side of target        */
    uint32_t step_mw;            /* size of one correction                 */
    uint32_t settle_s;           /* dwell after a change, for measurement */
    uint32_t min_hold_s;         /* minimum time between corrections      */
    uint32_t min_mw;             /* never go below this                    */
    uint32_t max_mw;             /* never go above this                    */
} pt_config_t;

typedef struct {
    uint32_t measured_mw;        /* 0 when there is no usable measurement  */
    bool power_valid;            /* false when the INA260 reading is bad  */
    uint32_t uptime_s;
} pt_inputs_t;

typedef struct {
    uint32_t current_mw;         /* the power budget in force right now    */
    uint32_t last_change_s;
    uint32_t settle_from_s;      /* when the current budget was applied    */
    bool settle_pending;
    uint32_t attempts;
    bool finished;
} pt_state_t;

/** @brief Seed the loop at the device's present power draw. */
void pt_init(pt_state_t * state, uint32_t current_mw);

/**
 * @brief Decide the next power-budget correction.
 *
 * Never returns SHED or RECLAIM while a previous change is still settling, so
 * calling this once per control tick is safe. A change is only made when the
 * measurement is valid and outside the deadband, which keeps a noisy or missing
 * reading from driving the loop.
 */
pt_action_t pt_step(const pt_config_t * cfg, pt_state_t * state, const pt_inputs_t * in);

/** @brief Power budget currently in force, for logging and the API. */
uint32_t pt_target_mw(const pt_state_t * state);

const char *pt_action_name(pt_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* POWER_TARGET_H */