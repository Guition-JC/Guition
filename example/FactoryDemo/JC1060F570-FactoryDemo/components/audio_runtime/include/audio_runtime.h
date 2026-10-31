#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the application-wide audio resources after esp_board_manager_init().
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_runtime_init(void);
/**
 * @brief Return whether the global audio runtime is ready for use.
 * @return true if the global audio runtime is ready for use; false otherwise.
 */
bool audio_runtime_is_initialized(void);
/**
 * @brief Acquire a shared user reference before creating a pipeline.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_runtime_acquire(void);
/**
 * @brief Release a shared user reference after destroying all owned pipelines.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_runtime_release(void);
/**
 * @brief Shutdown global audio resources; all user pipelines must already be destroyed.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_runtime_deinit(void);

#ifdef __cplusplus
}
#endif
