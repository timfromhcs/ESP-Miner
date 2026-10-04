#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "nvs_config.h"
#include "vcore.h"
#include "thermal.h"
#include "power.h"
#include "asic.h"
#include "utils.h"
#include "asic_init.h"
#include "asic_reset.h"
#include "atm_policy.h"
#include "vf_tuner.h"
#include "power_target.h"
#include "driver/uart.h"

#define POLL_RATE 100
#define MAX_TEMP 90.0
#define THROTTLE_TEMP 75.0
#define SAFE_TEMP 45.0

#define VOLTAGE_START_THROTTLE 4900
#define VOLTAGE_MIN_THROTTLE 3500
#define VOLTAGE_RANGE (VOLTAGE_START_THROTTLE - VOLTAGE_MIN_THROTTLE)

#define TPS546_THROTTLE_TEMP 105.0
#define TPS546_MAX_TEMP 145.0

#define ASIC_REDUCTION 100.0

/*
 * ATM / V/F policy tuning.
 *
 * ATM_HOT_TEMP sits *below* THROTTLE_TEMP on purpose: ATM sheds one 25 MHz /
 * 25 mV step while the situation is still recoverable, so that the hard backstop
 * further down (which halts hashing entirely, forces a cooldown and drops the
 * ASIC by a full 100 MHz / 100 mV) should almost never fire. The board's
 * documented continuous limits are chip <= 65 C and regulator <= 85 C, with the
 * dashboard warning at 70 / 105 C.
 */
#define ATM_HOT_TEMP 68.0
#define ATM_SHUTDOWN_TEMP 75.0
#define ATM_BUFFER_TEMP 8.0
#define ATM_RECHECK_MARGIN_TEMP 4.0
#define ATM_MIN_HOLD_S 90
#define ATM_FREQ_STEP_MHZ 25.0f
#define ATM_VOLTAGE_STEP_MV 25

/* Do not climb unless the temperature has genuinely settled. A dwell time alone
 * cannot distinguish "stable" from "quiet at this instant", and climbing into a
 * still-moving reading is how a controller overshoots. Braiins' DPS requires
 * "did not fluctuate by more than 1 C in the last minute"; we use 2 C over 60 s to
 * allow for sensor granularity on this board. */
#define ATM_STABILITY_WINDOW_TEMP 2.0
#define ATM_STABILITY_WINDOW_S 60

/* The first minutes are a distinct thermal regime: the rail, the die and the
 * cooler are all still settling. If the chip is already at the hot threshold
 * this early, ease it down to 66 % of the ceiling immediately instead of
 * crawling down one step per hold period. Braiins uses the same 66 % / 5 min. */
#define ATM_STARTUP_WINDOW_S 300
#define ATM_STARTUP_DERATE_PERCENT 66

/* A 20-point fan-duty improvement counts as bought thermal headroom and lets ATM
 * undo a derate without waiting out the full recheck band. */
#define ATM_FAN_RELIEF_PERCENT 20.0

/* TPS546 self-shutdown is 145 C; 100 C is where the regulator is already losing
 * headroom on its own thermal budget, so ATM starts shedding there. The VR
 * emergency stop itself is left to the legacy backstop at TPS546_THROTTLE_TEMP
 * (105 C) so that exactly one component owns the hard VR cutoff. */
#define ATM_VR_HOT_TEMP 100.0
#define ATM_VR_SHUTDOWN_TEMP 105.0

/* Acceptance gate shared with the V/F tuner. */
#define STABILITY_MIN_HASHRATE_RATIO 0.95
#define STABILITY_MAX_ERROR_PERCENT 0.10

/* Above the board's documented continuous chip limit (65 C) the V/F search is
 * abandoned outright. Shaving voltage into a thermal excursion would make the
 * excursion worse, and that is ATM's job to resolve, not the tuner's. */
#define VF_MAX_TUNE_TEMP 65.0

/* Power targeting tuning. The budget is operator-supplied; these bound the
 * search. Tolerance is generous because INA260 readings at this power level carry
 * tens of milliwatts of noise, and a tighter deadband would make the loop chase
 * its own measurement. */
#define PT_TOLERANCE_MW 500
#define PT_STEP_MW 100
#define PT_SETTLE_S 300
#define PT_MIN_HOLD_S 120
#define PT_MIN_MW 5000
#define PT_MAX_MW 30000

/* The regulator cannot be driven below this, no matter what the thermal policy
 * asks for. Stepping under it is what produced the false "Power Fault" latch. */
#define ASIC_VOLTAGE_REGULATOR_FLOOR_MV 1000

/* Temperature sanity window. A BM1366 junction cannot be below ambient and
 * certainly not below -40 C; anything outside this is a sensor fault, not a
 * measurement. -1 is the codebase's "ASIC powered down / not valid" sentinel. */
#define TEMP_PLAUSIBLE_MIN_C (-40.0)
#define TEMP_PLAUSIBLE_MAX_C 150.0
#define BAD_READING_MAX 10

static bool is_plausible_temp(double t)
{
    return t >= TEMP_PLAUSIBLE_MIN_C && t <= TEMP_PLAUSIBLE_MAX_C;
}

static const char * TAG = "power_management";

static float atm_ceiling_frequency(void)
{
    float f = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
    return (f > 0.0f) ? f : 400.0f;
}

static uint16_t atm_ceiling_voltage(void)
{
    uint16_t v = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    return (v > ASIC_VOLTAGE_REGULATOR_FLOOR_MV) ? v : ASIC_VOLTAGE_REGULATOR_FLOOR_MV;
}

static void mining_stop(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Stopping mining");

    // Wind frequency down to 50 MHz before cutting power. This also updates
    // the transition tracker so the ramp starts from 50 MHz on next start,
    // rather than the stale pre-reset frequency.
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;

    ASIC_set_frequency(GLOBAL_STATE);
    ASIC_set_nonce_space(GLOBAL_STATE);

    // Cut ASIC power and hold in reset
    VCORE_set_voltage(GLOBAL_STATE, 0.0f);
    asic_hold_reset_low();

    // Mark uninitialized immediately so tasks stop issuing UART commands
    GLOBAL_STATE->ASIC_initalized = false;

    // Give tasks time to complete any in-progress UART operation
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Flush any stale data from the UART buffers
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Mining stopped");
}

static uint8_t mining_start(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Starting mining");

    // Restore voltage from NVS
    uint16_t voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    VCORE_set_voltage(GLOBAL_STATE, (double) voltage / 1000.0);

    // Wait for voltage to stabilize before touching the ASIC
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Clear any accumulated UART garbage before init
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    // Stabilization delay of 2000ms prevents race conditions where tasks are
    // just starting to use the ASIC while power management tries to change frequency
    uint8_t chip_count = asic_initialize(GLOBAL_STATE, ASIC_INIT_RECOVERY, 2000);

    if (chip_count > 0) {
        ESP_LOGI(TAG, "Mining started successfully (%d chip(s))", chip_count);
    } else {
        ESP_LOGE(TAG, "Mining start failed - ASIC not detected");
    }

    return chip_count;
}

static float expected_hashrate(GlobalState * GLOBAL_STATE)
{
    return GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value * GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count * GLOBAL_STATE->DEVICE_CONFIG.family.asic_count / 1000.0;
}

void POWER_MANAGEMENT_init_frequency(GlobalState * GLOBAL_STATE)
{
    float frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);

    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = frequency;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency = 50.0;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = expected_hashrate(GLOBAL_STATE);
    
    char expected_hashrate_str[16] = {0};
    suffixString(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate * 1e6, expected_hashrate_str, sizeof(expected_hashrate_str), 0);
    ESP_LOGI(TAG, "ASIC Frequency: %g MHz, Expected hashrate: %sH/s", frequency, expected_hashrate_str);
}

void POWER_MANAGEMENT_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    
    float last_asic_frequency = power_management->frequency_value;

    vTaskDelay(500 / portTICK_PERIOD_MS);
    uint16_t last_core_voltage = 0.0;

    uint16_t last_known_asic_voltage = 0;
    float last_known_asic_frequency = 0.0;
    bool is_paused = false;
    int bad_reading_count = 0;

    /* The user's configured values are the ceiling; ATM may only work below it
     * and restores back up to it, never above. That keeps the user's own
     * overclock decision authoritative. */
    const float atm_freq_ceiling = atm_ceiling_frequency();
    const uint16_t atm_voltage_ceiling = atm_ceiling_voltage();

    atm_config_t atm_cfg = {
        .freq_min_mhz = 400.0f,
        .freq_ceiling_mhz = atm_freq_ceiling,
        .freq_step_mhz = ATM_FREQ_STEP_MHZ,
        .voltage_floor_mv = ASIC_VOLTAGE_REGULATOR_FLOOR_MV,
        .voltage_ceiling_mv = atm_voltage_ceiling,
        .voltage_step_mv = ATM_VOLTAGE_STEP_MV,
        .hot_temp_c = ATM_HOT_TEMP,
        .shutdown_temp_c = ATM_SHUTDOWN_TEMP,
        .buffer_temp_c = ATM_BUFFER_TEMP,
        .stability_window_c = ATM_STABILITY_WINDOW_TEMP,
        .stability_window_s = ATM_STABILITY_WINDOW_S,
        .startup_window_s = ATM_STARTUP_WINDOW_S,
        .startup_derate_percent = ATM_STARTUP_DERATE_PERCENT,
        .vr_hot_temp_c = ATM_VR_HOT_TEMP,
        .vr_shutdown_temp_c = ATM_VR_SHUTDOWN_TEMP,
        .fan_relief_percent = ATM_FAN_RELIEF_PERCENT,
        .min_hashrate_ratio = STABILITY_MIN_HASHRATE_RATIO,
        .max_error_percent = STABILITY_MAX_ERROR_PERCENT,
        .min_hold_s = ATM_MIN_HOLD_S,
        .recheck_margin_c = ATM_RECHECK_MARGIN_TEMP,
    };

    atm_state_t atm_state;
    atm_init(&atm_state, atm_freq_ceiling, atm_voltage_ceiling);

    /* V/F tuner: frequency is the user's decision and stays fixed, voltage is
     * what we minimise. Seeded at the configured pair. */
    vf_config_t vf_cfg = {
        .voltage_floor_mv = ASIC_VOLTAGE_REGULATOR_FLOOR_MV,
        .voltage_step_mv = 25,
        .min_hashrate_ratio = STABILITY_MIN_HASHRATE_RATIO,
        .max_error_percent = STABILITY_MAX_ERROR_PERCENT,
        .max_chip_temp_c = VF_MAX_TUNE_TEMP,
        .settle_s = 600,
        .max_attempts = 8,
    };
    vf_state_t vf_state;
    vf_init(&vf_state, atm_voltage_ceiling, atm_voltage_ceiling);

    /* Power targeting: opt-in, off unless the operator configured a budget. */
    uint16_t power_management_target_mw = nvs_config_get_u16(NVS_CONFIG_POWER_TARGET_MW);
    bool power_target_enabled = nvs_config_get_bool(NVS_CONFIG_POWER_TARGET_ENABLED)
                                && power_management_target_mw > 0;
    pt_config_t pt_cfg = {
        .target_mw = power_management_target_mw,
        .tolerance_mw = PT_TOLERANCE_MW,
        .step_mw = PT_STEP_MW,
        .settle_s = PT_SETTLE_S,
        .min_hold_s = PT_MIN_HOLD_S,
        .min_mw = PT_MIN_MW,
        .max_mw = PT_MAX_MW,
    };
    pt_state_t pt_state;
    pt_init(&pt_state, power_management_target_mw);
    if (power_target_enabled) {
        ESP_LOGI(TAG, "Power targeting enabled: budget %umW (tolerance +/-%umW, step %umW)",
                 power_management_target_mw, PT_TOLERANCE_MW, PT_STEP_MW);
    }

    while (1) {
        if (GLOBAL_STATE->SELF_TEST_MODULE.is_finished) {
            ESP_LOGI(TAG, "Stopped");
            vTaskDelete(NULL);
            return;
        }

        power_management->voltage = Power_get_input_voltage(GLOBAL_STATE);
        Power_get_output(GLOBAL_STATE, &power_management->power, &power_management->current);
        power_management->core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);

        power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
        power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);

        power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
        // User pause, hardware fault, or all pools unreachable
        bool wants_stop = sys_module->mining_paused || sys_module->hardware_fault || sys_module->pools_unavailable;
        if (wants_stop && !is_paused) {
            mining_stop(GLOBAL_STATE);
            is_paused = true;
        } else if (!wants_stop && is_paused) {
            mining_start(GLOBAL_STATE);
            is_paused = false;
        }

        // If we've paused or have a hardware fault, skip doing anything else
        if (is_paused || sys_module->hardware_fault) {
            vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
            continue;
        }

        bool asic_overheat =
            power_management->chip_temp_avg > THROTTLE_TEMP
            || power_management->chip_temp2_avg > THROTTLE_TEMP;

        /* ----------------------------------------------------------------
         * ATM — proactive thermal management.
         *
         * Deliberately placed *before* the hard backstop below so a survivable
         * excursion costs one 25 MHz / 25 mV step instead of a full mining stop,
         * a forced 30 s cooldown and a permanent 100 MHz / 100 mV reduction
         * written to NVS. The ceiling is always the user's configured value, so
         * ATM can shed and restore but never overclock on its own.
         * ---------------------------------------------------------------- */
        if (!is_paused && !sys_module->hardware_fault) {
            /* Keep ATM's ceiling pinned to the user's *current* setting. The
             * ceiling was captured once at task start; if the user edits
             * frequency/voltage in AxeOS afterwards, a stale ceiling would keep
             * re-applying the old, higher value and silently fight the change.
             * Only adopt a new baseline while no derate is outstanding, so a
             * transient excursion can never redefine the user's setting. */
            float cur_freq_ceiling = atm_ceiling_frequency();
            uint16_t cur_voltage_ceiling = atm_ceiling_voltage();
            if (!atm_state.ever_downscaled
                && (cur_freq_ceiling != atm_cfg.freq_ceiling_mhz
                    || cur_voltage_ceiling != atm_cfg.voltage_ceiling_mv)) {
                atm_cfg.freq_ceiling_mhz = cur_freq_ceiling;
                atm_cfg.voltage_ceiling_mv = cur_voltage_ceiling;
                atm_set_baseline(&atm_state, cur_freq_ceiling, cur_voltage_ceiling);
                ESP_LOGI(TAG, "ATM baseline follows settings: %.0f MHz / %umV",
                         cur_freq_ceiling, cur_voltage_ceiling);
            }

            double hottest = power_management->chip_temp_avg;
            if (power_management->chip_temp2_avg > hottest) {
                hottest = power_management->chip_temp2_avg;
            }

            /* Sensor quorum. A dead or shorted sensor reports a plausible-looking
             * number, and a controller that trusts it will happily under-clock a
             * healthy chip or fail to protect a hot one. Reject readings outside
             * any physically possible range, and if the ASIC sensors stay bad,
             * fall back to the regulator temperature - a different sensor on a
             * different bus - rather than flying blind. LuxOS ships the same idea
             * (Required Critical Temperature Sensors per Board, Max Bad Readings
             * 10, Bad Avg Threshold 2). */
            double t1 = power_management->chip_temp_avg;
            double t2 = power_management->chip_temp2_avg;
            bool t1_ok = is_plausible_temp(t1);
            bool t2_ok = is_plausible_temp(t2);

            if (!t1_ok && !t2_ok) {
                bad_reading_count++;
                if (bad_reading_count >= BAD_READING_MAX) {
                    double vr = power_management->vr_temp;
                    if (is_plausible_temp(vr)) {
                        ESP_LOGE(TAG, "ATM: no valid ASIC sensor after %d bad readings (t1=%.1f t2=%.1f) - using VR %.1f C",
                                 bad_reading_count, t1, t2, vr);
                        t1_ok = true;
                        t1 = vr;
                        bad_reading_count = 0;
                    } else {
                        ESP_LOGE(TAG, "ATM: no valid temperature sensor at all (t1=%.1f t2=%.1f vr=%.1f) - holding operating point",
                                 t1, t2, vr);
                    }
                }
            } else {
                if (bad_reading_count > 0) {
                    ESP_LOGW(TAG, "ATM: temperature sensors recovered after %d bad readings", bad_reading_count);
                }
                bad_reading_count = 0;
            }

            if (!t1_ok) {
                t1 = t2_ok ? t2 : -1;
            }
            if (!t2_ok) {
                t2 = t1;
            }

            hottest = t1;

            float expected = power_management->expected_hashrate;
            float measured = sys_module->hashrate_1h;
            double ratio = (expected > 0.0f && measured > 0.0f)
                         ? (double) measured / (double) expected
                         : 0.0;

            atm_inputs_t atm_inputs = {
                .chip_temp_c = hottest,
                .vr_temp_c = power_management->vr_temp,
                .fan_percent = power_management->fan_perc,
                .hashrate_ratio = ratio,
                .error_percent = sys_module->error_percentage,
                .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            };

            atm_action_t atm_action = atm_step(&atm_cfg, &atm_state, &atm_inputs);

            if (atm_action == ATM_ACTION_STEP_DOWN || atm_action == ATM_ACTION_STEP_UP) {
                ESP_LOGW(TAG, "ATM %s -> %.0f MHz / %umV (chip %.1fC VR %.1fC fan %.0f%% ratio %.2f)",
                         atm_action_name(atm_action), atm_state.frequency_mhz,
                         atm_state.voltage_mv, hottest, power_management->vr_temp,
                         power_management->fan_perc, ratio);
                /* Deliberately NOT written to NVS. NVS holds the user's own
                 * setting and stays the single source of truth; the derate lives
                 * only in atm_state and is applied by the override further down.
                 * Persisting it here would make one transient thermal event cost
                 * hashrate across a reboot, which is defect #2 this module was
                 * written to remove. */
            } else if (atm_action == ATM_ACTION_FLOOR_REACHED) {
                ESP_LOGE(TAG, "ATM at floor (%.0f MHz / %umV) and still %.1fC — the hard backstop takes over",
                         atm_state.frequency_mhz, atm_state.voltage_mv, hottest);
            } else if (atm_action == ATM_ACTION_SHUTDOWN) {
                ESP_LOGE(TAG, "ATM shutdown latched (chip %.1fC VR %.1fC)", hottest,
                         power_management->vr_temp);
            }
        }

        if ((power_management->vr_temp > TPS546_THROTTLE_TEMP || asic_overheat) && (power_management->frequency_value > 50 || power_management->voltage > 1000)) {
            if (power_management->chip_temp2_avg > 0) {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC1: %fC ASIC2: %fC", power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
            } else {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC: %fC", power_management->vr_temp, power_management->chip_temp_avg);
            }

            last_known_asic_voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
            last_known_asic_frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
            nvs_config_set_bool(NVS_CONFIG_AUTO_FAN_SPEED, false);
            nvs_config_set_u16(NVS_CONFIG_MANUAL_FAN_SPEED, 100);
            nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true);
            ESP_LOGW(TAG, "Entering safe mode due to overheat condition. System operation halted.");
            mining_stop(GLOBAL_STATE);
            
            // Note: ASIC temperature readings are invalid when ASIC is powered down (returns -1)
            // For 600-series boards that use ASIC thermal diode, we rely on VR temp and fixed cooling time
            // For boards with EMC internal temp sensor, readings remain valid
            bool asic_temp_valid = GLOBAL_STATE->DEVICE_CONFIG.emc_internal_temp;
            int cooling_cycles = 0;
            const int MIN_COOLING_CYCLES = 6; // Minimum 30 seconds cooling
            
            while (cooling_cycles < MIN_COOLING_CYCLES || power_management->vr_temp > TPS546_THROTTLE_TEMP - 10) {
                vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait 5 seconds
                cooling_cycles++;
                
                power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
                
                // Only check ASIC temps if they're valid (not using ASIC thermal diode)
                if (asic_temp_valid) {
                    power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
                    power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);
                    ESP_LOGW(TAG, "Safe mode active (cycle %d) - VR: %.1f°C ASIC1: %.1f°C ASIC2: %.1f°C",
                             cooling_cycles, power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
                    
                    // Continue if ASIC temps still too high
                    if (power_management->chip_temp_avg >  SAFE_TEMP || power_management->chip_temp2_avg > SAFE_TEMP) {
                        cooling_cycles = 0; // Reset cycle count if still hot
                    }
                } else {
                    // For boards using ASIC thermal diode (600 series), rely on VR temp and time
                    ESP_LOGW(TAG, "Safe mode active (cycle %d/%d) - VR: %.1f°C (ASIC temps unavailable while powered down)",
                             cooling_cycles, MIN_COOLING_CYCLES, power_management->vr_temp);
                }
            }
            ESP_LOGI(TAG, "Temperature normalized after %d cooling cycles. Reinitializing ASIC...", cooling_cycles);
            
            uint16_t reduced_voltage = last_known_asic_voltage > ASIC_REDUCTION ? last_known_asic_voltage - ASIC_REDUCTION : 1000;
            float reduced_asic_frequency = last_known_asic_frequency > ASIC_REDUCTION ? last_known_asic_frequency - ASIC_REDUCTION : 400.0;

            /* A flat -100 mV step is not safe on every board: 1000 mV - 100 mV
             * lands below the TPS546/TPS40305 operating floor and latches a bogus
             * "Power Fault — check your power supply" with a perfectly healthy
             * PSU. Clamp to the regulator floor. */
            if (reduced_voltage < ASIC_VOLTAGE_REGULATOR_FLOOR_MV) {
                ESP_LOGW(TAG, "Overheat derate would drive Vcore to %umV, clamping to %umV regulator floor",
                         reduced_voltage, ASIC_VOLTAGE_REGULATOR_FLOOR_MV);
                reduced_voltage = ASIC_VOLTAGE_REGULATOR_FLOOR_MV;
            }
            
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, reduced_voltage);
            nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, reduced_asic_frequency);
            
            ESP_LOGI(TAG, "Restoring at reduced settings: %umV (was %umV), %.0f MHz (was %.0f MHz)",
                     reduced_voltage, last_known_asic_voltage, reduced_asic_frequency, last_known_asic_frequency);

            uint8_t chip_count = mining_start(GLOBAL_STATE);

            if (chip_count > 0) {
                // Frequency reduction will now be applied by normal power management loop
                nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, false);
                ESP_LOGI(TAG, "Resuming normal operation. Reduced frequency (%.0f MHz) will be applied automatically.", reduced_asic_frequency);
            }

            /* The cooldown above is a real restart, so ATM's latched shutdown
             * has served its purpose. Clearing it here is mandatory: the latch
             * is sticky by design, and without this one overheat event would
             * silently disable thermal management for the rest of the task's
             * life. Re-pin the baseline onto the values actually written to NVS
             * so ATM tracks the post-derate operating point. */
            atm_clear_shutdown(&atm_state);
            atm_set_baseline(&atm_state, atm_ceiling_frequency(), atm_ceiling_voltage());
            ESP_LOGI(TAG, "ATM latch cleared after cooldown, baseline %.0f MHz / %umV",
                     atm_state.frequency_mhz, atm_state.voltage_mv);
        }

        uint16_t core_voltage = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                 ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv
                                 : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
        float asic_frequency = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                 ? GLOBAL_STATE-> DEVICE_CONFIG.family.asic.default_frequency_mhz
                                 : nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);

        /* ATM override: while a thermal derate is outstanding, the policy's
         * in-memory pair replaces the NVS values for this cycle. NVS itself is
         * left untouched, so the user's configured ceiling survives both the
         * excursion and a reboot. Once ATM climbs back to the ceiling,
         * ever_downscaled clears and the NVS values take over again. */
        if (atm_state.ever_downscaled) {
            core_voltage = atm_state.voltage_mv;
            asic_frequency = atm_state.frequency_mhz;
        }

        if (core_voltage != last_core_voltage) {
            ESP_LOGI(TAG, "setting new vcore voltage to %umV", core_voltage);
            VCORE_set_voltage(GLOBAL_STATE, (double) core_voltage / 1000.0);
            last_core_voltage = core_voltage;
        }

        if (asic_frequency != last_asic_frequency) {
            ESP_LOGI(TAG, "New ASIC frequency requested: %g MHz (current: %g MHz)", asic_frequency, last_asic_frequency);
            
            power_management->frequency_value = asic_frequency;
            power_management->expected_hashrate = expected_hashrate(GLOBAL_STATE);

            ASIC_set_frequency(GLOBAL_STATE);
            ASIC_set_nonce_space(GLOBAL_STATE);
            
            last_asic_frequency = asic_frequency;

            // The user moved frequency, so the previously proven voltage is no
            // longer a valid baseline for the tuner. Only react to *user* changes:
            // an ATM-originated step must not keep resetting the tuner, or the two
            // controllers would livelock each other.
            if (!atm_state.ever_downscaled) {
                vf_init(&vf_state, core_voltage, core_voltage);
            }
        }

        /* ----------------------------------------------------------------
         * V/F autotuner — hold the frequency, minimise the voltage.
         *
         * Opt-in via NVS_CONFIG_AUTOTUNE_VOLTAGE because it writes to NVS. It
         * never changes frequency, never drops below the regulator floor, and
         * reverts to the last proven-stable voltage as soon as the acceptance
         * gate (>= 95 % of expected hashrate, <= 0.1 % errors) is missed.
         * ---------------------------------------------------------------- */
        if (nvs_config_get_bool(NVS_CONFIG_AUTOTUNE_VOLTAGE) && !is_paused
            && !sys_module->hardware_fault && GLOBAL_STATE->ASIC_initalized) {

            float expected = power_management->expected_hashrate;
            float measured = sys_module->hashrate_1h;
            double ratio = (expected > 0.0f && measured > 0.0f)
                         ? (double) measured / (double) expected
                         : 0.0;

            vf_inputs_t vf_inputs = {
                .hashrate_ratio = ratio,
                .error_percent = sys_module->error_percentage,
                .chip_temp_c = power_management->chip_temp_avg,
                .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            };

            vf_action_t vf_action = vf_step(&vf_cfg, &vf_state, &vf_inputs);
            uint16_t target = vf_target_voltage(&vf_state);

            if (vf_action != VF_ACTION_NONE && target != core_voltage) {
                ESP_LOGI(TAG, "VF tuner %s -> %umV (ratio %.2f errors %.2f%%)",
                         vf_action_name(vf_action), target, ratio,
                         sys_module->error_percentage);
                VCORE_set_voltage(GLOBAL_STATE, (double) target / 1000.0);
                nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, target);
                last_core_voltage = target;
            }
            if (vf_action == VF_ACTION_BASELINE_REACHED || vf_action == VF_ACTION_EXHAUSTED
     || vf_action == VF_ACTION_ABORT_THERMAL) {
                nvs_config_set_bool(NVS_CONFIG_AUTOTUNE_VOLTAGE, false);
                ESP_LOGI(TAG, "VF tuner finished (%s) at %umV, autotuning disabled",
                         vf_action_name(vf_action), target);
            }
        }

        // Check for changing of overheat mode
        bool new_overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);
        
        if (new_overheat_mode != sys_module->overheat_mode) {
            sys_module->overheat_mode = new_overheat_mode;
            ESP_LOGI(TAG, "Overheat mode updated to: %d", sys_module->overheat_mode);
        }

        /* ----------------------------------------------------------------
         * Power targeting — hold a power budget instead of a frequency.
         *
         * Opt-in and off by default. LuxOS and Braiins both anchor on watts rather
         * than temperature, and the reason is measurable: LuxOS reports power draw
         * varying >10 % through the day at identical settings purely from ambient.
         * That swing is the whole prize, and the fan PID above cannot see it.
         *
         * It composes with ATM rather than competing: ATM owns the thermal envelope
         * and the safety backstops, and while it is holding a derate this loop stays
         * out of the way entirely. The budget is nudged by trimming the ceiling the
         * tuner is allowed to work to, so the mechanism underneath (frequency and
         * voltage moving together, regulator floor respected) is unchanged.
         * ---------------------------------------------------------------- */
        if (power_target_enabled && !is_paused && !sys_module->hardware_fault
            && GLOBAL_STATE->ASIC_initalized && !atm_state.ever_downscaled) {

            uint32_t measured_mw = (uint32_t)(power_management->power * 1000.0f);
            pt_inputs_t pt_inputs = {
                .measured_mw = measured_mw,
                .power_valid = measured_mw > 0,
                .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            };

            pt_action_t pt_action = pt_step(&pt_cfg, &pt_state, &pt_inputs);
            if (pt_action == PT_ACTION_SHED || pt_action == PT_ACTION_RECLAIM) {
                /* Translate the power budget into a frequency ceiling. The linear
                 * map is deliberately conservative: it is a ceiling, never a
                 * setpoint, so ATM and the V/F tuner still own the real operating
                 * point within it. */
                uint32_t budget = pt_target_mw(&pt_state);
                float ceiling = last_known_asic_frequency > 0.0f ? last_known_asic_frequency : atm_cfg.freq_ceiling_mhz;
                float scaled = ceiling * ((float) budget / (float) power_management_target_mw);
                if (scaled < 400.0f) {
                    scaled = 400.0f;
                }
                if (scaled > ceiling) {
                    scaled = ceiling;
                }
                ESP_LOGI(TAG, "Power target %s -> budget %umW (measured %umW), ceiling %.0f MHz",
                         pt_action_name(pt_action), budget, measured_mw, scaled);
                if (fabsf(scaled - last_asic_frequency) > 1.0f) {
                    nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, scaled);
                }
            }
        }

        /* ----------------------------------------------------------------
         * Flatline-of-death watchdog.
         *
         * Upstream issue #1053: the miner can keep "hashing" while producing
         * nothing acceptable, and nothing notices. The key to a watchdog here is
         * to trigger on shares the POOL accepted, not on local counters: a chip
         * returning garbage nonces can inflate local counters but cannot pass pool
         * validation, so only the pool can tell us mining is genuinely working.
         *
         * Timeout is 25x the expected share interval at the current pool
         * difficulty, floored at 10 minutes so a high-difficulty vardiff step can
         * never arm a false trigger. With that margin the probability of firing
         * spuriously is e^-25 (~1e-11), i.e. it will not fire in the lifetime of
         * the device.
         * ---------------------------------------------------------------- */
        {
            static uint32_t last_pool_accepted = 0;
            static uint32_t last_pool_accept_s = 0;

            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
            uint32_t accepted32 = (uint32_t) sys_module->shares_accepted;

            if (accepted32 != last_pool_accepted) {
                last_pool_accepted = accepted32;
                last_pool_accept_s = now_s;
            } else if (!is_paused && !sys_module->hardware_fault && GLOBAL_STATE->ASIC_initalized
                       && last_pool_accept_s != 0) {
                /* 2^32 * D / H seconds between shares at hashrate H. H in TH/s. */
                double difficulty = GLOBAL_STATE->pool_difficulty > 0.0 ? GLOBAL_STATE->pool_difficulty : 1.0;
                double hashrate_ths = power_management->expected_hashrate / 1000.0;
                double expected_interval_s = 0.0;
                if (hashrate_ths > 1.0) {
                    expected_interval_s = (4294967296.0 * difficulty) / (hashrate_ths * 1e12);
                }
                uint32_t timeout_s = (expected_interval_s > 0.0)
                                     ? (uint32_t)(expected_interval_s * 25.0)
                                     : 0;
                if (timeout_s < 600) {
                    timeout_s = 600;
                }
                if (now_s - last_pool_accept_s > timeout_s) {
                    ESP_LOGE(TAG, "FLATLINE: no pool-accepted share for %us (expected interval %.0fs x25, floor 600s) — reinitialising ASIC",
                             now_s - last_pool_accept_s, expected_interval_s);
                    /* Do not reset the timer here: leave it latched so the next
                     * accepted share is what clears it, and so repeated failures
                     * keep being visible in the log instead of being masked. */
                    if (GLOBAL_STATE->ASIC_initalized) {
                        ASIC_init(GLOBAL_STATE);
                    }
                    last_pool_accept_s = now_s - (timeout_s / 2);
                }
            }
        }

        VCORE_check_fault(GLOBAL_STATE);

        // looper:
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}
