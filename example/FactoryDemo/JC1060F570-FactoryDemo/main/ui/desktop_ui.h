#pragma once

#include <stdbool.h>

#include "desktop_language.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the desktop and navigation objects; the caller must hold the LVGL adapter lock.
 */
void desktop_ui_create(void);

/**
 * @brief Register the Wi-Fi indicator listener once after Wi-Fi service initialization.
 */
void desktop_ui_register_wifi_indicator(void);

/**
 * @brief Register the weather listener once after weather service initialization.
 */
void desktop_ui_register_weather_widget(void);

#ifdef __cplusplus
}
#endif
