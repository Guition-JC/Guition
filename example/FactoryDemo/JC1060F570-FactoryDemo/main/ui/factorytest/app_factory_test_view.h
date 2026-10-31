/**
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_factory_test_toast_cb_t)(const char *message);

/**
 * @brief Ask the shell to hide or restore its exit strip.
 * @param covered true to hide the strip; false to restore it.
 * @param user_data Context supplied when building the diagnostics page.
 */
typedef void (*app_factory_test_cover_cb_t)(bool covered, void *user_data);

/**
 * @brief Build the factory diagnostics page; caller holds the LVGL adapter lock.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback used to display messages.
 * @param cover_cb Callback notifying the shell when diagnostics cover the menu.
 * @param user_data Context passed to cover_cb.
 */
void app_factory_test_view_build(lv_obj_t *parent,
                                   app_factory_test_toast_cb_t toast_cb,
                                   app_factory_test_cover_cb_t cover_cb,
                                   void *user_data);

/**
 * @brief Stop diagnostics and clear page-owned widget references.
 */
void app_factory_test_view_cleanup(void);

#ifdef __cplusplus
}
#endif
