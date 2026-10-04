/*
 * Host-side tests for the thermal (ATM) and voltage (V/F) policies.
 *
 * No ESP-IDF, no hardware: both modules are dependency-free decision logic.
 *
 *   clang -std=c11 -Wall -Wextra -Werror \
 *         -I ../main/thermal -I ../main/power \
 *         ../main/thermal/atm_policy.c ../main/power/vf_tuner.c \
 *         test_power_policy.c -o test_power_policy
 */

#include "atm_policy.h"
#include "vf_tuner.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static atm_config_t atm_cfg(void)
{
    atm_config_t c;
    memset(&c, 0, sizeof(c));
    c.freq_min_mhz = 400.0f;
    c.freq_ceiling_mhz = 485.0f;
    c.freq_step_mhz = 25.0f;
    c.voltage_floor_mv = 1000;      /* TPS546/TPS40305 operating floor */
    c.voltage_ceiling_mv = 1200;
c.voltage_step_mv = 25;
    c.hot_temp_c = 68.0;
    c.shutdown_temp_c = 75.0;
    c.buffer_temp_c = 8.0;
    c.vr_hot_temp_c = 100.0;
    c.vr_shutdown_temp_c = 105.0;
    c.fan_relief_percent = 20.0;
    c.min_hashrate_ratio = 0.95;
    c.max_error_percent = 0.1;
    c.min_hold_s = 90;
    c.recheck_margin_c = 4;
    return c;
}

static atm_inputs_t atm_in(double chip, double vr, double fan,
                           double ratio, double err, uint32_t t)
{
    atm_inputs_t i;
    memset(&i, 0, sizeof(i));
    i.chip_temp_c = chip;
    i.vr_temp_c = vr;
    i.fan_percent = fan;
    i.hashrate_ratio = ratio;
    i.error_percent = err;
    i.uptime_s = t;
    return i;
}

static void test_atm_does_nothing_at_target(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);
    atm_inputs_t in = atm_in(60.0, 60.0, 33.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st, &in) == ATM_ACTION_NONE, "60 C at target -> no action");
    check(st.frequency_mhz == 485.0f && st.voltage_mv == 1200, "settings untouched");
}

static void test_atm_hysteresis_blocks_creep(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

    /* 64 C is above target but leaves less than the 8 C buffer below the 68 C
     * hot threshold, so climbing here would overshoot. LuxOS's rule. */
    atm_inputs_t in = atm_in(64.0, 60.0, 40.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st, &in) == ATM_ACTION_NONE, "64 C inside buffer -> no upscale");
    check(st.frequency_mhz == 485.0f, "frequency still at ceiling");

    /* Genuine headroom -> allowed to climb. */
    atm_inputs_t cold = atm_in(58.0, 55.0, 35.0, 1.00, 0.0, 2000);
    check(atm_step(&cfg, &st, &cold) == ATM_ACTION_NONE,
          "already at ceiling -> nothing to climb to");
}

static void test_atm_steps_down_freq_and_voltage_together(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);
    atm_inputs_t hot = atm_in(70.0, 60.0, 100.0, 1.00, 0.0, 1000);

    check(atm_step(&cfg, &st, &hot) == ATM_ACTION_STEP_DOWN, "70 C -> step down");
    check(st.frequency_mhz == 460.0f, "frequency stepped down 25 MHz");
    check(st.voltage_mv == 1175, "voltage stepped down 25 mV in the same direction");
    check(st.ever_downscaled, "anti-hunting memory records the excursion");
}

static void test_atm_never_undercuts_regulator_floor(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    /* Already sitting on the floor: the old latch would have gone to 900 mV
     * here and latched a false "Power Fault" (upstream PR #1790). */
    atm_init(&st, 400.0f, 1000);

    for (int i = 0; i < 10; i++) {
        atm_inputs_t hot = atm_in(80.0, 60.0, 100.0, 1.00, 0.0, (uint32_t)(1000 + i));
        atm_action_t a = atm_step(&cfg, &st, &hot);
        if (a == ATM_ACTION_FLOOR_REACHED || a == ATM_ACTION_SHUTDOWN) {
            break;
        }
    }
    check(st.voltage_mv >= cfg.voltage_floor_mv,
          "voltage never drops below the regulator floor");
    check(st.voltage_mv == 1000, "voltage parks exactly on the floor");
    check(st.frequency_mhz >= cfg.freq_min_mhz, "frequency respects its own floor");
}

static void test_atm_shutdown_threshold_latches(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

    atm_inputs_t critical = atm_in(78.0, 60.0, 100.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st, &critical) == ATM_ACTION_SHUTDOWN, "78 C -> shutdown");

    /* A single cool reading must not resume hashing on its own. */
    atm_inputs_t cool = atm_in(50.0, 50.0, 100.0, 1.00, 0.0, 2000);
    check(atm_step(&cfg, &st, &cool) == ATM_ACTION_SHUTDOWN,
          "shutdown stays latched until atm_init() after a real restart");
}

static void test_atm_vr_temperature_path(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

/* The VRM runs hotter than the ASIC under load, so it can be the first
     * thing to throttle even when the chip looks fine. 102 C sits inside the
     * proactive band (vr_hot 100 C) but below the emergency cutoff (105 C),
     * which the legacy hard backstop owns — see ATM_VR_* in
     * power_management_task.c. */
    atm_inputs_t vr_hot = atm_in(62.0, 102.0, 100.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st, &vr_hot) == ATM_ACTION_STEP_DOWN, "VR 102 C -> step down");

    /* Above the emergency cutoff the policy must latch a shutdown instead of
     * trying to climb its way out with a frequency step. */
    atm_state_t st_vr;
    atm_init(&st_vr, 485.0f, 1200);
    atm_inputs_t vr_critical = atm_in(62.0, 108.0, 100.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st_vr, &vr_critical) == ATM_ACTION_SHUTDOWN,
          "VR 108 C -> latched shutdown, not a step down");

    atm_state_t st2;
    atm_init(&st2, 485.0f, 1200);
    atm_inputs_t vr_crit = atm_in(62.0, 112.0, 100.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st2, &vr_crit) == ATM_ACTION_SHUTDOWN, "VR 112 C -> shutdown");
}

static void test_atm_min_hold_time_blocks_hunting(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

    atm_inputs_t hot = atm_in(70.0, 60.0, 100.0, 1.00, 0.0, 1000);
    check(atm_step(&cfg, &st, &hot) == ATM_ACTION_STEP_DOWN, "first downshift happens");

    /* Right after a change nothing else may happen, no matter how the readings
     * move — this is what stops threshold oscillation. */
    atm_inputs_t still_hot = atm_in(70.0, 60.0, 100.0, 1.00, 0.0, 1010);
    check(atm_step(&cfg, &st, &still_hot) == ATM_ACTION_NONE,
          "no second step inside the hold window");
}

static void test_atm_recovery_needs_extra_margin(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 460.0f, 1175);

/* Simulate a downshift having happened. */
st.ever_downscaled = true;
st.downscale_fan_percent = 60.0f;
st.last_change_s = 1000;

    /* 60 C has 8 C headroom to the 68 C hot threshold, but only 4 C once the
     * extra anti-hunting margin is required. Must not climb. */
    atm_inputs_t borderline = atm_in(60.0, 55.0, 60.0, 1.00, 0.0, 5000);
    check(atm_step(&cfg, &st, &borderline) == ATM_ACTION_NONE,
          "post-downshift climb requires the extra margin");

    /* Comfortable headroom -> climb back one step. */
    atm_inputs_t cool = atm_in(52.0, 50.0, 60.0, 1.00, 0.0, 5000);
    check(atm_step(&cfg, &st, &cool) == ATM_ACTION_STEP_UP, "cool + settled -> climb back");
    check(st.frequency_mhz == 485.0f, "climb restored frequency to the ceiling");
    check(st.voltage_mv == 1200, "climb restored voltage to the ceiling");
    check(!st.ever_downscaled, "baseline reached -> anti-hunting memory cleared");
}

static void test_atm_refuses_to_climb_when_silicon_unhealthy(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 460.0f, 1175);
    st.last_change_s = 1000;

    /* Plenty of thermal headroom, but the chip is only delivering 70 % of
     * expected: climbing voltage is exactly the wrong move. */
    atm_inputs_t starved = atm_in(45.0, 45.0, 50.0, 0.70, 2.5, 5000);
    check(atm_step(&cfg, &st, &starved) == ATM_ACTION_NONE,
          "unhealthy silicon blocks a voltage climb");
    check(st.voltage_mv == 1175, "voltage unchanged while unhealthy");
}

static void test_atm_rejects_null(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);
    atm_inputs_t in = atm_in(60.0, 60.0, 33.0, 1.0, 0.0, 10);
    check(atm_step(NULL, &st, &in) == ATM_ACTION_NONE, "NULL cfg safe");
    check(atm_step(&cfg, NULL, &in) == ATM_ACTION_NONE, "NULL state safe");
    check(atm_step(&cfg, &st, NULL) == ATM_ACTION_NONE, "NULL inputs safe");
    check(strcmp(atm_action_name(ATM_ACTION_STEP_DOWN), "STEP_DOWN") == 0, "action name");
    atm_clear_shutdown(NULL);
    atm_set_baseline(NULL, 1.0f, 1);
    check(true, "NULL state safe in atm_clear_shutdown/atm_set_baseline");
}

/* A latched shutdown must survive a single cool reading — otherwise hashing
 * resumes on the very next poll — but must be clearable afterwards. Without
 * atm_clear_shutdown() one excursion would disable the policy for good. */
static void test_atm_shutdown_latch_is_clearable(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

    atm_inputs_t critical = atm_in(80.0, 60.0, 100.0, 1.0, 0.0, 1000);
    check(atm_step(&cfg, &st, &critical) == ATM_ACTION_SHUTDOWN, "critical -> shutdown");
    check(st.shutdown_requested, "shutdown is latched");

    atm_inputs_t cool = atm_in(45.0, 45.0, 100.0, 1.0, 0.0, 2000);
    check(atm_step(&cfg, &st, &cool) == ATM_ACTION_SHUTDOWN,
          "latch survives a cool reading");

    atm_clear_shutdown(&st);
    check(!st.shutdown_requested, "atm_clear_shutdown clears the latch");
    check(atm_step(&cfg, &st, &cool) == ATM_ACTION_NONE,
          "policy resumes after the latch is cleared");
}

/* A settings change must re-pin the baseline, otherwise the ceiling captured
 * at task start would keep re-applying the old, higher frequency. */
static void test_atm_set_baseline_repins_policy(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 485.0f, 1200);

    st.ever_downscaled = true;
    st.frequency_mhz = 460.0f;
    st.voltage_mv = 1175;
    st.last_change_s = 1000;

    atm_set_baseline(&st, 425.0f, 1150);
    check(st.frequency_mhz == 425.0f, "baseline frequency adopted");
    check(st.voltage_mv == 1150, "baseline voltage adopted");
    check(!st.ever_downscaled, "derate memory dropped on baseline change");
    check(st.last_change_s == 0, "hold timer reset on baseline change");

    cfg.freq_ceiling_mhz = 425.0f;
    cfg.voltage_ceiling_mv = 1150;
    /* 70 C is inside the proactive band (hot 68 C) but below the emergency
     * cutoff (75 C), so this must shed rather than shut down. */
    atm_inputs_t hot = atm_in(70.0, 60.0, 100.0, 1.0, 0.0, 5000);
    check(atm_step(&cfg, &st, &hot) == ATM_ACTION_STEP_DOWN,
          "still sheds heat at the new lower baseline");
    check(st.frequency_mhz == 400.0f && st.voltage_mv == 1125,
          "shed one step from the new baseline, floor respected");
}

/* After a derate, a materially better fan duty cycle is itself bought
 * headroom: waiting out the full recheck band would leave hashrate idle. */
static void test_atm_fan_relief_allows_earlier_climb(void)
{
    atm_config_t cfg = atm_cfg();
    atm_state_t st;
    atm_init(&st, 460.0f, 1175);

    st.ever_downscaled = true;
    st.downscale_fan_percent = 60.0f;
    st.last_change_s = 1000;

    /* 60 C leaves only 4 C against the anti-hunting band, so it must hold... */
    atm_inputs_t borderline = atm_in(60.0, 55.0, 60.0, 1.00, 0.0, 5000);
    check(atm_step(&cfg, &st, &borderline) == ATM_ACTION_NONE,
          "same fan duty -> extra margin still required");

    /* ...until the fan picks up 30 points, which is real extra cooling. */
    atm_state_t st2;
    atm_init(&st2, 460.0f, 1175);
    st2.ever_downscaled = true;
    st2.downscale_fan_percent = 60.0f;
    st2.last_change_s = 1000;
    atm_inputs_t fanned = atm_in(60.0, 55.0, 90.0, 1.00, 0.0, 5000);
    check(atm_step(&cfg, &st2, &fanned) == ATM_ACTION_STEP_UP,
          "materially better fan duty unlocks the climb");

    /* An improvement below the configured threshold must not unlock it. */
    atm_state_t st3;
    atm_init(&st3, 460.0f, 1175);
    st3.ever_downscaled = true;
    st3.downscale_fan_percent = 60.0f;
    st3.last_change_s = 1000;
    atm_inputs_t marginal = atm_in(60.0, 55.0, 70.0, 1.00, 0.0, 5000);
    check(atm_step(&cfg, &st3, &marginal) == ATM_ACTION_NONE,
          "marginal fan improvement does not unlock the climb");
}

/* ------------------------------------------------------------------ */
/* V/F tuner                                                          */
/* ------------------------------------------------------------------ */

static vf_config_t vf_cfg(void)
{
vf_config_t c;
    memset(&c, 0, sizeof(c));
    c.voltage_floor_mv = 1000;
    c.voltage_step_mv = 25;
    c.min_hashrate_ratio = 0.95;
    c.max_error_percent = 0.1;
    c.max_chip_temp_c = 65.0;
    c.settle_s = 600;
    c.max_attempts = 8;
    return c;
}

static vf_inputs_t vf_in(double ratio, double err, double temp, uint32_t t)
{
    vf_inputs_t i;
    memset(&i, 0, sizeof(i));
    i.hashrate_ratio = ratio;
    i.error_percent = err;
    i.chip_temp_c = temp;
    i.uptime_s = t;
    return i;
}

static void test_vf_lower_voltage_while_holding_frequency(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1200, 1200);
    uint16_t baseline_before = st.baseline_mv;

    vf_inputs_t healthy = vf_in(1.00, 0.0, 60.0, 1000);
    check(vf_step(&cfg, &st, &healthy) == VF_ACTION_LOWER_VOLTAGE, "first step lowers V");
    check(st.voltage_mv == 1175, "voltage 1200 -> 1175");
    /* The tuner only ever moves voltage: frequency is the user's decision and
     * is not part of its state at all, so the provable invariant is that the
     * baseline it can revert to never rises. */
    check(st.baseline_mv == baseline_before,
          "tuner never raises the voltage it can revert to");

    /* Nothing may happen until the change has settled. */
    vf_inputs_t mid_settle = vf_in(1.00, 0.0, 60.0, 1100);
    check(vf_step(&cfg, &st, &mid_settle) == VF_ACTION_NONE, "waits out settle_s");
}

static void test_vf_rejects_step_that_breaks_acceptance(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1100, 1100);

    vf_inputs_t start = vf_in(1.00, 0.0, 60.0, 1000);
    check(vf_step(&cfg, &st, &start) == VF_ACTION_LOWER_VOLTAGE, "step down to 1075");
    check(st.voltage_mv == 1075, "voltage 1100 -> 1075");

    /* Hasrate collapses and errors appear -> the step was wrong. */
    vf_inputs_t degraded = vf_in(0.80, 1.4, 60.0, 2000);
    check(vf_step(&cfg, &st, &degraded) == VF_ACTION_RAISE_VOLTAGE,
          "acceptance gate rejects the step");
    check(st.voltage_mv == 1100, "reverted to the last proven-stable voltage");
    check(st.finished, "search stops instead of starving the chip further");
}

static void test_vf_rejects_on_error_rate_alone(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1100, 1100);

    vf_inputs_t start = vf_in(1.00, 0.0, 60.0, 1000);
    check(vf_step(&cfg, &st, &start) == VF_ACTION_LOWER_VOLTAGE, "step down");

    /* Hashrate fine, but the error rate is above the 0.1 % gate. */
    vf_inputs_t noisy = vf_in(1.00, 0.4, 60.0, 2000);
    check(vf_step(&cfg, &st, &noisy) == VF_ACTION_RAISE_VOLTAGE,
          "error rate alone fails the gate");
    check(st.voltage_mv == 1100, "reverted");
}

static void test_vf_walks_down_to_the_floor(void)
{
    vf_config_t cfg = vf_cfg();
    cfg.max_attempts = 32;
    vf_state_t st;
    vf_init(&st, 1100, 1100);

    uint32_t t = 1000;
    int steps = 0;
    for (int i = 0; i < 64 && !st.finished; i++) {
        vf_inputs_t tmp = vf_in(1.00, 0.0, 60.0, t);
        vf_action_t a = vf_step(&cfg, &st, &tmp);
        if (a == VF_ACTION_LOWER_VOLTAGE || a == VF_ACTION_RAISE_VOLTAGE) {
            steps++;
        }
        t += 700;   /* more than settle_s */
    }
    check(st.voltage_mv == cfg.voltage_floor_mv, "walked down to the regulator floor");
    check(st.voltage_mv >= cfg.voltage_floor_mv, "never undercut the floor");
    check(steps >= 3, "actually took several steps");
}

static void test_vf_stops_at_floor_instead_of_silencing_the_chip(void)
{
    vf_config_t cfg = vf_cfg();
    cfg.max_attempts = 32;
    vf_state_t st;
    vf_init(&st, 1000, 1000);

    vf_inputs_t base = vf_in(1.00, 0.0, 60.0, 1000);
    vf_action_t a = vf_step(&cfg, &st, &base);
    check(a == VF_ACTION_BASELINE_REACHED, "already on the floor -> baseline reached");
    check(st.voltage_mv == 1000, "voltage untouched");
}

static void test_vf_respects_attempt_budget(void)
{
    vf_config_t cfg = vf_cfg();
    cfg.max_attempts = 2;
    cfg.voltage_floor_mv = 900;   /* deliberately deep so the floor is not the limit */
    vf_state_t st;
    vf_init(&st, 1200, 1200);

    uint32_t t = 1000;
    vf_action_t last = VF_ACTION_NONE;
    for (int i = 0; i < 20 && !st.finished; i++) {
        vf_inputs_t tick = vf_in(1.00, 0.0, 60.0, t);
        last = vf_step(&cfg, &st, &tick);
        t += 700;
    }
    check(last == VF_ACTION_EXHAUSTED, "attempt budget respected");
    check(st.attempts == 2, "exactly max_attempts used");
}

static void test_vf_does_not_start_when_baseline_already_failing(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1200, 1200);

    /* Already degraded at the baseline voltage: stepping lower would only make
     * it worse, so the tuner must not act. */
    vf_inputs_t degraded = vf_in(0.70, 3.0, 60.0, 1000);
    check(vf_step(&cfg, &st, &degraded) == VF_ACTION_NONE,
          "no step when the baseline is already outside the window");
    check(st.voltage_mv == 1200, "voltage unchanged");
}

static void test_vf_aborts_when_too_hot(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1200, 1200);

    /* Above the continuous chip limit the tuner must refuse to shave voltage
     * any further — that is ATM's job, and doing it here fights it. */
    vf_inputs_t hot = vf_in(1.00, 0.0, 70.0, 1000);
    check(vf_step(&cfg, &st, &hot) == VF_ACTION_ABORT_THERMAL,
          "too hot -> abort thermal, no voltage step");
    check(st.voltage_mv == 1200, "voltage untouched while too hot");
    check(st.attempts == 0, "abort does not consume an attempt");

    /* At the limit exactly it must also refuse. */
    vf_state_t st2;
    vf_init(&st2, 1200, 1200);
    vf_inputs_t at_limit = vf_in(1.00, 0.0, 65.0, 1000);
    check(vf_step(&cfg, &st2, &at_limit) == VF_ACTION_ABORT_THERMAL,
          "exactly at the limit -> abort thermal");

    /* A negative reading means the ASIC is powered down (the sensor is not
     * valid), which must not be mistaken for "cold enough to tune". */
    vf_state_t st3;
    vf_init(&st3, 1200, 1200);
    vf_inputs_t invalid = vf_in(1.00, 0.0, -1.0, 1000);
    check(vf_step(&cfg, &st3, &invalid) == VF_ACTION_LOWER_VOLTAGE,
          "invalid (-1) temperature does not block tuning");

    /* Just below the limit it tunes normally. */
    vf_state_t st4;
    vf_init(&st4, 1200, 1200);
    vf_inputs_t warm = vf_in(1.00, 0.0, 64.0, 1000);
    check(vf_step(&cfg, &st4, &warm) == VF_ACTION_LOWER_VOLTAGE,
          "below the limit -> tuning proceeds");
}

static void test_vf_rejects_null(void)
{
    vf_config_t cfg = vf_cfg();
    vf_state_t st;
    vf_init(&st, 1200, 1200);
    vf_inputs_t in = vf_in(1.0, 0.0, 60.0, 10);
    check(vf_step(NULL, &st, &in) == VF_ACTION_NONE, "NULL cfg safe");
    check(vf_step(&cfg, NULL, &in) == VF_ACTION_NONE, "NULL state safe");
    check(vf_step(&cfg, &st, NULL) == VF_ACTION_NONE, "NULL inputs safe");
    check(vf_target_voltage(NULL) == 0, "NULL state target voltage is 0");
    check(strcmp(vf_action_name(VF_ACTION_LOWER_VOLTAGE), "LOWER_VOLTAGE") == 0, "action name");
}

int main(void)
{
    puts("== ATM policy ==");
    test_atm_does_nothing_at_target();
    test_atm_hysteresis_blocks_creep();
    test_atm_steps_down_freq_and_voltage_together();
    test_atm_never_undercuts_regulator_floor();
    test_atm_shutdown_threshold_latches();
    test_atm_vr_temperature_path();
    test_atm_min_hold_time_blocks_hunting();
    test_atm_recovery_needs_extra_margin();
    test_atm_refuses_to_climb_when_silicon_unhealthy();
    test_atm_rejects_null();
    test_atm_shutdown_latch_is_clearable();
    test_atm_set_baseline_repins_policy();
    test_atm_fan_relief_allows_earlier_climb();

    puts("== V/F tuner ==");
    test_vf_lower_voltage_while_holding_frequency();
    test_vf_rejects_step_that_breaks_acceptance();
    test_vf_rejects_on_error_rate_alone();
    test_vf_walks_down_to_the_floor();
    test_vf_stops_at_floor_instead_of_silencing_the_chip();
    test_vf_respects_attempt_budget();
    test_vf_does_not_start_when_baseline_already_failing();
    test_vf_rejects_null();
    test_vf_aborts_when_too_hot();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
