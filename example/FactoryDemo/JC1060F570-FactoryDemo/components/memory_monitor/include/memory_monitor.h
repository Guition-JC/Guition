#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log current SRAM, PSRAM and ROM (flash) capacity without creating a task.
 * @param phase Optional label for the report; NULL uses "unknown".
 */
void memory_monitor_report(const char *phase);

/**
 * @brief Start idempotent 30-second memory reports on the esp_timer task.
 * @return ESP_OK if reporting is running, or an esp_timer error.
 */
esp_err_t memory_monitor_start(void);

#ifdef __cplusplus
}
#endif
