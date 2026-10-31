/** SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "time_settings.h"

#include "time_policy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TIME_SETTINGS_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#endif

#define TIME_SETTINGS_NAMESPACE "clock"
#define TIME_SETTINGS_KEY_VALID "valid"
#define TIME_SETTINGS_KEY_VERSION "version"
#define TIME_SETTINGS_KEY_OFFSET "offset_min"
#define TIME_SETTINGS_RECORD_VERSION ((uint8_t)1)
#define TIME_SETTINGS_RECORD_VALID ((uint8_t)1)
#define TIME_SETTINGS_RECORD_INVALID ((uint8_t)0)

#ifdef TIME_SETTINGS_HOST_TEST
#include <pthread.h>
static pthread_mutex_t s_timezone_lock = PTHREAD_MUTEX_INITIALIZER;
#define TIME_SETTINGS_LOCK() pthread_mutex_lock(&s_timezone_lock)
#define TIME_SETTINGS_UNLOCK() pthread_mutex_unlock(&s_timezone_lock)
#else
static SemaphoreHandle_t s_timezone_lock;

static esp_err_t timezone_lock_init(void)
{
    if (s_timezone_lock == NULL) {
        s_timezone_lock = xSemaphoreCreateMutex();
        if (s_timezone_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

#define TIME_SETTINGS_LOCK() xSemaphoreTake(s_timezone_lock, portMAX_DELAY)
#define TIME_SETTINGS_UNLOCK() xSemaphoreGive(s_timezone_lock)
#endif

#ifndef TIME_SETTINGS_HOST_TEST
static esp_err_t default_open(void *context, bool read_write, time_settings_nvs_handle_t *handle)
{
    (void)context;
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(TIME_SETTINGS_NAMESPACE,
                             read_write ? NVS_READWRITE : NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        *handle = (time_settings_nvs_handle_t)nvs_handle;
    }
    return ret;
}

static esp_err_t default_get_u8(void *context, time_settings_nvs_handle_t handle,
                                const char *key, uint8_t *value)
{
    (void)context;
    return nvs_get_u8((nvs_handle_t)handle, key, value);
}

static esp_err_t default_get_i32(void *context, time_settings_nvs_handle_t handle,
                                 const char *key, int32_t *value)
{
    (void)context;
    return nvs_get_i32((nvs_handle_t)handle, key, value);
}

static esp_err_t default_set_u8(void *context, time_settings_nvs_handle_t handle,
                                const char *key, uint8_t value)
{
    (void)context;
    return nvs_set_u8((nvs_handle_t)handle, key, value);
}

static esp_err_t default_set_i32(void *context, time_settings_nvs_handle_t handle,
                                 const char *key, int32_t value)
{
    (void)context;
    return nvs_set_i32((nvs_handle_t)handle, key, value);
}

static esp_err_t default_commit(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    return nvs_commit((nvs_handle_t)handle);
}

static esp_err_t default_erase_all(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    return nvs_erase_all((nvs_handle_t)handle);
}

static void default_close(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    nvs_close((nvs_handle_t)handle);
}
#else
static esp_err_t missing_open(void *context, bool read_write, time_settings_nvs_handle_t *handle)
{
    (void)context;
    (void)read_write;
    (void)handle;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_get_u8(void *context, time_settings_nvs_handle_t handle,
                                const char *key, uint8_t *value)
{
    (void)context;
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_get_i32(void *context, time_settings_nvs_handle_t handle,
                                 const char *key, int32_t *value)
{
    (void)context;
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_set_u8(void *context, time_settings_nvs_handle_t handle,
                                const char *key, uint8_t value)
{
    (void)context;
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_set_i32(void *context, time_settings_nvs_handle_t handle,
                                 const char *key, int32_t value)
{
    (void)context;
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_commit(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    (void)handle;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t missing_erase_all(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    (void)handle;
    return ESP_ERR_INVALID_STATE;
}

static void missing_close(void *context, time_settings_nvs_handle_t handle)
{
    (void)context;
    (void)handle;
}
#endif

static int default_setenv(void *context, const char *name, const char *value, int overwrite)
{
    (void)context;
    return setenv(name, value, overwrite);
}

static int default_unsetenv(void *context, const char *name)
{
    (void)context;
    return unsetenv(name);
}

static int default_tzset(void *context)
{
    (void)context;
    tzset();
    return 0;
}

static const time_settings_ops_t s_default_ops = {
    .context = NULL,
#ifdef TIME_SETTINGS_HOST_TEST
    .open = missing_open,
    .get_u8 = missing_get_u8,
    .get_i32 = missing_get_i32,
    .set_u8 = missing_set_u8,
    .set_i32 = missing_set_i32,
    .commit = missing_commit,
    .erase_all = missing_erase_all,
    .close = missing_close,
#else
    .open = default_open,
    .get_u8 = default_get_u8,
    .get_i32 = default_get_i32,
    .set_u8 = default_set_u8,
    .set_i32 = default_set_i32,
    .commit = default_commit,
    .erase_all = default_erase_all,
    .close = default_close,
#endif
    .setenv = default_setenv,
    .unsetenv = default_unsetenv,
    .tzset = default_tzset,
};

static time_settings_ops_t s_ops;
static bool s_ops_initialized;

static const time_settings_ops_t *current_ops(void)
{
    if (!s_ops_initialized) {
        s_ops = s_default_ops;
        s_ops_initialized = true;
    }
    return &s_ops;
}

esp_err_t time_settings_set_ops(const time_settings_ops_t *ops)
{
    if (ops == NULL || ops->open == NULL || ops->get_u8 == NULL || ops->get_i32 == NULL ||
        ops->set_u8 == NULL || ops->set_i32 == NULL || ops->commit == NULL ||
        ops->erase_all == NULL || ops->close == NULL || ops->setenv == NULL ||
        ops->unsetenv == NULL || ops->tzset == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = *ops;
    s_ops_initialized = true;
    return ESP_OK;
}

void time_settings_reset_ops(void)
{
    s_ops = s_default_ops;
    s_ops_initialized = true;
}

static esp_err_t normalize_load_error(esp_err_t ret)
{
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
}

esp_err_t time_settings_load(int32_t *offset_minutes)
{
    if (offset_minutes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;

    const time_settings_ops_t *ops = current_ops();
    time_settings_nvs_handle_t handle = 0;
    esp_err_t ret = ops->open(ops->context, false, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t valid = TIME_SETTINGS_RECORD_INVALID;
    ret = ops->get_u8(ops->context, handle, TIME_SETTINGS_KEY_VALID, &valid);
    if (ret == ESP_ERR_NVS_NOT_FOUND || (ret == ESP_OK && valid == TIME_SETTINGS_RECORD_INVALID)) {
        ops->close(ops->context, handle);
        return ESP_OK;
    }
    if (ret != ESP_OK || valid != TIME_SETTINGS_RECORD_VALID) {
        ops->close(ops->context, handle);
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : normalize_load_error(ret);
    }

    uint8_t version = 0;
    int32_t stored_offset = 0;
    ret = ops->get_u8(ops->context, handle, TIME_SETTINGS_KEY_VERSION, &version);
    if (ret == ESP_OK) {
        ret = ops->get_i32(ops->context, handle, TIME_SETTINGS_KEY_OFFSET, &stored_offset);
    }
    ops->close(ops->context, handle);
    if (ret != ESP_OK) {
        return normalize_load_error(ret);
    }
    if (version != TIME_SETTINGS_RECORD_VERSION || !time_policy_is_valid_offset(stored_offset)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *offset_minutes = stored_offset;
    return ESP_OK;
}

esp_err_t time_settings_save(int32_t offset_minutes)
{
    if (!time_policy_is_valid_offset(offset_minutes)) {
        return ESP_ERR_INVALID_ARG;
    }

    const time_settings_ops_t *ops = current_ops();
    time_settings_nvs_handle_t handle = 0;
    esp_err_t ret = ops->open(ops->context, true, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ops->set_u8(ops->context, handle, TIME_SETTINGS_KEY_VALID, TIME_SETTINGS_RECORD_INVALID);
    if (ret == ESP_OK) {
        ret = ops->commit(ops->context, handle);
    }
    if (ret == ESP_OK) {
        ret = ops->set_u8(ops->context, handle, TIME_SETTINGS_KEY_VERSION,
                          TIME_SETTINGS_RECORD_VERSION);
    }
    if (ret == ESP_OK) {
        ret = ops->set_i32(ops->context, handle, TIME_SETTINGS_KEY_OFFSET, offset_minutes);
    }
    if (ret == ESP_OK) {
        ret = ops->commit(ops->context, handle);
    }
    if (ret == ESP_OK) {
        ret = ops->set_u8(ops->context, handle, TIME_SETTINGS_KEY_VALID, TIME_SETTINGS_RECORD_VALID);
    }
    if (ret == ESP_OK) {
        ret = ops->commit(ops->context, handle);
    }

    ops->close(ops->context, handle);
    return ret;
}

static char *copy_environment_value(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    const size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

esp_err_t time_settings_apply(int32_t offset_minutes)
{
    if (!time_policy_is_valid_offset(offset_minutes)) {
        return ESP_ERR_INVALID_ARG;
    }

    char display_offset[TIME_SETTINGS_DISPLAY_OFFSET_MAX_LENGTH];
    char posix_tz[TIME_SETTINGS_POSIX_TZ_MAX_LENGTH];
    if (!time_policy_format_display_offset(offset_minutes, display_offset, sizeof(display_offset)) ||
        !time_policy_format_posix_tz(offset_minutes, posix_tz, sizeof(posix_tz))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

#ifndef TIME_SETTINGS_HOST_TEST
    esp_err_t ret = timezone_lock_init();
    if (ret != ESP_OK) {
        return ret;
    }
#endif
    TIME_SETTINGS_LOCK();
    const time_settings_ops_t *ops = current_ops();
    const char *old_tz = getenv("TZ");
    char *old_tz_copy = copy_environment_value(old_tz);
    if (old_tz != NULL && old_tz_copy == NULL) {
        TIME_SETTINGS_UNLOCK();
        return ESP_ERR_NO_MEM;
    }

    int env_ret = ops->setenv(ops->context, "TZ", posix_tz, 1);
    if (env_ret != 0) {
        free(old_tz_copy);
        TIME_SETTINGS_UNLOCK();
        return ESP_FAIL;
    }

    int tzset_ret = ops->tzset(ops->context);
    if (tzset_ret != 0) {
        if (old_tz_copy != NULL) {
            (void)ops->setenv(ops->context, "TZ", old_tz_copy, 1);
        } else {
            (void)ops->unsetenv(ops->context, "TZ");
        }
        (void)ops->tzset(ops->context);
        free(old_tz_copy);
        TIME_SETTINGS_UNLOCK();
        return ESP_FAIL;
    }

    free(old_tz_copy);
    TIME_SETTINGS_UNLOCK();
    (void)display_offset;
    return ESP_OK;
}

esp_err_t time_settings_clear(void)
{
    const time_settings_ops_t *ops = current_ops();
    time_settings_nvs_handle_t handle = 0;
    esp_err_t ret = ops->open(ops->context, true, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ops->erase_all(ops->context, handle);
    if (ret == ESP_OK) {
        ret = ops->commit(ops->context, handle);
    }
    ops->close(ops->context, handle);
    return ret;
}

esp_err_t time_settings_format_local(time_t epoch, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef TIME_SETTINGS_HOST_TEST
    esp_err_t ret = timezone_lock_init();
    if (ret != ESP_OK) {
        return ret;
    }
#endif
    TIME_SETTINGS_LOCK();
    struct tm local_time;
    const bool converted = localtime_r(&epoch, &local_time) != NULL;
    const size_t written = converted ? strftime(buffer, buffer_size, "%Y-%m-%d %H:%M", &local_time) : 0U;
    TIME_SETTINGS_UNLOCK();
    if (!converted || written == 0U) {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
