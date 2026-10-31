/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "time_policy.h"

#ifdef TIME_RTC_HOST_TEST

typedef int32_t esp_err_t;
#ifndef ESP_OK
#define ESP_OK ((esp_err_t)0)
#endif
#ifndef ESP_FAIL
#define ESP_FAIL ((esp_err_t)-1)
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG ((esp_err_t)0x102)
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE ((esp_err_t)0x103)
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND ((esp_err_t)0x105)
#endif
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT ((esp_err_t)0x107)
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE ((esp_err_t)0x10f)
#endif

/** @brief Minimal host declarations mirror the public RX8025 component ABI. */
typedef void *i2c_master_bus_handle_t;
typedef void *rx8025_handle_t;
typedef enum {
    RX8025_MODEL_RX8025 = 0,
    RX8025_MODEL_RX8035,
} rx8025_model_t;
/** @brief RTC device address, I2C timing and chip model. */
typedef struct {
    i2c_master_bus_handle_t bus_handle;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t timeout_ms;
    rx8025_model_t model;
} rx8025_config_t;
/** @brief RTC calendar fields. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rx8025_datetime_t;
#define RX8025_DEFAULT_SCL_SPEED_HZ 400000U
#define RX8025_DEFAULT_TIMEOUT_MS 1000U

/** @brief RTC validity and interrupt flags. */
typedef struct {
    uint8_t raw_control2;
    bool voltage_drop;
    bool power_on_reset;
    bool oscillator_stopped;
    bool alarm_flag;
    bool weekday_alarm_flag;
    bool periodic_flag;
} rx8025_status_t;

#else

#include "esp_err.h"
#include "rx8025.h"

#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief RTC bus and device operations supplied by the platform. */
typedef struct {
    void *context;
    esp_err_t (*get_bus)(const char *name, void **bus, void *context);
    esp_err_t (*new_device)(const rx8025_config_t *config, rx8025_handle_t *handle, void *context);
    esp_err_t (*read_status)(rx8025_handle_t handle, rx8025_status_t *status, void *context);
    esp_err_t (*read_time)(rx8025_handle_t handle, rx8025_datetime_t *time, void *context);
    esp_err_t (*set_time)(rx8025_handle_t handle, const rx8025_datetime_t *time, void *context);
    esp_err_t (*delete_device)(rx8025_handle_t handle, void *context); /**< Optional device destructor; supply it to release each temporary probe handle. */
} rtc_time_ops_t;

/** @brief Cached RTC availability and reconciliation result. */
typedef struct {
    rtc_state_t state;
    bool initialized;
    bool available;
    bool repaired;
    unsigned int repair_attempts;
    unsigned int readback_attempts;
    rx8025_datetime_t last_written_time;
} rtc_time_result_t;

/** @brief Result of an uncached RTC communication probe. */
typedef struct {
    bool bus_available;
    bool device_opened;
    bool responded;      /**< The status register answered: communication is up. */
    bool calendar_read;  /**< Register data also decoded into a calendar. */
    rx8025_status_t status;
    rx8025_datetime_t time;
    esp_err_t last_error;
} rtc_time_probe_result_t;

/**
 * @brief Returns ESP_OK only when the device answered its status register.
 * @param[out] result Receives the RTC probe result.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_probe(rtc_time_probe_result_t *result);

/**
 * @brief Install RTC platform operations before initialization.
 * @param ops Operations to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_set_ops(const rtc_time_ops_t *ops);
/**
 * @brief Restore default RTC platform operations.
 */
void rtc_time_reset_ops(void);

/**
 * @brief Initialize the RTC and cache its availability.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_init(void);
/**
 * @brief Compare RTC time with synchronized UTC seconds and repair when required.
 * @param synchronized_epoch Synchronized epoch in seconds.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_reconcile(int64_t synchronized_epoch);

/**
 * @brief Read a valid RTC calendar as UTC seconds; reject missing devices, invalid dates and power-loss or oscillator-stop status.
 * @param[out] epoch Receives UTC epoch seconds.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_read_now(int64_t *epoch);
/**
 * @brief Copy the latest RTC initialization and reconciliation result.
 * @param[out] result Receives the latest RTC initialization and reconciliation result.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rtc_time_get_result(rtc_time_result_t *result);

#ifdef __cplusplus
}
#endif
