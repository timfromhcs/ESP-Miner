#ifndef PLL_H_
#define PLL_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define FREQ_MULT 25.0 // MHz

/**
 * Solve PLL dividers for @p target_freq.
 *
 * @return ESP_OK when a solution was found; ESP_ERR_NOT_FOUND when none exists
 *         in the given divider range, in which case all out-params are set to 0
 *         and the caller MUST NOT program register 0x08. Callers used to ignore
 *         this and wrote all-zero dividers, which left the PLL unlocked and the
 *         chip idle until a reboot.
 */
esp_err_t pll_get_parameters(float target_freq, uint16_t fb_divider_min, uint16_t fb_divider_max,
                        uint8_t *fb_divider, uint8_t *refdiv, uint8_t *postdiv1, uint8_t *postdiv2,
                        float *actual_freq);

/**
 * @brief Frequency actually derived from the last solved divider set.
 *
 * Nonce space (HCN) must be sized from this, not from the requested frequency.
 * @return ESP_OK and a value > 0 when a solution has been solved; otherwise
 *         ESP_ERR_INVALID_STATE and 0.
 */
esp_err_t pll_get_last_solved_frequency(float * out);

#endif /* PLL_H_ */
