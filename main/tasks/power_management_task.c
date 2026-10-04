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

/* The regulator cannot be driven below this, no matter what the thermal policy
 * asks for. Stepping under it is what produced the false "Power Fault" latch. */
#define ASIC_VOLTAGE_REGULATOR_FLOOR_MV 1000

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

        VCORE_check_fault(GLOBAL_STATE);

        // looper:
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}
