/** SPDX-License-Identifier: GPL-2.0-only */
/** RX8025/RX8035 driver. The caller owns the I2C bus; the driver owns its device handle. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RX8025_MODEL_RX8025 = 0,
    RX8025_MODEL_RX8035,
} rx8025_model_t;

typedef struct rx8025_dev_t *rx8025_handle_t;

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

/** @brief Daily alarm registers and enable state. */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t raw_hour;
    uint8_t raw_minute;
    bool enabled;
    bool pending;
} rx8025_alarm_t;

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

#define RX8025_DEFAULT_SCL_SPEED_HZ 400000U
#define RX8025_DEFAULT_TIMEOUT_MS 1000U

/**
 * @brief Create an RTC device handle on a caller-owned I2C bus.
 * @param config Configuration to use.
 * @param ret_handle Receives the created handle.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_new(const rx8025_config_t *config, rx8025_handle_t *ret_handle);
/**
 * @brief Delete the RTC device handle without deleting its I2C bus.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_delete(rx8025_handle_t handle);

/**
 * @brief Read and validate RTC calendar registers.
 * @param handle Handle of the resource.
 * @param[out] time Receives the RTC calendar.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_time(rx8025_handle_t handle, rx8025_datetime_t *time);
/**
 * @brief Write RTC calendar registers and clear invalid-time status.
 * @param handle Handle of the resource.
 * @param time Time value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_set_time(rx8025_handle_t handle, const rx8025_datetime_t *time);

/**
 * @brief Read the daily alarm time, enable state and pending flag.
 * @param handle Handle of the resource.
 * @param[out] alarm Receives the alarm state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_alarm(rx8025_handle_t handle, rx8025_alarm_t *alarm);
/**
 * @brief Write the daily alarm time and enable state.
 * @param handle Handle of the resource.
 * @param alarm Alarm configuration or observation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_set_alarm(rx8025_handle_t handle, const rx8025_alarm_t *alarm);
/**
 * @brief Enable or disable the daily alarm.
 * @param handle Handle of the resource.
 * @param enabled Whether the feature is enabled.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_alarm_enable(rx8025_handle_t handle, bool enabled);

/**
 * @brief Read RTC validity and interrupt status flags.
 * @param handle Handle of the resource.
 * @param[out] status Receives the RTC status flags.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_status(rx8025_handle_t handle, rx8025_status_t *status);
/**
 * @brief Clear pending RTC interrupt flags.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_clear_interrupt_flags(rx8025_handle_t handle);
/**
 * @brief Read the clock calibration offset in parts per billion.
 * @param handle Handle of the resource.
 * @param[out] offset_ppb Receives the calibration offset in parts per billion.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_offset(rx8025_handle_t handle, int32_t *offset_ppb);
/**
 * @brief Set the clock calibration offset in parts per billion.
 * @param handle Handle of the resource.
 * @param offset_ppb Offset in parts per billion.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_set_offset(rx8025_handle_t handle, int32_t offset_ppb);

#ifdef __cplusplus
}
#endif
