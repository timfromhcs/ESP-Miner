#include "research_guard.h"

#include <string.h>

/* Absolute bounds on what a caller may configure. These are ceilings on the
 * guard's own allowances, not device limits: the guard can never permit more
 * than this, whatever config it is given. */
#define RESEARCH_GUARD_MAX_JOBS_LIMIT 4096u
#define RESEARCH_GUARD_MAX_RUNTIME_MS (4ull * 60ull * 60ull * 1000ull)
#define RESEARCH_GUARD_MAX_TOKEN_TTL_MS 600000u
#define RESEARCH_GUARD_MAX_CEILING_C 85.0f

research_guard_config_t research_guard_config_default(void)
{
    research_guard_config_t cfg = {
        .max_jobs = 64,
        .max_runtime_ms = 15ull * 60ull * 1000ull,
        .token_ttl_ms = 120u * 1000u,
        .max_temperature_c = 68.0f,
        .max_power_w = 30.0f,
    };
    return cfg;
}

static bool config_valid(const research_guard_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->max_jobs == 0 || cfg->max_jobs > RESEARCH_GUARD_MAX_JOBS_LIMIT) {
        return false;
    }
    if (cfg->max_runtime_ms == 0 || cfg->max_runtime_ms > RESEARCH_GUARD_MAX_RUNTIME_MS) {
        return false;
    }
    if (cfg->token_ttl_ms == 0 || cfg->token_ttl_ms > RESEARCH_GUARD_MAX_TOKEN_TTL_MS) {
        return false;
    }
    if (!(cfg->max_temperature_c > 0.0f) || cfg->max_temperature_c > RESEARCH_GUARD_MAX_CEILING_C) {
        return false;
    }
    if (!(cfg->max_power_w > 0.0f)) {
        return false;
    }
    return true;
}

/* Unknown is unsafe. NaN is unsafe: both comparisons below are written so that a
 * NaN reading falls through to the refusal. */
static research_guard_result_t check_conditions(const research_guard_t *guard, const research_guard_conditions_t *cond)
{
    if (guard == NULL || cond == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (!cond->identity_verified) {
        return RESEARCH_GUARD_ERR_IDENTITY;
    }
    if (cond->pool_connected || cond->share_in_flight) {
        return RESEARCH_GUARD_ERR_POOL;
    }
    if (!cond->thermal_known) {
        return RESEARCH_GUARD_ERR_THERMAL;
    }
    if (!(cond->temperature_c <= guard->cfg.max_temperature_c)) {
        return RESEARCH_GUARD_ERR_THERMAL;
    }
    if (cond->power_known && !(cond->power_w <= guard->cfg.max_power_w)) {
        return RESEARCH_GUARD_ERR_POWER;
    }
    return RESEARCH_GUARD_OK;
}

uint64_t research_guard_active_ms(const research_guard_t *guard, uint64_t now_ms)
{
    uint64_t end;
    uint64_t total;

    if (guard == NULL || guard->state == RESEARCH_GUARD_DISARMED) {
        return 0;
    }
    end = guard->paused ? guard->pause_started_ms : now_ms;
    if (end < guard->session_start_ms) {
        return 0; /* clock went backwards: report no progress, never a negative */
    }
    total = end - guard->session_start_ms;
    if (guard->paused_total_ms > total) {
        return 0;
    }
    return total - guard->paused_total_ms;
}

uint32_t research_guard_remaining_jobs(const research_guard_t *guard)
{
    if (guard == NULL || guard->state == RESEARCH_GUARD_DISARMED) {
        return 0;
    }
    if (guard->jobs_started >= guard->cfg.max_jobs) {
        return 0;
    }
    return guard->cfg.max_jobs - guard->jobs_started;
}

static research_guard_result_t check_budget(const research_guard_t *guard, uint64_t now_ms)
{
    if (guard->jobs_started >= guard->cfg.max_jobs) {
        return RESEARCH_GUARD_ERR_JOBS;
    }
    if (research_guard_active_ms(guard, now_ms) >= guard->cfg.max_runtime_ms) {
        return RESEARCH_GUARD_ERR_TIME;
    }
    return RESEARCH_GUARD_OK;
}

static research_guard_result_t check_token(const research_guard_t *guard, uint32_t token, uint64_t now_ms)
{
    if (!guard->token_live || token == 0 || token != guard->offered_token) {
        return RESEARCH_GUARD_ERR_TOKEN;
    }
    if (now_ms < guard->offered_at_ms) {
        return RESEARCH_GUARD_ERR_TOKEN;
    }
    if (now_ms - guard->offered_at_ms > guard->cfg.token_ttl_ms) {
        return RESEARCH_GUARD_ERR_TOKEN;
    }
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_init(research_guard_t *guard, const research_guard_config_t *cfg)
{
    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    memset(guard, 0, sizeof(*guard));
    guard->state = RESEARCH_GUARD_DISARMED;
    guard->last_refusal = RESEARCH_GUARD_OK;
    if (cfg == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (!config_valid(cfg)) {
        guard->last_refusal = RESEARCH_GUARD_ERR_ARG;
        return RESEARCH_GUARD_ERR_ARG;
    }
    guard->cfg = *cfg;
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_offer_token(research_guard_t *guard, uint32_t token, uint64_t now_ms)
{
    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (token == 0) {
        return RESEARCH_GUARD_ERR_TOKEN;
    }
    guard->offered_token = token;
    guard->offered_at_ms = now_ms;
    guard->token_live = true;
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_arm(research_guard_t *guard, uint32_t token, uint64_t now_ms,
                                           const research_guard_conditions_t *cond)
{
    research_guard_result_t r;

    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state != RESEARCH_GUARD_DISARMED) {
        r = RESEARCH_GUARD_ERR_STATE;
    } else if ((r = check_token(guard, token, now_ms)) == RESEARCH_GUARD_OK &&
               (r = check_conditions(guard, cond)) == RESEARCH_GUARD_OK) {
        /* Single use: arming consumes the token, so a replayed arm request is
         * rejected even inside its TTL window. */
        guard->token_live = false;
        guard->offered_token = 0;
        guard->state = RESEARCH_GUARD_ARMED;
        guard->jobs_started = 0;
        guard->jobs_completed = 0;
        guard->session_start_ms = now_ms;
        guard->paused_total_ms = 0;
        guard->pause_started_ms = 0;
        guard->paused = false;
        guard->identity_verified = cond->identity_verified;
        guard->power_verified = cond->power_known;
        guard->last_refusal = RESEARCH_GUARD_OK;
        return RESEARCH_GUARD_OK;
    }
    guard->last_refusal = r;
    return r;
}

research_guard_result_t research_guard_begin_job(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond)
{
    research_guard_result_t r;

    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state == RESEARCH_GUARD_RUNNING) {
        r = RESEARCH_GUARD_ERR_BUSY;
    } else if (guard->state != RESEARCH_GUARD_ARMED) {
        r = RESEARCH_GUARD_ERR_STATE;
    } else if ((r = check_conditions(guard, cond)) != RESEARCH_GUARD_OK) {
        /* Conditions became unsafe between arm and start: refuse and disarm, so
         * the next request has to be authorised again from scratch. */
        guard->state = RESEARCH_GUARD_DISARMED;
    } else if ((r = check_budget(guard, now_ms)) != RESEARCH_GUARD_OK) {
        guard->state = RESEARCH_GUARD_DISARMED;
    } else {
        guard->jobs_started++;
        guard->state = RESEARCH_GUARD_RUNNING;
        guard->power_verified = cond->power_known;
        guard->last_refusal = RESEARCH_GUARD_OK;
        return RESEARCH_GUARD_OK;
    }
    guard->last_refusal = r;
    return r;
}

research_guard_result_t research_guard_finish_job(research_guard_t *guard, uint64_t now_ms)
{
    research_guard_result_t budget;

    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state != RESEARCH_GUARD_RUNNING) {
        guard->last_refusal = RESEARCH_GUARD_ERR_STATE;
        return RESEARCH_GUARD_ERR_STATE;
    }
    guard->jobs_completed++;
    guard->state = RESEARCH_GUARD_ARMED;

    /* A finished job is a success; running out of budget still disarms the
     * session, and the reason stays visible in `last_refusal` and the status. */
    budget = check_budget(guard, now_ms);
    if (budget != RESEARCH_GUARD_OK) {
        guard->state = RESEARCH_GUARD_DISARMED;
        guard->last_refusal = budget;
    }
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_pause(research_guard_t *guard, uint64_t now_ms)
{
    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state != RESEARCH_GUARD_RUNNING) {
        guard->last_refusal = RESEARCH_GUARD_ERR_STATE;
        return RESEARCH_GUARD_ERR_STATE;
    }
    guard->state = RESEARCH_GUARD_PAUSED;
    guard->paused = true;
    guard->pause_started_ms = now_ms;
    guard->last_refusal = RESEARCH_GUARD_OK;
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_resume(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond)
{
    research_guard_result_t r;

    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state != RESEARCH_GUARD_PAUSED) {
        r = RESEARCH_GUARD_ERR_STATE;
    } else if ((r = check_conditions(guard, cond)) == RESEARCH_GUARD_OK) {
        if (now_ms > guard->pause_started_ms) {
            guard->paused_total_ms += now_ms - guard->pause_started_ms;
        }
        guard->paused = false;
        guard->pause_started_ms = 0;
        /* Resume returns to ARMED, not RUNNING: paused work is abandoned, not
         * silently continued, so the next job goes through begin_job again. */
        guard->state = RESEARCH_GUARD_ARMED;
        guard->power_verified = cond->power_known;
        guard->last_refusal = RESEARCH_GUARD_OK;
        return RESEARCH_GUARD_OK;
    }
    guard->last_refusal = r;
    return r;
}

research_guard_result_t research_guard_cancel(research_guard_t *guard, uint64_t now_ms)
{
    research_guard_result_t budget;

    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    if (guard->state != RESEARCH_GUARD_RUNNING && guard->state != RESEARCH_GUARD_PAUSED) {
        guard->last_refusal = RESEARCH_GUARD_ERR_STATE;
        return RESEARCH_GUARD_ERR_STATE;
    }
    if (guard->paused) {
        if (now_ms > guard->pause_started_ms) {
            guard->paused_total_ms += now_ms - guard->pause_started_ms;
        }
        guard->paused = false;
        guard->pause_started_ms = 0;
    }
    guard->state = RESEARCH_GUARD_ARMED;
    budget = check_budget(guard, now_ms);
    if (budget != RESEARCH_GUARD_OK) {
        guard->state = RESEARCH_GUARD_DISARMED;
        guard->last_refusal = budget;
    } else {
        guard->last_refusal = RESEARCH_GUARD_OK;
    }
    return RESEARCH_GUARD_OK;
}

research_guard_result_t research_guard_disarm(research_guard_t *guard, uint64_t now_ms)
{
    (void) now_ms;
    if (guard == NULL) {
        return RESEARCH_GUARD_ERR_ARG;
    }
    guard->state = RESEARCH_GUARD_DISARMED;
    guard->token_live = false;
    guard->offered_token = 0;
    guard->paused = false;
    guard->pause_started_ms = 0;
    guard->last_refusal = RESEARCH_GUARD_OK;
    return RESEARCH_GUARD_OK;
}

research_guard_state_t research_guard_evaluate(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond)
{
    research_guard_result_t r;

    if (guard == NULL || guard->state == RESEARCH_GUARD_DISARMED) {
        return RESEARCH_GUARD_DISARMED;
    }
    r = check_conditions(guard, cond);
    if (r == RESEARCH_GUARD_OK) {
        r = check_budget(guard, now_ms);
    }
    if (r != RESEARCH_GUARD_OK) {
        guard->state = RESEARCH_GUARD_DISARMED;
        guard->paused = false;
        guard->pause_started_ms = 0;
        guard->last_refusal = r;
    }
    return guard->state;
}

const char *research_guard_state_str(research_guard_state_t state)
{
    switch (state) {
    case RESEARCH_GUARD_DISARMED:
        return "disarmed";
    case RESEARCH_GUARD_ARMED:
        return "armed";
    case RESEARCH_GUARD_RUNNING:
        return "running";
    case RESEARCH_GUARD_PAUSED:
        return "paused";
    default:
        return "unknown";
    }
}

const char *research_guard_result_str(research_guard_result_t result)
{
    switch (result) {
    case RESEARCH_GUARD_OK:
        return "ok";
    case RESEARCH_GUARD_ERR_ARG:
        return "bad_argument";
    case RESEARCH_GUARD_ERR_STATE:
        return "wrong_state";
    case RESEARCH_GUARD_ERR_TOKEN:
        return "no_valid_token";
    case RESEARCH_GUARD_ERR_POOL:
        return "pool_active";
    case RESEARCH_GUARD_ERR_IDENTITY:
        return "identity_unverified";
    case RESEARCH_GUARD_ERR_THERMAL:
        return "thermal_unsafe";
    case RESEARCH_GUARD_ERR_POWER:
        return "power_unsafe";
    case RESEARCH_GUARD_ERR_JOBS:
        return "job_budget_exhausted";
    case RESEARCH_GUARD_ERR_TIME:
        return "time_budget_exhausted";
    case RESEARCH_GUARD_ERR_BUSY:
        return "job_already_running";
    default:
        return "unknown";
    }
}
