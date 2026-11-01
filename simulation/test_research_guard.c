/*
 * Host tests for the research authorisation guard (main/research/research_guard.c).
 *
 * Every refusal path in AGENTBUILD.md section 5.6 is exercised here: pool
 * connected, a share still outstanding, unknown identity, unknown or too hot
 * thermal reading, over-power, no paired token, expired token, reused token,
 * exhausted job budget, exhausted time budget. Each one is a case below, and the
 * closing test asserts that a guard which has been disarmed by a safety trip
 * cannot be restarted without a fresh operator token.
 *
 * The guard is the only thing standing between "a HTTP request arrived" and "the
 * ASIC runs research work", so these tests are the firmware's safety contract,
 * not a formality.
 */

#include "research_guard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_res(research_guard_result_t got, research_guard_result_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s (got %s, want %s)\n", what, research_guard_result_str(got), research_guard_result_str(want));
    }
}

/* A device that is healthy for research: identity verified, no pool, 60 C. */
static research_guard_conditions_t healthy(void)
{
    research_guard_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.identity_verified = true;
    c.pool_connected = false;
    c.share_in_flight = false;
    c.thermal_known = true;
    c.temperature_c = 60.0f;
    c.power_known = true;
    c.power_w = 18.0f;
    return c;
}

static research_guard_t fresh(void)
{
    research_guard_t g;
    research_guard_config_t cfg = research_guard_config_default();
    research_guard_init(&g, &cfg);
    return g;
}

/* Arm a fresh guard at t=0 with token 0xC0FFEE. */
static research_guard_t armed_at_zero(void)
{
    research_guard_t g = fresh();
    research_guard_offer_token(&g, 0xC0FFEEu, 0);
    research_guard_conditions_t c = healthy();
    research_guard_arm(&g, 0xC0FFEEu, 0, &c);
    return g;
}

/* ------------------------------------------------------------- config */

static void test_config(void)
{
    printf("== configuration is fail-closed ==\n");
    research_guard_t g;
    research_guard_config_t cfg = research_guard_config_default();

    check(research_guard_init(NULL, &cfg) == RESEARCH_GUARD_ERR_ARG, "NULL guard rejected");
    check(research_guard_init(&g, NULL) == RESEARCH_GUARD_ERR_ARG, "NULL config rejected");
    check(g.state == RESEARCH_GUARD_DISARMED, "a rejected init leaves a disarmed guard");

    cfg = research_guard_config_default();
    cfg.max_jobs = 0;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "zero job budget rejected");

    cfg = research_guard_config_default();
    cfg.max_jobs = 100000;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "absurd job budget rejected");

    cfg = research_guard_config_default();
    cfg.max_runtime_ms = 0;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "zero time budget rejected");

    cfg = research_guard_config_default();
    cfg.max_runtime_ms = 48ull * 3600ull * 1000ull;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "two-day session rejected");

    cfg = research_guard_config_default();
    cfg.token_ttl_ms = 0;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "zero token TTL rejected");

    cfg = research_guard_config_default();
    cfg.token_ttl_ms = 86400000u;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "day-long token rejected");

    cfg = research_guard_config_default();
    cfg.max_temperature_c = 90.0f;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "thermal ceiling above 85 C rejected");

    cfg = research_guard_config_default();
    cfg.max_temperature_c = -1.0f;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "negative thermal ceiling rejected");

    cfg = research_guard_config_default();
    cfg.max_power_w = 0.0f;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "zero power ceiling rejected");

    cfg = research_guard_config_default();
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_OK, "default config accepted");
    check(g.state == RESEARCH_GUARD_DISARMED, "fresh guard is disarmed");
    check(g.token_live == false, "fresh guard holds no token");
    check(research_guard_remaining_jobs(&g) == 0, "a disarmed guard reports no remaining jobs");
}

/* --------------------------------------------------------------- token */

static void test_token(void)
{
    printf("== operator token ==\n");
    research_guard_t g = fresh();
    research_guard_conditions_t c = healthy();

    check_res(research_guard_offer_token(NULL, 1, 0), RESEARCH_GUARD_ERR_ARG, "offer on NULL guard rejected");
    check_res(research_guard_offer_token(&g, 0, 0), RESEARCH_GUARD_ERR_TOKEN, "zero token rejected");
    check_res(research_guard_offer_token(&g, 0x1234, 1000), RESEARCH_GUARD_OK, "token offered");

    check_res(research_guard_arm(&g, 0x9999, 1000, &c), RESEARCH_GUARD_ERR_TOKEN, "wrong token rejected");
    check_res(research_guard_arm(&g, 0x1234, 5000, &c), RESEARCH_GUARD_OK, "matching token arms the guard");
    check(g.state == RESEARCH_GUARD_ARMED, "state is armed");
    check(g.token_live == false, "arming consumes the token");

    check_res(research_guard_arm(&g, 0x1234, 5001, &c), RESEARCH_GUARD_ERR_STATE, "a second arm while armed is refused");

    /* A consumed token stays consumed after disarm: authorisation is not a
     * durable property of the device. */
    research_guard_disarm(&g, 6000);
    check_res(research_guard_arm(&g, 0x1234, 6001, &c), RESEARCH_GUARD_ERR_TOKEN, "a consumed token cannot re-arm");

    /* Expiry is measured from the offer, not from the arm attempt. */
    research_guard_t e = fresh();
    research_guard_offer_token(&e, 0x55AA, 1000);
    check_res(research_guard_arm(&e, 0x55AA, 1000 + e.cfg.token_ttl_ms + 1, &c), RESEARCH_GUARD_ERR_TOKEN, "expired token rejected");
    check_res(research_guard_arm(&e, 0x55AA, 1000 + e.cfg.token_ttl_ms, &c), RESEARCH_GUARD_OK, "token valid exactly at its TTL");

    /* A clock that jumps backwards must not extend the token's life. */
    research_guard_t b = fresh();
    research_guard_offer_token(&b, 0x77, 10000);
    check_res(research_guard_arm(&b, 0x77, 5000, &c), RESEARCH_GUARD_ERR_TOKEN, "backwards clock rejected");
}

/* ---------------------------------------------------------- conditions */

static void test_conditions(void)
{
    printf("== safety conditions ==\n");
    research_guard_conditions_t c;

    struct {
        const char *what;
        research_guard_result_t want;
    } cases[] = {
        {"identity not verified", RESEARCH_GUARD_ERR_IDENTITY},
        {"pool connected", RESEARCH_GUARD_ERR_POOL},
        {"share outstanding", RESEARCH_GUARD_ERR_POOL},
        {"temperature unknown", RESEARCH_GUARD_ERR_THERMAL},
        {"temperature above the ceiling", RESEARCH_GUARD_ERR_THERMAL},
        {"temperature is NaN", RESEARCH_GUARD_ERR_THERMAL},
        {"power above the ceiling", RESEARCH_GUARD_ERR_POWER},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        research_guard_t g = fresh();
        research_guard_offer_token(&g, 0xA1, 0);
        c = healthy();
        switch (i) {
        case 0:
            c.identity_verified = false;
            break;
        case 1:
            c.pool_connected = true;
            break;
        case 2:
            c.share_in_flight = true;
            break;
        case 3:
            c.thermal_known = false;
            break;
        case 4:
            c.temperature_c = g.cfg.max_temperature_c + 0.5f;
            break;
        case 5:
            c.temperature_c = NAN;
            break;
        case 6:
            c.power_w = g.cfg.max_power_w + 1.0f;
            break;
        default:
            break;
        }
        check_res(research_guard_arm(&g, 0xA1, 0, &c), cases[i].want, cases[i].what);
        check(g.state == RESEARCH_GUARD_DISARMED, "a refused arm leaves the guard disarmed");
        check(g.last_refusal == cases[i].want, "the refusal reason is recorded");
    }

    /* Exactly at each ceiling is allowed; one step above is not. This pins the
     * comparison direction so an inverted test cannot pass as "safe". */
    research_guard_t g = fresh();
    research_guard_offer_token(&g, 0xB2, 0);
    c = healthy();
    c.temperature_c = g.cfg.max_temperature_c;
    c.power_w = g.cfg.max_power_w;
    check_res(research_guard_arm(&g, 0xB2, 0, &c), RESEARCH_GUARD_OK, "readings exactly at the ceiling are accepted");

    /* Unknown power is a documented downgrade, not a refusal: it is surfaced in
     * the status as power_verified=false. */
    research_guard_t u = fresh();
    research_guard_offer_token(&u, 0xC3, 0);
    c = healthy();
    c.power_known = false;
    c.power_w = NAN;
    check_res(research_guard_arm(&u, 0xC3, 0, &c), RESEARCH_GUARD_OK, "unknown power alone does not block");
    check(u.power_verified == false, "unknown power is recorded as unverified");
}

/* -------------------------------------------------------------- session */

static void test_job_cycle(void)
{
    printf("== job lifecycle ==\n");
    research_guard_conditions_t c = healthy();
    research_guard_t g = fresh();

    check_res(research_guard_begin_job(&g, 0, &c), RESEARCH_GUARD_ERR_STATE, "cannot start a job while disarmed");

    g = armed_at_zero();
    check_res(research_guard_begin_job(&g, 10, &c), RESEARCH_GUARD_OK, "job starts when armed");
    check(g.state == RESEARCH_GUARD_RUNNING, "state is running");
    check(g.jobs_started == 1, "job counted at start");
    check_res(research_guard_begin_job(&g, 20, &c), RESEARCH_GUARD_ERR_BUSY, "a second concurrent job is refused");
    check_res(research_guard_finish_job(&g, 30), RESEARCH_GUARD_OK, "job finishes");
    check(g.state == RESEARCH_GUARD_ARMED, "state returns to armed");
    check(g.jobs_completed == 1, "completion counted");

    check_res(research_guard_finish_job(&g, 40), RESEARCH_GUARD_ERR_STATE, "finishing when nothing runs is refused");

    /* Cancel abandons the job but keeps the session (there is budget left). */
    check_res(research_guard_begin_job(&g, 50, &c), RESEARCH_GUARD_OK, "second job starts");
    check_res(research_guard_cancel(&g, 60), RESEARCH_GUARD_OK, "cancel accepted while running");
    check(g.state == RESEARCH_GUARD_ARMED, "cancel returns to armed");
    check_res(research_guard_cancel(&g, 70), RESEARCH_GUARD_ERR_STATE, "cancel while armed is refused");
}

static void test_pause_accounting(void)
{
    printf("== paused time does not consume the budget ==\n");
    research_guard_conditions_t c = healthy();
    research_guard_t g = armed_at_zero();

    check_res(research_guard_pause(&g, 0), RESEARCH_GUARD_ERR_STATE, "pausing an idle session is refused");

    research_guard_begin_job(&g, 100, &c);
    check_res(research_guard_pause(&g, 200), RESEARCH_GUARD_OK, "pause accepted while running");
    check(g.state == RESEARCH_GUARD_PAUSED, "state is paused");
    /* The session clock starts at arm, so 200 ms of armed+job time have passed. */
    check(research_guard_active_ms(&g, 200) == 200, "active time stops at the pause");

    /* A long pause must not be charged to the session. */
    check(research_guard_active_ms(&g, 50000) == 200, "a long pause does not accrue active time");

    check_res(research_guard_begin_job(&g, 50000, &c), RESEARCH_GUARD_ERR_STATE, "cannot start a job while paused");

    check_res(research_guard_resume(&g, 50100, &c), RESEARCH_GUARD_OK, "resume accepted");
    check(g.state == RESEARCH_GUARD_ARMED, "resume returns to armed, not running");
    check(research_guard_active_ms(&g, 50100) == 200, "active time excludes the pause");

    check_res(research_guard_resume(&g, 50200, &c), RESEARCH_GUARD_ERR_STATE, "resume while armed is refused");

    /* Resuming into an unsafe device must not reopen the session. */
    research_guard_begin_job(&g, 50300, &c);
    research_guard_pause(&g, 50400);
    c.temperature_c = 99.0f;
    check_res(research_guard_resume(&g, 50500, &c), RESEARCH_GUARD_ERR_THERMAL, "unsafe resume refused");
    check(g.state == RESEARCH_GUARD_PAUSED, "a refused resume stays paused, not armed");
}

static void test_budgets(void)
{
    printf("== budgets ==\n");
    research_guard_conditions_t c = healthy();

    /* Job budget: the last permitted job finishes and disarms the session. */
    research_guard_t g = fresh();
    g.cfg.max_jobs = 2;
    research_guard_offer_token(&g, 1, 0);
    research_guard_arm(&g, 1, 0, &c);

    check(research_guard_remaining_jobs(&g) == 2, "two jobs available");
    research_guard_begin_job(&g, 1, &c);
    research_guard_finish_job(&g, 2);
    check(research_guard_remaining_jobs(&g) == 1, "one job left");
    check(g.state == RESEARCH_GUARD_ARMED, "session continues after one job");

    research_guard_begin_job(&g, 3, &c);
    research_guard_finish_job(&g, 4);
    check(g.state == RESEARCH_GUARD_DISARMED, "session disarms when the job budget is spent");
    check(g.last_refusal == RESEARCH_GUARD_ERR_JOBS, "reason is the job budget");

    /* Re-arming needs a new token: the spent session cannot be extended. */
    check_res(research_guard_arm(&g, 1, 5, &c), RESEARCH_GUARD_ERR_TOKEN, "spent session cannot be re-armed with the old token");

    /* Time budget: active time crossing the limit during a job disarms it. */
    research_guard_t t = fresh();
    t.cfg.max_runtime_ms = 1000;
    t.cfg.max_jobs = 100;
    research_guard_offer_token(&t, 2, 0);
    research_guard_arm(&t, 2, 0, &c);
    research_guard_begin_job(&t, 10, &c);
    check_res(research_guard_finish_job(&t, 1200), RESEARCH_GUARD_OK, "an over-budget job still reports that it finished");
    check(t.state == RESEARCH_GUARD_DISARMED, "session disarms when the time budget is spent");
    check(t.last_refusal == RESEARCH_GUARD_ERR_TIME, "reason is the time budget");

    /* A job may not start on a session that is already out of time. */
    research_guard_t d = fresh();
    d.cfg.max_runtime_ms = 100;
    research_guard_offer_token(&d, 3, 0);
    research_guard_arm(&d, 3, 0, &c);
    check_res(research_guard_begin_job(&d, 500, &c), RESEARCH_GUARD_ERR_TIME, "late job refused");
    check(d.state == RESEARCH_GUARD_DISARMED, "late job attempt disarms");
}

static void test_watchdog(void)
{
    printf("== watchdog ==\n");
    research_guard_conditions_t c = healthy();
    research_guard_t g = armed_at_zero();

    check(research_guard_evaluate(&g, 10, &c) == RESEARCH_GUARD_ARMED, "healthy session stays armed");

    research_guard_begin_job(&g, 20, &c);
    c.pool_connected = true; /* the miner reconnected to a pool */
    check(research_guard_evaluate(&g, 30, &c) == RESEARCH_GUARD_DISARMED, "a reconnected pool disarms the session");
    check(g.last_refusal == RESEARCH_GUARD_ERR_POOL, "reason is the pool");

    /* A thermal trip mid-run disarms too, and the next request cannot resume. */
    research_guard_t h = armed_at_zero();
    c = healthy();
    research_guard_begin_job(&h, 20, &c);
    c.temperature_c = h.cfg.max_temperature_c + 5.0f;
    check(research_guard_evaluate(&h, 40, &c) == RESEARCH_GUARD_DISARMED, "an over-temperature reading disarms");
    check(h.last_refusal == RESEARCH_GUARD_ERR_THERMAL, "reason is thermal");
    research_guard_conditions_t ok = healthy();
    check_res(research_guard_begin_job(&h, 50, &ok), RESEARCH_GUARD_ERR_STATE, "a tripped session cannot just continue");

    /* Evaluate on a disarmed guard is inert. */
    check(research_guard_evaluate(&h, 60, &ok) == RESEARCH_GUARD_DISARMED, "disarmed guard stays disarmed");
    check(research_guard_evaluate(NULL, 0, &c) == RESEARCH_GUARD_DISARMED, "NULL guard reports disarmed");
}

static void test_strings(void)
{
    printf("== status strings ==\n");
    check(strcmp(research_guard_state_str(RESEARCH_GUARD_DISARMED), "disarmed") == 0, "disarmed");
    check(strcmp(research_guard_state_str(RESEARCH_GUARD_ARMED), "armed") == 0, "armed");
    check(strcmp(research_guard_state_str(RESEARCH_GUARD_RUNNING), "running") == 0, "running");
    check(strcmp(research_guard_state_str(RESEARCH_GUARD_PAUSED), "paused") == 0, "paused");
    check(strcmp(research_guard_result_str(RESEARCH_GUARD_OK), "ok") == 0, "ok");
    check(strcmp(research_guard_result_str(RESEARCH_GUARD_ERR_POOL), "pool_active") == 0, "pool_active");
    check(strcmp(research_guard_result_str(RESEARCH_GUARD_ERR_THERMAL), "thermal_unsafe") == 0, "thermal_unsafe");
    check(strcmp(research_guard_result_str(RESEARCH_GUARD_ERR_TOKEN), "no_valid_token") == 0, "no_valid_token");
    check(strcmp(research_guard_result_str((research_guard_result_t) 999), "unknown") == 0, "unknown result code");
}

/* ------------------------------------------------------- mining activity */

static void test_mining_active(void)
{
    printf("== mining activity (pause keeps the socket open) ==\n");
    check(!research_guard_mining_active(false, false, false), "idle and disconnected is not active");
    check(research_guard_mining_active(true, false, false), "connected and running is active");
    /* The device's pause does not close the stratum socket; the guard has to ask
     * whether a share can still leave, not whether a socket exists. */
    check(!research_guard_mining_active(true, true, false), "connected but paused is not active");
    check(research_guard_mining_active(true, true, true), "a share in flight still counts as active");
    check(research_guard_mining_active(false, false, true), "a share in flight blocks even without a socket");
}

static void test_thermal_age(void)
{
    printf("== thermal reading age ==\n");
    research_guard_conditions_t c;
    research_guard_t g;
    research_guard_config_t cfg = research_guard_config_default();

    /* A reading taken while the ASIC was running is an upper bound after the
     * pause: the die cannot get hotter with the ASIC powered down. It is only
     * accepted inside the configured window. */
    g = fresh();
    research_guard_offer_token(&g, 0x71, 0);
    c = healthy();
    c.thermal_age_ms = 0;
    check_res(research_guard_arm(&g, 0x71, 0, &c), RESEARCH_GUARD_OK, "a live reading is accepted");

    g = fresh();
    research_guard_offer_token(&g, 0x72, 0);
    c = healthy();
    c.thermal_age_ms = g.cfg.max_thermal_age_ms;
    check_res(research_guard_arm(&g, 0x72, 0, &c), RESEARCH_GUARD_OK, "a reading exactly at the age limit is accepted");

    g = fresh();
    research_guard_offer_token(&g, 0x73, 0);
    c = healthy();
    c.thermal_age_ms = g.cfg.max_thermal_age_ms + 1;
    check_res(research_guard_arm(&g, 0x73, 0, &c), RESEARCH_GUARD_ERR_THERMAL, "a stale reading is refused");

    /* The window can be tightened but not opened wide: a day-old reading must be
     * impossible to configure. */
    cfg = research_guard_config_default();
    cfg.max_thermal_age_ms = 86400000ull;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "a day-long thermal age is rejected");

    cfg = research_guard_config_default();
    cfg.max_thermal_age_ms = 1000ull;
    check(research_guard_init(&g, &cfg) == RESEARCH_GUARD_ERR_ARG, "an age limit under 30 s is rejected");
}

int main(void)
{
    test_config();
    test_token();
    test_conditions();
    test_job_cycle();
    test_pause_accounting();
    test_budgets();
    test_watchdog();
    test_mining_active();
    test_thermal_age();
    test_strings();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
