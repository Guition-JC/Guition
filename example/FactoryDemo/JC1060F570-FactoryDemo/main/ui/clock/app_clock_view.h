/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if !defined(APP_CLOCK_VIEW_HOST_TEST) && !defined(APP_CLOCK_HOST_TEST)
#include "lvgl.h"
#else
typedef struct lv_obj_t lv_obj_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_clock_toast_cb_t)(const char *message);

/** @brief Timezone adjustment step in minutes. */
#define APP_CLOCK_TIMEZONE_STEP_MINUTES (60)

/**
 * @brief Build the fullscreen clock and its settings; caller holds the LVGL lock.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback for displaying a message.
 */
void app_clock_view_build(lv_obj_t *parent, app_clock_toast_cb_t toast_cb);
/**
 * @brief Stop clock timers and release the view and its resources; caller holds the LVGL lock.
 */
void app_clock_view_destroy(void);

/**
 * @brief Supply local civil time to the sleep face.
 * @param context Callback context supplied when building the sleep view.
 * @param[out] hour Receives the local hour.
 * @param[out] minute Receives the local minute.
 * @param[out] second Receives the local second.
 * @return false when time is unset; true when the output time is available.
 */
typedef bool (*app_clock_time_cb_t)(void *context, uint8_t *hour, uint8_t *minute,
                                    uint8_t *second);

/**
 * @brief Share the singleton renderer while the app is closed; caller holds the LVGL lock.
 * @details Sleep mode omits date, hints, settings and the second timer; failure cleans up the face.
 * @param parent Parent LVGL object.
 * @param time_cb Callback supplying local time.
 * @param time_context Context passed to time_cb.
 * @return true if the sleep view was built; false on failure.
 */
bool app_clock_view_build_sleep(lv_obj_t *parent, app_clock_time_cb_t time_cb,
                                  void *time_context);
/**
 * @brief Refresh the externally driven sleep face while holding the LVGL lock.
 */
void app_clock_view_refresh_sleep(void);

#ifdef __cplusplus
}
#endif
