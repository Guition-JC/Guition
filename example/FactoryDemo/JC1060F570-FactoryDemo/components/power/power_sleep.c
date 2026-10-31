/** SPDX-License-Identifier: GPL-2.0-only */
#include "power_sleep.h"

#include "esp_log.h"

#include "display.h"

#define TAG "PowerSleep"

/** @brief Sleep backlight brightness, in percent. */
#define POWER_SLEEP_BACKLIGHT_PERCENT 30U

static void sleep_dim_backlight(power_sleep_state_t *state)
{
    const uint8_t current = display_backlight_get();
    if (current == 0U) {
        /** Backlight unknown or unsupported on this board. */
        return;
    }
    if (current <= POWER_SLEEP_BACKLIGHT_PERCENT) {
        /** The user picked a dimmer level: never brighten it for the sleep screen. */
        return;
    }
    if (display_backlight_set((uint8_t)POWER_SLEEP_BACKLIGHT_PERCENT) != ESP_OK) {
        ESP_LOGW(TAG, "Backlight dim to %u%% failed", (unsigned)POWER_SLEEP_BACKLIGHT_PERCENT);
        return;
    }
    state->saved_backlight_percent = current;
    state->saved_backlight_valid = true;
    ESP_LOGI(TAG, "Backlight %u%% -> %u%%", (unsigned)current,
             (unsigned)POWER_SLEEP_BACKLIGHT_PERCENT);
}

static void sleep_restore_backlight(power_sleep_state_t *state)
{
    if (state->saved_backlight_valid) {
        /** Runtime only: the user's stored brightness preference is untouched. */
        (void)display_backlight_set(state->saved_backlight_percent);
        ESP_LOGI(TAG, "Backlight restored to %u%%", (unsigned)state->saved_backlight_percent);
        state->saved_backlight_valid = false;
    }
}

void power_sleep_enter(power_sleep_state_t *state)
{
    if (state == NULL || state->active) {
        return;
    }
    state->active = true;

    sleep_dim_backlight(state);
}

void power_sleep_exit(power_sleep_state_t *state)
{
    if (state == NULL || !state->active) {
        return;
    }
    sleep_restore_backlight(state);
    state->active = false;
}
