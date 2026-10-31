/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef TIME_SETTINGS_HOST_TEST
typedef int32_t esp_err_t;
#define ESP_OK ((esp_err_t)0)
#define ESP_FAIL ((esp_err_t)-1)
#define ESP_ERR_NO_MEM ((esp_err_t)0x101)
#define ESP_ERR_INVALID_ARG ((esp_err_t)0x102)
#define ESP_ERR_INVALID_STATE ((esp_err_t)0x103)
#define ESP_ERR_INVALID_RESPONSE ((esp_err_t)0x10f)
#define ESP_ERR_NVS_NOT_FOUND ((esp_err_t)0x1102)
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TIME_SETTINGS_DEFAULT_OFFSET_MINUTES (480)
#define TIME_SETTINGS_DISPLAY_OFFSET_MAX_LENGTH (10)
#define TIME_SETTINGS_POSIX_TZ_MAX_LENGTH (10)
#define TIME_SETTINGS_LOCAL_TIME_MAX_LENGTH (32)

typedef uintptr_t time_settings_nvs_handle_t;

/** @brief Persistence and process-timezone operations. */
typedef struct {
    void *context;
    esp_err_t (*open)(void *context, bool read_write, time_settings_nvs_handle_t *handle);
    esp_err_t (*get_u8)(void *context, time_settings_nvs_handle_t handle, const char *key, uint8_t *value);
    esp_err_t (*get_i32)(void *context, time_settings_nvs_handle_t handle, const char *key, int32_t *value);
    esp_err_t (*set_u8)(void *context, time_settings_nvs_handle_t handle, const char *key, uint8_t value);
    esp_err_t (*set_i32)(void *context, time_settings_nvs_handle_t handle, const char *key, int32_t value);
    esp_err_t (*commit)(void *context, time_settings_nvs_handle_t handle);
    esp_err_t (*erase_all)(void *context, time_settings_nvs_handle_t handle);
    void (*close)(void *context, time_settings_nvs_handle_t handle);
    int (*setenv)(void *context, const char *name, const char *value, int overwrite);
    int (*unsetenv)(void *context, const char *name);
    int (*tzset)(void *context);
} time_settings_ops_t;

/**
 * @brief Install timezone persistence and platform operations.
 * @param ops Operations to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_set_ops(const time_settings_ops_t *ops);
/**
 * @brief Restore default timezone operations.
 */
void time_settings_reset_ops(void);

/**
 * @brief NVS persistence and process-wide application are intentionally separate.
 * @param offset_minutes Timezone offset in minutes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_load(int32_t *offset_minutes);
/**
 * @brief Persist a validated timezone offset in minutes.
 * @param offset_minutes Timezone offset in minutes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_save(int32_t offset_minutes);
/**
 * @brief Apply a timezone offset to the process-wide environment.
 * @param offset_minutes Timezone offset in minutes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_apply(int32_t offset_minutes);
/**
 * @brief Clear persisted timezone settings.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_clear(void);

/**
 * @brief Formats local civil time while holding the same process-wide timezone lock.
 * @param epoch Epoch time in seconds.
 * @param[out] buffer Receives the formatted local time.
 * @param buffer_size Buffer capacity in bytes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_settings_format_local(time_t epoch, char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
