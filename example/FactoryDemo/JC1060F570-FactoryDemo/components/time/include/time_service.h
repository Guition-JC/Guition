/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "time_policy.h"

#ifdef TIME_SERVICE_HOST_TEST
#include "rtc_time.h"
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Select internal system time or seed system time from the RTC; SNTP corrects the active clock source. */
typedef enum {
    TIME_SOURCE_INTERNAL = 0,
    TIME_SOURCE_RTC,
} time_source_t;

#define TIME_SOURCE_DEFAULT TIME_SOURCE_INTERNAL

/**
 * @brief Check whether the time source is supported.
 * @param source Source to use.
 * @return true if the time source is supported; false otherwise.
 */
bool time_source_is_valid(time_source_t source);

/** @brief Time-service synchronization and error snapshot. */
typedef struct {
    bool initialized;
    bool worker_ready;
    bool sntp_initialized;
    bool network_ready;
    bool sync_pending;
    time_sync_state_t sync_state;
    rtc_state_t rtc_state;
    time_source_t time_source;
    int32_t timezone_offset_minutes;
    int64_t synchronized_epoch;
    struct timeval last_sync;
    esp_err_t init_error;
    esp_err_t timezone_error;
    esp_err_t rtc_error;
    esp_err_t sntp_error;
    esp_err_t reconcile_error;
    unsigned int got_ip_events;
    unsigned int disconnect_events;
    unsigned int sntp_start_calls;
    unsigned int sync_callbacks;
    unsigned int worker_runs;
} time_service_snapshot_t;

/**
 * @brief Initialize timezone, RTC, one SNTP instance, and one synchronization worker.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_init(void);

/**
 * @brief Start or restart SNTP once for the current station GOT_IP event.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_notify_network_ready(void);

/**
 * @brief Record loss of station IP connectivity without tearing down the service.
 */
void time_service_notify_network_lost(void);

/**
 * @brief Copy the current service state while holding the service mutex.
 * @param[out] snapshot Receives the copied state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_get_snapshot(time_service_snapshot_t *snapshot);

/**
 * @brief Publish a timezone already applied and persisted by the settings layer.
 * @param offset_minutes Timezone offset in minutes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_update_timezone_offset(int32_t offset_minutes);

/**
 * @brief Ask SNTP to (re)sync now, for the settings page's manual action.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_request_sync(void);

/**
 * @brief Copy the stored time source.
 * @param[out] source Receives the stored time source.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_get_time_source(time_source_t *source);

/**
 * @brief Persist the source and apply it now; RTC seeds the system clock from the chip.
 * @param source Source to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_set_time_source(time_source_t source);

#ifdef TIME_SERVICE_HOST_TEST

typedef void (*time_service_test_sync_callback_t)(struct timeval *tv);

typedef struct {
    void *context;
    esp_err_t (*timezone_load)(void *context, int32_t *offset_minutes);
    esp_err_t (*timezone_apply)(void *context, int32_t offset_minutes);
    esp_err_t (*timezone_clear)(void *context);
    esp_err_t (*rtc_init)(void *context);
    esp_err_t (*rtc_reconcile)(void *context, int64_t synchronized_epoch);
    esp_err_t (*sntp_init)(void *context, const char *server, bool start,
                           bool smooth_sync, bool wait_for_sync,
                           time_service_test_sync_callback_t sync_callback);
    void (*sntp_deinit)(void *context);
    esp_err_t (*sntp_start)(void *context);
    bool (*worker_create)(void *context);
    esp_err_t (*time_source_load)(void *context, time_source_t *source);
    esp_err_t (*time_source_save)(void *context, time_source_t source);
    esp_err_t (*rtc_read_now)(void *context, int64_t *epoch);
    esp_err_t (*system_time_set)(void *context, int64_t epoch);
} time_service_test_ops_t;

/**
 * @brief Install injectable time-service operations.
 * @param ops Operations to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t time_service_test_set_ops(const time_service_test_ops_t *ops);
/**
 * @brief Reset the injectable time-service state.
 */
void time_service_test_reset(void);
/**
 * @brief Deliver a supplied synchronization timestamp.
 * @param tv Time value.
 */
void time_service_test_invoke_sync_callback(const struct timeval *tv);
/**
 * @brief Process pending synchronization work synchronously.
 * @return true if pending synchronization work was processed.
 */
bool time_service_test_drain_worker(void);

#endif

#ifdef __cplusplus
}
#endif
