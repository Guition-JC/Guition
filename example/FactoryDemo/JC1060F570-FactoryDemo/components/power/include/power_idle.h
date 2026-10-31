/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Determine sleep transitions from monotonic activity timestamps. */

/** @brief Timeout choices offered by the settings page, in seconds. 0 disables sleep. */
typedef enum {
    POWER_IDLE_TIMEOUT_OFF = 0,
    POWER_IDLE_TIMEOUT_30S = 30,
    POWER_IDLE_TIMEOUT_1MIN = 60,
    POWER_IDLE_TIMEOUT_2MIN = 120,
    POWER_IDLE_TIMEOUT_5MIN = 300,
} power_idle_timeout_t;

/** @brief Default idle timeout: one minute. */
#define POWER_IDLE_DEFAULT_TIMEOUT_SECONDS ((uint32_t)POWER_IDLE_TIMEOUT_1MIN)

/** @brief Idle timeout, last activity and sleep latch. */
typedef struct {
    uint32_t timeout_ms; /**< Required idle time in milliseconds; 0 means "never sleep". */
    int64_t last_activity_ms; /**< Monotonic stamp of the last observed activity, in the caller's clock. */
    bool sleeping; /**< Latched while the sleep screen is up. */
} power_idle_state_t;

/**
 * @brief True only for the five values the settings page can produce.
 * @param timeout_seconds Timeout in seconds.
 * @return True only for the five values the settings page can produce.
 */
bool power_idle_timeout_supported(uint32_t timeout_seconds);

/**
 * @brief Return a supported timeout, or the one-minute default for anything else.
 * @param timeout_seconds Timeout in seconds.
 * @return A supported timeout, or the one-minute default for anything else.
 */
uint32_t power_idle_sanitize_timeout(uint32_t timeout_seconds);

/**
 * @brief Apply a timeout and treat `now_ms` as the last activity.
 * @param state State used by the operation.
 * @param timeout_seconds Timeout in seconds.
 * @param now_ms Current time in milliseconds.
 */
void power_idle_init(power_idle_state_t *state, uint32_t timeout_seconds, int64_t now_ms);

/**
 * @brief Change the timeout without touching the activity stamp; 0 disables sleep.
 * @param state State used by the operation.
 * @param timeout_seconds Timeout in seconds.
 */
void power_idle_set_timeout(power_idle_state_t *state, uint32_t timeout_seconds);

/**
 * @brief Applied timeout in seconds; 0 when disabled.
 * @param state State used by the operation.
 * @return Applied timeout in seconds; 0 when disabled.
 */
uint32_t power_idle_get_timeout_seconds(const power_idle_state_t *state);

/**
 * @brief Record user activity. Activity always means "awake", so it clears the latch.
 * @param state State used by the operation.
 * @param now_ms Current time in milliseconds.
 */
void power_idle_note_activity(power_idle_state_t *state, int64_t now_ms);

/**
 * @brief True once the timeout elapsed with no activity and sleep is not latched yet.
 * @param state State used by the operation.
 * @param now_ms Current time in milliseconds.
 * @return True once the timeout elapsed with no activity and sleep is not latched yet.
 */
bool power_idle_should_sleep(const power_idle_state_t *state, int64_t now_ms);

/**
 * @brief Latch the sleeping state after the sleep screen was built.
 * @param state State used by the operation.
 */
void power_idle_mark_sleeping(power_idle_state_t *state);

/**
 * @brief Clear the latch and restart the countdown from `now_ms`.
 * @param state State used by the operation.
 * @param now_ms Current time in milliseconds.
 */
void power_idle_mark_awake(power_idle_state_t *state, int64_t now_ms);

/**
 * @brief Return whether sleep is latched.
 * @param state State used by the operation.
 * @return true if sleep is latched; false otherwise.
 */
bool power_idle_is_sleeping(const power_idle_state_t *state);

#ifdef __cplusplus
}
#endif
