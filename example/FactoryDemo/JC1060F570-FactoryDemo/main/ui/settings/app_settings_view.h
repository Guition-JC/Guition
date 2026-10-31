/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "lvgl.h"

#include "desktop_language.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_settings_view_toast_cb_t)(const char *message);

/**
 * @brief Request a language change and UI rebuild from the desktop shell.
 * @param language Selected language.
 */
typedef void (*app_settings_view_language_cb_t)(desktop_language_t language);

/**
 * @brief Report font selection to the desktop shell.
 * @param file_name Selected font file name.
 * @param applied false when the bundled font remains active.
 */
typedef void (*app_settings_view_font_cb_t)(const char *file_name, bool applied);

/**
 * @brief Build settings while holding the LVGL adapter lock; unmount other fullscreen views first.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback used to display messages.
 * @param language_cb Callback invoked after changing language.
 * @param font_cb Callback invoked after changing font.
 */
void app_settings_view_build(lv_obj_t *parent,
                               app_settings_view_toast_cb_t toast_cb,
                               app_settings_view_language_cb_t language_cb,
                               app_settings_view_font_cb_t font_cb);

/**
 * @brief Clear page-owned widget references.
 */
void app_settings_view_destroy(void);

/**
 * @brief Refresh standby controls after external edits; caller holds the LVGL lock.
 */
void app_settings_view_refresh_standby(void);

#ifdef __cplusplus
}
#endif
