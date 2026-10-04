#include <stdbool.h>
#include <float.h>
#include <math.h>

#include "pll.h"

#include "esp_log.h"
#include "esp_err.h"

#define EPSILON 0.0001f

static const char * TAG = "pll";

/* Frequency actually derived from the last solved divider set, 0 when the search
 * found no solution. Exposed because callers must size nonce space from the clock
 * the chip really runs at, not the one that was requested. */
static float s_last_solved_freq = 0.0f;

esp_err_t pll_get_last_solved_frequency(float * out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_last_solved_freq;
    return (s_last_solved_freq > 0.0f) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t pll_get_parameters(float target_freq, uint16_t fb_divider_min, uint16_t fb_divider_max,
                        uint8_t *fb_divider, uint8_t *refdiv, uint8_t *postdiv1, uint8_t *postdiv2,
                        float *actual_freq)
{
    float best_freq = 0;
    uint8_t best_refdiv = 0, best_fb_divider = 0, best_postdiv1 = 0, best_postdiv2 = 0;
    float min_diff = FLT_MAX;
    float min_vco_freq = FLT_MAX;
    uint16_t min_postdiv = UINT16_MAX;

    for (uint8_t refdiv = 2; refdiv > 0; refdiv--) {
        for (uint8_t postdiv1 = 7; postdiv1 > 0; postdiv1--) {
            for (uint8_t postdiv2 = 7; postdiv2 > 0; postdiv2--) {
                uint16_t divider = refdiv * postdiv1 * postdiv2;
                uint16_t fb_divider = round(target_freq / FREQ_MULT * divider);
                if (postdiv1 > postdiv2 &&
                    fb_divider >= fb_divider_min && fb_divider <= fb_divider_max) {
                    float new_freq = FREQ_MULT * fb_divider / divider;
                    float curr_diff = fabs(target_freq - new_freq);
                    float vco_freq = FREQ_MULT * fb_divider / refdiv;
                    // Prioritize: 
                    // 1. Closest frequency to target
                    // 2. Lowest VCO frequency
                    // 3. Lowest postdiv1 * postdiv2
                    if (curr_diff < min_diff ||
                       (fabs(curr_diff - min_diff) < EPSILON && vco_freq < min_vco_freq) ||
                       (fabs(curr_diff - min_diff) < EPSILON && fabs(vco_freq - min_vco_freq) < EPSILON && postdiv1 * postdiv2 < min_postdiv)) {
                        min_diff = curr_diff;
                        min_vco_freq = vco_freq;
                        min_postdiv = postdiv1 * postdiv2;
                        best_freq = new_freq;
                        best_refdiv = refdiv;
                        best_fb_divider = fb_divider;
                        best_postdiv1 = postdiv1;
                        best_postdiv2 = postdiv2;
                    }
                }
            }
        }
    }

    /* No solution in range: previously this returned ESP_OK with all-zero
     * dividers, and the caller packed those straight into register 0x08. The PLL
     * then never locked and the chip stopped hashing until a reboot, with the
     * bogus value persisted in NVS. Report the failure instead so callers can
     * refuse to write the register. See upstream PR #1864 / #1989. */
    if (best_freq <= 0.0f) {
        ESP_LOGE(TAG, "No PLL solution for %g MHz in fb_divider %u..%u - refusing to program",
                 target_freq, fb_divider_min, fb_divider_max);
        s_last_solved_freq = 0.0f;
        if (actual_freq) {
            *actual_freq = 0.0f;
        }
        if (fb_divider) {
            *fb_divider = 0;
        }
        if (refdiv) {
            *refdiv = 0;
        }
        if (postdiv1) {
            *postdiv1 = 0;
        }
        if (postdiv2) {
            *postdiv2 = 0;
        }
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Frequency: %g MHz (fb_divider: %d, refdiv: %d, postdiv1: %d, postdiv2: %d)", best_freq, best_fb_divider, best_refdiv, best_postdiv1, best_postdiv2);

    s_last_solved_freq = best_freq;
    *actual_freq = best_freq;
    *fb_divider = best_fb_divider;
    *refdiv = best_refdiv;
    *postdiv1 = best_postdiv1;
    *postdiv2 = best_postdiv2;
    return ESP_OK;
}
