#include "atm_policy.h"

void atm_init(atm_state_t * state, float frequency_mhz, uint16_t voltage_mv)
{
    if (state == NULL) {
        return;
    }
    state->frequency_mhz = frequency_mhz;
    state->voltage_mv = voltage_mv;
    state->last_change_s = 0;
    state->ever_downscaled = false;
    state->downscale_fan_percent = 0.0f;
    state->shutdown_requested = false;
}

void atm_clear_shutdown(atm_state_t * state)
{
    if (state == NULL) {
        return;
    }
    state->shutdown_requested = false;
}

void atm_set_baseline(atm_state_t * state, float frequency_mhz, uint16_t voltage_mv)
{
    if (state == NULL) {
        return;
    }
    state->frequency_mhz = frequency_mhz;
    state->voltage_mv = voltage_mv;
    state->last_change_s = 0;
    state->ever_downscaled = false;
    state->downscale_fan_percent = 0.0f;
    state->shutdown_requested = false;
}

const char *atm_action_name(atm_action_t action)
{
    switch (action) {
        case ATM_ACTION_NONE:         return "NONE";
        case ATM_ACTION_STEP_DOWN:    return "STEP_DOWN";
        case ATM_ACTION_STEP_UP:      return "STEP_UP";
        case ATM_ACTION_SHUTDOWN:     return "SHUTDOWN";
        case ATM_ACTION_FLOOR_REACHED:return "FLOOR_REACHED";
    }
    return "UNKNOWN";
}

/** Silicon is only considered stable if it is actually delivering hashes. */
static bool silicon_healthy(const atm_config_t * cfg, const atm_inputs_t * in)
{
    if (in->hashrate_ratio <= 0.0) {
        /* No hashrate telemetry yet — cannot claim stability, and cannot claim
         * instability either. Treat as healthy so the controller does not climb
         * voltage on an unmeasured device. */
        return true;
    }
    if (in->hashrate_ratio < cfg->min_hashrate_ratio) {
        return false;
    }
    return in->error_percent <= cfg->max_error_percent;
}

atm_action_t atm_step(const atm_config_t * cfg, atm_state_t * state, const atm_inputs_t * in)
{
    if (cfg == NULL || state == NULL || in == NULL) {
        return ATM_ACTION_NONE;
    }

    /* ---- 1. hard backstop ------------------------------------------- */
    const bool chip_critical = in->chip_temp_c >= cfg->shutdown_temp_c;
    const bool vr_critical = in->vr_temp_c >= cfg->vr_shutdown_temp_c;
    if (chip_critical || vr_critical) {
        state->shutdown_requested = true;
        return ATM_ACTION_SHUTDOWN;
    }
    if (state->shutdown_requested) {
        /* Latched: only cleared by atm_init() once the caller has actually
         * completed the cooldown + restart sequence. Prevents flapping back
         * into hashing the instant a single reading dips. */
        return ATM_ACTION_SHUTDOWN;
    }

    const bool hot = in->chip_temp_c >= cfg->hot_temp_c || in->vr_temp_c >= cfg->vr_hot_temp_c;

    /* ---- 2. downshift ------------------------------------------------ */
    if (hot) {
        /* A step is already in flight and has not had time to take effect. Do
         * not stack another one: at a 100 ms poll rate an un-gated ladder
         * would walk the whole frequency and voltage range away in a couple of
         * seconds and turn a survivable thermal excursion into a hashrate
         * collapse. The shutdown backstop above covers the genuine emergency. */
        if (state->last_change_s != 0 && in->uptime_s < state->last_change_s + cfg->min_hold_s) {
            return ATM_ACTION_NONE;
        }

        float next_freq = state->frequency_mhz - cfg->freq_step_mhz;
        int next_v = (int) state->voltage_mv - (int) cfg->voltage_step_mv;

        /* Frequency and voltage move together. Clamping voltage to the
         * regulator floor is what prevents the false "Power Fault" latch. */
        if (next_freq < cfg->freq_min_mhz) {
            next_freq = cfg->freq_min_mhz;
        }
        if (next_v < (int) cfg->voltage_floor_mv) {
            next_v = (int) cfg->voltage_floor_mv;
        }
        if (next_v > (int) cfg->voltage_ceiling_mv) {
            next_v = (int) cfg->voltage_ceiling_mv;
        }

        state->frequency_mhz = next_freq;
        state->voltage_mv = (uint16_t) next_v;
        state->last_change_s = in->uptime_s;
        state->ever_downscaled = true;
        state->downscale_fan_percent = (float) in->fan_percent;

        bool at_floor = (next_freq <= cfg->freq_min_mhz)
                     && (next_v <= (int) cfg->voltage_floor_mv);
        return at_floor ? ATM_ACTION_FLOOR_REACHED : ATM_ACTION_STEP_DOWN;
    }

    /* ---- 3. everything else is rate limited ------------------------- */
    if (state->last_change_s != 0 && in->uptime_s < state->last_change_s + cfg->min_hold_s) {
        return ATM_ACTION_NONE;
    }

    /* ---- 4. recovery step up ---------------------------------------- */
    /* Requires real headroom, not merely "below the threshold": LuxOS's rule
     * is that an observed 60 C with an 8 C buffer and a 65 C hot threshold must
     * NOT trigger an increase, because 65 C would then be overshot. */
    double required_headroom = cfg->buffer_temp_c;
    bool fan_relief = false;
    if (state->ever_downscaled) {
        /* Anti-hunting: after a downshift, demand extra margin before trying to
         * climb again... */
        required_headroom += (double) cfg->recheck_margin_c;
        /* ...unless cooling itself has measurably improved. If the fan is now
         * running meaningfully harder than it was when we derated, the extra
         * thermal margin has effectively already been bought, so making the user
         * wait for the full recheck band would only leave hashrate on the table.
         * This can never trigger on the first derate (no reference yet), and the
         * comparison is against a snapshot taken at derate time, so it cannot
         * ratchet: the fan figure is re-read every cycle and never accumulated. */
        if (cfg->fan_relief_percent > 0.0 && state->downscale_fan_percent > 0.0f) {
            double delta = in->fan_percent - (double) state->downscale_fan_percent;
            if (delta >= cfg->fan_relief_percent) {
                fan_relief = true;
            }
        }
    }

    if (!fan_relief) {
        if (in->chip_temp_c + required_headroom > cfg->hot_temp_c) {
            return ATM_ACTION_NONE;
        }
        if (in->vr_temp_c + required_headroom > cfg->vr_hot_temp_c) {
            return ATM_ACTION_NONE;
        }
    }
    if (!silicon_healthy(cfg, in)) {
        return ATM_ACTION_NONE;
    }

    if (state->frequency_mhz >= cfg->freq_ceiling_mhz
        && state->voltage_mv >= cfg->voltage_ceiling_mv) {
        return ATM_ACTION_NONE;
    }
    if (state->frequency_mhz >= cfg->freq_ceiling_mhz
        && state->voltage_mv < (int) cfg->voltage_ceiling_mv) {
        /* Frequency is already at the user's ceiling: only voltage may climb,
         * and only if it is below where the downshift left it. */
        if (state->ever_downscaled) {
            int next_v = (int) state->voltage_mv + (int) cfg->voltage_step_mv;
            if (next_v > (int) cfg->voltage_ceiling_mv) {
                next_v = (int) cfg->voltage_ceiling_mv;
            }
            if (next_v != (int) state->voltage_mv) {
                state->voltage_mv = (uint16_t) next_v;
                state->last_change_s = in->uptime_s;
                return ATM_ACTION_STEP_UP;
            }
        }
        return ATM_ACTION_NONE;
    }

    float next_freq = state->frequency_mhz + cfg->freq_step_mhz;
    int next_v = (int) state->voltage_mv + (int) cfg->voltage_step_mv;

    if (next_freq > cfg->freq_ceiling_mhz) {
        next_freq = cfg->freq_ceiling_mhz;
    }
    if (next_v > (int) cfg->voltage_ceiling_mv) {
        next_v = (int) cfg->voltage_ceiling_mv;
    }
    /* Never undercut the regulator while climbing either. */
    if (next_v < (int) cfg->voltage_floor_mv) {
        next_v = (int) cfg->voltage_floor_mv;
    }

    if (next_freq == state->frequency_mhz && next_v == (int) state->voltage_mv) {
        return ATM_ACTION_NONE;
    }

    /* Climb back to the baseline and forget the excursion. */
    if (next_freq >= cfg->freq_ceiling_mhz && next_v >= (int) cfg->voltage_ceiling_mv) {
        state->ever_downscaled = false;
    }

    state->frequency_mhz = next_freq;
    state->voltage_mv = (uint16_t) next_v;
    state->last_change_s = in->uptime_s;
    return ATM_ACTION_STEP_UP;
}
