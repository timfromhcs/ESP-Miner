#include "power_target.h"

void pt_init(pt_state_t * state, uint32_t current_mw)
{
    if (state == NULL) {
        return;
    }
    state->current_mw = current_mw;
    state->last_change_s = 0;
    state->settle_from_s = 0;
    state->settle_pending = false;
    state->attempts = 0;
    state->finished = false;
}

uint32_t pt_target_mw(const pt_state_t * state)
{
    return state ? state->current_mw : 0;
}

const char *pt_action_name(pt_action_t action)
{
    switch (action) {
        case PT_ACTION_NONE:           return "NONE";
        case PT_ACTION_SHED:           return "SHED";
        case PT_ACTION_RECLAIM:        return "RECLAIM";
        case PT_ACTION_TARGET_INVALID: return "TARGET_INVALID";
    }
    return "UNKNOWN";
}

/** Clamp a proposed budget into the configured envelope. */
static uint32_t clamp_budget(const pt_config_t * cfg, int64_t proposed)
{
    if (proposed < (int64_t) cfg->min_mw) {
        return cfg->min_mw;
    }
    if (proposed > (int64_t) cfg->max_mw) {
        return cfg->max_mw;
    }
    return (uint32_t) proposed;
}

pt_action_t pt_step(const pt_config_t * cfg, pt_state_t * state, const pt_inputs_t * in)
{
    if (cfg == NULL || state == NULL || in == NULL || state->finished) {
        return PT_ACTION_NONE;
    }
    /* A target outside the envelope can never be satisfied, so refuse it up front
     * rather than oscillating against a limit forever. */
    if (cfg->target_mw < cfg->min_mw || cfg->target_mw > cfg->max_mw || cfg->step_mw == 0) {
        return PT_ACTION_TARGET_INVALID;
    }

    /* A reading we do not trust must not steer the loop. Sitting still is the
     * safe response: guessing could walk the budget away on bad telemetry. */
    if (!in->power_valid || in->measured_mw == 0) {
        return PT_ACTION_NONE;
    }

    /* Let a previous correction take effect before judging the next one. */
    if (state->settle_pending) {
        if (in->uptime_s < state->settle_from_s + cfg->settle_s) {
            return PT_ACTION_NONE;
        }
        state->settle_pending = false;
    }

    if (state->last_change_s != 0 && in->uptime_s < state->last_change_s + cfg->min_hold_s) {
        return PT_ACTION_NONE;
    }

    if (in->measured_mw > cfg->target_mw + cfg->tolerance_mw) {
        /* Over budget: give some back. Shrinking the budget is always safe, so no
         * asymmetric acceptance gate is needed here. */
        uint32_t next = clamp_budget(cfg, (int64_t) state->current_mw - (int64_t) cfg->step_mw);
        if (next == state->current_mw) {
            return PT_ACTION_NONE;
        }
        state->current_mw = next;
        state->last_change_s = in->uptime_s;
        state->settle_pending = true;
        state->settle_from_s = in->uptime_s;
        state->attempts++;
        return PT_ACTION_SHED;
    }

    if (in->measured_mw < cfg->target_mw - cfg->tolerance_mw) {
        /* Under budget: we could reclaim. Only worth doing once we have actually
         * reached the budget - otherwise we would be adding allowance to a budget
         * we are already failing, which makes the shortfall worse. */
        if (state->current_mw < cfg->target_mw) {
            return PT_ACTION_NONE;
        }
        uint32_t next = clamp_budget(cfg, (int64_t) state->current_mw + (int64_t) cfg->step_mw);
        if (next == state->current_mw) {
            return PT_ACTION_NONE;
        }
        state->current_mw = next;
        state->last_change_s = in->uptime_s;
        state->settle_pending = true;
        state->settle_from_s = in->uptime_s;
        state->attempts++;
        return PT_ACTION_RECLAIM;
    }

    /* Inside the deadband, or the budget cannot be moved further: stop here. */
    if (state->current_mw >= cfg->target_mw && cfg->max_mw <= cfg->target_mw) {
        state->finished = true;
    }
    return PT_ACTION_NONE;
}