/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include "rx8025.h"

#define RX8025_REG_SEC 0x00U
#define RX8025_REG_MIN 0x01U
#define RX8025_REG_HOUR 0x02U
#define RX8025_REG_WDAY 0x03U
#define RX8025_REG_MDAY 0x04U
#define RX8025_REG_MONTH 0x05U
#define RX8025_REG_YEAR 0x06U
#define RX8025_REG_DIGOFF 0x07U
#define RX8025_REG_ALDMIN 0x0bU
#define RX8025_REG_ALDHOUR 0x0cU
#define RX8025_REG_CTRL1 0x0eU
#define RX8025_REG_CTRL2 0x0fU

#define RX8025_BIT_CTRL1_TEST (1U << 3)
#define RX8025_BIT_CTRL1_1224 (1U << 5)
#define RX8025_BIT_CTRL1_DALE (1U << 6)

#define RX8025_BIT_CTRL2_DAFG (1U << 0)
#define RX8025_BIT_CTRL2_WAFG (1U << 1)
#define RX8025_BIT_CTRL2_CTFG (1U << 2)
#define RX8025_BIT_CTRL2_PON (1U << 4)
#define RX8025_BIT_CTRL2_XST (1U << 5)
#define RX8025_BIT_CTRL2_VDET (1U << 6)

#define RX8035_BIT_HOUR_1224 (1U << 7)
#define RX8025_ADJ_RESOLUTION_PPB 3050
#define RX8025_ADJ_DATA_MAX 62
#define RX8025_ADJ_DATA_MIN (-62)
#define RX8025_MAX_TRANSFER 16U
#define RX8025_STOP_TO_START_DELAY_US 100U

struct rx8025_dev_t {
    i2c_master_dev_handle_t i2c_device;
    rx8025_model_t model;
    uint32_t timeout_ms;
    uint8_t ctrl1;
    bool is_24_hour;
};

/**
 * @brief Read a bounded range of RTC registers.
 * @param handle Handle of the resource.
 * @param reg Register value.
 * @param[out] data Receives the register bytes.
 * @param length Length of the supplied data.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_registers(rx8025_handle_t handle, uint8_t reg, uint8_t *data, size_t length);
/**
 * @brief Write a bounded range of RTC registers.
 * @param handle Handle of the resource.
 * @param reg Register value.
 * @param data Data used by the operation.
 * @param length Length of the supplied data.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_write_registers(rx8025_handle_t handle, uint8_t reg, const uint8_t *data, size_t length);
/**
 * @brief Write one RTC register.
 * @param handle Handle of the resource.
 * @param reg Register value.
 * @param value Value used by the operation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_write_register(rx8025_handle_t handle, uint8_t reg, uint8_t value);
/**
 * @brief Read one RTC register.
 * @param handle Handle of the resource.
 * @param reg Register value.
 * @param[out] value Receives the register value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_read_register(rx8025_handle_t handle, uint8_t reg, uint8_t *value);
/**
 * @brief Convert a binary integer to packed BCD.
 * @param value Value used by the operation.
 * @return Packed BCD representation.
 */
uint8_t rx8025_bin_to_bcd(uint8_t value);
/**
 * @brief Convert packed BCD to a binary integer.
 * @param value Value used by the operation.
 * @return Binary representation.
 */
uint8_t rx8025_bcd_to_bin(uint8_t value);
/**
 * @brief Interpret oscillator validity for the selected RTC model.
 * @param handle Handle of the resource.
 * @param ctrl2 RTC control register value.
 * @return true if the oscillator is reported stopped.
 */
bool rx8025_is_oscillator_stopped(const struct rx8025_dev_t *handle, uint8_t ctrl2);
/**
 * @brief Clear RTC power-loss and oscillator-invalid status.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t rx8025_reset_validity(rx8025_handle_t handle);
