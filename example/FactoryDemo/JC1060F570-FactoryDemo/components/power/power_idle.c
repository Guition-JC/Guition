/** SPDX-License-Identifier: GPL-2.0-only */
#include "power_idle.h"

#include <stddef.h>

/** The settings page offers exactly these five steps. */
static const uint32_t s_supported_timeouts[] = {
    (uint32_t)POWER_IDLE_TIMEOUT_OFF,
    (uint32_t)POWER_IDLE_TIMEOUT_30S,
    (uint32_t)POWER_IDLE_TIMEOUT_1MIN,
    (uint32_t)POWER_IDLE_TIMEOUT_2MIN,
    (uint32_t)POWER_IDLE_TIMEOUT_5MIN,
};

#define POWER_IDLE_TIMEOUT_COUNT \
    (sizeof(s_supported_timeouts) / sizeof(s_supported_timeouts[0]))

bool power_idle_timeout_supported(uint32_t timeout_seconds)
{
    for (size_t i = 0; i < POWER_IDLE_TIMEOUT_COUNT; ++i) {
        if (s_supported_timeouts[i] == timeout_seconds) {
            return true;
        }
    }
    return false;
}

uint32_t power_idle_sanitize_timeout(uint32_t timeout_seconds)
{
    return power_idle_timeout_supported(timeout_seconds)
               ? timeout_seconds
               : POWER_IDLE_DEFAULT_TIMEOUT_SECONDS;
}

void power_idle_init(power_idle_state_t *state, uint32_t timeout_seconds, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->timeout_ms = power_idle_sanitize_timeout(timeout_seconds) * 1000U;
    state->last_activity_ms = now_ms;
    state->sleeping = false;
}

void power_idle_set_timeout(power_idle_state_t *state, uint32_t timeout_seconds)
{
    if (state == NULL) {
        return;
    }
    const uint32_t sanitized = power_idle_sanitize_timeout(timeout_seconds);
    state->timeout_ms = sanitized * 1000U;
    /** The activity stamp stays: shortening the timeout may legitimately put a device that has already been idle long enough to sleep on the next tick. */
}

uint32_t power_idle_get_timeout_seconds(const power_idle_state_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    return state->timeout_ms / 1000U;
}

void power_idle_note_activity(power_idle_state_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->last_activity_ms = now_ms;
    state->sleeping = false;
}

bool power_idle_should_sleep(const power_idle_state_t *state, int64_t now_ms)
{
    if (state == NULL || state->sleeping || state->timeout_ms == 0U) {
        return false;
    }
    const int64_t elapsed_ms = now_ms - state->last_activity_ms;
    return elapsed_ms >= (int64_t)state->timeout_ms;
}

void power_idle_mark_sleeping(power_idle_state_t *state)
{
    if (state != NULL) {
        state->sleeping = true;
    }
}

void power_idle_mark_awake(power_idle_state_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->sleeping = false;
    state->last_activity_ms = now_ms;
}

bool power_idle_is_sleeping(const power_idle_state_t *state)
{
    return state != NULL && state->sleeping;
}
