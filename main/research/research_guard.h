#ifndef RESEARCH_GUARD_H_
#define RESEARCH_GUARD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime authorisation for on-device SHA research work.
 *
 * The firmware must never start research work because a request arrived. It
 * starts work only when a short-lived operator token was paired, the device
 * identity was verified in this boot, no mining work can reach a pool, the
 * thermal reading is fresh and below the research ceiling, and the session is
 * still inside its job/time budget. Every one of those is a decision function
 * here, with no ESP-IDF dependency, so each refusal can be proven in a host test
 * (simulation/test_research_guard.c) instead of on the only device in the field.
 *
 * Fail-closed defaults: a fresh guard is DISARMED, it holds no token, and any
 * unknown safety input is treated as unsafe. Nothing in this module can raise a
 * thermal, power or frequency limit; the ceiling is an *upper* bound the caller
 * passes in from the existing policy, and the guard refuses above it.
 */

typedef enum {
    RESEARCH_GUARD_DISARMED = 0,
    RESEARCH_GUARD_ARMED,
    RESEARCH_GUARD_RUNNING,
    RESEARCH_GUARD_PAUSED,
} research_guard_state_t;

typedef enum {
    RESEARCH_GUARD_OK = 0,
    RESEARCH_GUARD_ERR_ARG,
    RESEARCH_GUARD_ERR_STATE,     /* command not valid in the current state */
    RESEARCH_GUARD_ERR_TOKEN,     /* no offered token, wrong token, or expired */
    RESEARCH_GUARD_ERR_POOL,      /* pool connected or a share is outstanding */
    RESEARCH_GUARD_ERR_IDENTITY,  /* device identity not verified */
    RESEARCH_GUARD_ERR_THERMAL,   /* temperature unknown or above the ceiling */
    RESEARCH_GUARD_ERR_POWER,     /* measured power above the ceiling */
    RESEARCH_GUARD_ERR_JOBS,      /* job budget exhausted */
    RESEARCH_GUARD_ERR_TIME,      /* session time budget exhausted */
    RESEARCH_GUARD_ERR_BUSY,      /* a job is already running */
} research_guard_result_t;

/* Safety inputs sampled by the caller at the moment of the decision. Stale data
 * is the caller's responsibility to mark unknown (`*_known = false`), which the
 * guard treats as unsafe. */
typedef struct {
    bool identity_verified;
    bool pool_connected;
    bool share_in_flight;
    bool thermal_known;
    float temperature_c;
    /* Age of the reading. A reading taken while the ASIC was running stays usable
     * for a short window after the pause: with the ASIC powered down the die can
     * only get cooler, so the value is an upper bound, never a guess in the
     * dangerous direction. Older than the configured window and it is refused. */
    uint64_t thermal_age_ms;
    bool power_known;
    float power_w;
} research_guard_conditions_t;

typedef struct {
    uint32_t max_jobs;         /* hard count, e.g. 64 */
    uint64_t max_runtime_ms;   /* active session time, pauses excluded */
    uint32_t token_ttl_ms;     /* how long an offered token stays valid */
    float max_temperature_c;   /* never above the device's own limits */
    float max_power_w;         /* only enforced when power is known */
    uint64_t max_thermal_age_ms; /* how old a pre-pause reading may be */
} research_guard_config_t;

typedef struct {
    research_guard_config_t cfg;
    research_guard_state_t state;
    research_guard_result_t last_refusal;
    uint32_t offered_token;
    uint64_t offered_at_ms;
    bool token_live;
    uint32_t jobs_started;
    uint32_t jobs_completed;
    uint64_t session_start_ms;
    uint64_t paused_total_ms;
    uint64_t pause_started_ms;
    bool paused;
    /* Last accepted condition snapshot, for status reporting only. */
    bool identity_verified;
    bool power_verified;
} research_guard_t;

/* Conservative defaults: 64 jobs, 15 minutes of active time, a 2-minute token,
 * and a thermal ceiling well below the miner's own shutdown path. */
research_guard_config_t research_guard_config_default(void);

/* Returns RESEARCH_GUARD_OK, or ERR_ARG for a config that could weaken safety
 * (zero budgets, a ceiling above 85 C, a token TTL of zero). */
research_guard_result_t research_guard_init(research_guard_t *guard, const research_guard_config_t *cfg);

/* Operator pairing: the UI asks for a token, the operator confirms it out of
 * band (screen/serial), and only then can the session be armed. */
research_guard_result_t research_guard_offer_token(research_guard_t *guard, uint32_t token, uint64_t now_ms);

research_guard_result_t research_guard_arm(research_guard_t *guard, uint32_t token, uint64_t now_ms,
                                           const research_guard_conditions_t *cond);
research_guard_result_t research_guard_begin_job(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond);
research_guard_result_t research_guard_finish_job(research_guard_t *guard, uint64_t now_ms);
research_guard_result_t research_guard_pause(research_guard_t *guard, uint64_t now_ms);
research_guard_result_t research_guard_resume(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond);
research_guard_result_t research_guard_cancel(research_guard_t *guard, uint64_t now_ms);
research_guard_result_t research_guard_disarm(research_guard_t *guard, uint64_t now_ms);

/* Watchdog. Called on every status poll and before every job: if the session is
 * armed and a condition became unsafe or the budget expired, the guard disarms
 * itself and records why. Returns the state after the check. */
research_guard_state_t research_guard_evaluate(research_guard_t *guard, uint64_t now_ms, const research_guard_conditions_t *cond);

/* Time actually spent in the session, excluding paused intervals. */
uint64_t research_guard_active_ms(const research_guard_t *guard, uint64_t now_ms);
uint32_t research_guard_remaining_jobs(const research_guard_t *guard);

/* Stable identifiers for the JSON status document. */
const char *research_guard_state_str(research_guard_state_t state);
const char *research_guard_result_str(research_guard_result_t result);

/* Whether mining work can still reach a pool right now.
 *
 * Measured on the device: ESP-Miner's `/api/system/pause` stops creating jobs and
 * submitting shares but keeps the stratum socket open, so "connected" alone is
 * not the question the guard needs answered. The question is "can a share leave
 * this device", and that is false only while the operator's pause flag is set and
 * nothing is already in flight. A single outstanding share still blocks, because
 * its result would be submitted after the pause. */
bool research_guard_mining_active(bool pool_connected, bool mining_paused, bool share_in_flight);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_GUARD_H_ */
