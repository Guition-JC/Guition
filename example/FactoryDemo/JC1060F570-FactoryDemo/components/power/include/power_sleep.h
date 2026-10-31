/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Control and restore backlight brightness during sleep. */

typedef struct {
    bool active; /**< True between enter and exit. */
    uint8_t saved_backlight_percent; /**< Backlight level to restore, captured before dimming. */
    bool saved_backlight_valid; /**< True when the sleep screen dimmed the panel. */
} power_sleep_state_t;

/**
 * @brief Apply the sleep power measures. Idempotent.
 * @param state State used by the operation.
 */
void power_sleep_enter(power_sleep_state_t *state);

/**
 * @brief Restore the backlight captured before entering sleep.
 * @param state State used by the operation.
 */
void power_sleep_exit(power_sleep_state_t *state);

#ifdef __cplusplus
}
#endif
