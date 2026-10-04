#include "vf_tuner.h"

void vf_init(vf_state_t * state, uint16_t voltage_mv, uint16_t baseline_mv)
{
    if (state == NULL) {
        return;
    }
    state->voltage_mv = voltage_mv;
    state->pending_change_s = 0;
    state->change_pending = false;
    state->baseline_mv = baseline_mv;
    state->attempts = 0;
    state->finished = false;
}

uint16_t vf_target_voltage(const vf_state_t * state)
{
    return state ? state->voltage_mv : 0;
}

const char *vf_action_name(vf_action_t action)
{
    switch (action) {
        case VF_ACTION_NONE:            return "NONE";
        case VF_ACTION_LOWER_VOLTAGE:   return "LOWER_VOLTAGE";
        case VF_ACTION_RAISE_VOLTAGE:   return "RAISE_VOLTAGE";
        case VF_ACTION_BASELINE_REACHED:return "BASELINE_REACHED";
        case VF_ACTION_ABORT_THERMAL:   return "ABORT_THERMAL";
        case VF_ACTION_EXHAUSTED:       return "EXHAUSTED";
    }
    return "UNKNOWN";
}

static bool acceptance_passed(const vf_config_t * cfg, const vf_inputs_t * in)
{
    if (in->hashrate_ratio <= 0.0) {
        /* Not enough telemetry to judge. Do not treat silence as failure, but
         * also do not spend another attempt on it. */
        return false;
    }
    return in->hashrate_ratio >= cfg->min_hashrate_ratio
        && in->error_percent <= cfg->max_error_percent;
}

vf_action_t vf_step(const vf_config_t * cfg, vf_state_t * state, const vf_inputs_t * in)
{
    if (cfg == NULL || state == NULL || in == NULL || state->finished) {
        return VF_ACTION_NONE;
    }

    /* ---- 1. evaluate a pending change once it has settled ----------- */
    if (state->change_pending) {
        if (in->uptime_s < state->pending_change_s + cfg->settle_s) {
            return VF_ACTION_NONE;   /* still settling, say nothing */
        }

        state->change_pending = false;

        if (!acceptance_passed(cfg, in)) {
            /* The step was rejected by the acceptance gate. Restore the last
             * voltage that was proven stable; that becomes the new floor. */
            if (state->baseline_mv <= cfg->voltage_floor_mv) {
                state->voltage_mv = cfg->voltage_floor_mv;
                state->finished = true;
                return VF_ACTION_BASELINE_REACHED;
            }
            state->voltage_mv = (uint16_t) state->baseline_mv;
            state->finished = true;
            return VF_ACTION_RAISE_VOLTAGE;
        }

        /* Accepted. This voltage is now proven stable, so it becomes the new
         * baseline we must fall back to if the next step fails. */
        state->baseline_mv = state->voltage_mv;

        int next = (int) state->voltage_mv - (int) cfg->voltage_step_mv;
        if (next < (int) cfg->voltage_floor_mv) {
            state->voltage_mv = cfg->voltage_floor_mv;
            state->finished = true;
            return VF_ACTION_BASELINE_REACHED;
        }
        if (state->attempts >= cfg->max_attempts) {
            state->finished = true;
            return VF_ACTION_EXHAUSTED;
        }
        state->voltage_mv = (uint16_t) next;
        state->attempts++;
        state->change_pending = true;
        state->pending_change_s = in->uptime_s;
        return VF_ACTION_LOWER_VOLTAGE;
    }

    /* ---- 2. no change in flight: decide whether to start one -------- */
    if (state->attempts >= cfg->max_attempts) {
        state->finished = true;
        return VF_ACTION_EXHAUSTED;
    }
    /* Never keep shaving voltage while the chip is already too hot: the search
     * would push it further out of its stable envelope and fight the thermal
     * policy instead of helping it. Thermal work belongs to atm_policy. */
    if (in->chip_temp_c >= cfg->max_chip_temp_c) {
        return VF_ACTION_ABORT_THERMAL;
    }
    /* Already outside the acceptance window at the baseline voltage: more
     * steps down would only starve the chip further. */
    if (!acceptance_passed(cfg, in)) {
        return VF_ACTION_NONE;
    }
    if ((int) state->voltage_mv - (int) cfg->voltage_step_mv < (int) cfg->voltage_floor_mv) {
        state->finished = true;
        return VF_ACTION_BASELINE_REACHED;
    }

    state->voltage_mv = (uint16_t)((int) state->voltage_mv - (int) cfg->voltage_step_mv);
    state->attempts++;
    state->change_pending = true;
    state->pending_change_s = in->uptime_s;
    return VF_ACTION_LOWER_VOLTAGE;
}