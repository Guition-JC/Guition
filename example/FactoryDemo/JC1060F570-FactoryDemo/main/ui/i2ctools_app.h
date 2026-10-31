/**
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2CTOOLS_APP_ADDRESS_COUNT 128U
#define I2CTOOLS_APP_DUMP_SIZE 128U
#define I2CTOOLS_APP_MAX_WRITE_DATA 256U
#define I2CTOOLS_APP_DEFAULT_FREQUENCY_HZ 100000U
#define I2CTOOLS_APP_DEFAULT_TIMEOUT_MS 50U
#define I2CTOOLS_APP_DEFAULT_GLITCH_IGNORE_COUNT 7U

/** @brief Opaque I2C tools backend. */
typedef struct i2ctools_app i2ctools_app_t;

/** @brief Master bus configuration used by the tools backend. */
typedef struct {
  i2c_port_num_t i2c_port;
  gpio_num_t sda_io_num;
  gpio_num_t scl_io_num;
  i2c_clock_source_t clk_source;
  uint32_t frequency_hz;
  uint32_t timeout_ms;
  uint8_t glitch_ignore_cnt;
  bool enable_internal_pullup;
} i2ctools_app_config_t;

/** @brief Result of probing one I2C address. */
typedef enum {
  I2CTOOLS_APP_PROBE_RESERVED = 0,
  I2CTOOLS_APP_PROBE_ABSENT,
  I2CTOOLS_APP_PROBE_PRESENT,
  I2CTOOLS_APP_PROBE_TIMEOUT,
  I2CTOOLS_APP_PROBE_ERROR,
} i2ctools_app_probe_state_t;

/** @brief Results of a scan across the 7-bit address space. */
typedef struct {
  uint8_t addresses[I2CTOOLS_APP_ADDRESS_COUNT];
  i2ctools_app_probe_state_t state[I2CTOOLS_APP_ADDRESS_COUNT];
  size_t count;
  esp_err_t first_error;
} i2ctools_app_scan_result_t;

/** @brief Register dump data and per-byte validity flags. */
typedef struct {
  uint8_t data[I2CTOOLS_APP_DUMP_SIZE];
  bool valid[I2CTOOLS_APP_DUMP_SIZE];
  size_t valid_count;
  uint8_t chunk_size;
  esp_err_t first_error;
} i2ctools_app_dump_result_t;

/**
 * @brief Initialize defaults with SDA and SCL unset; the caller must select the board pins.
 * @param[out] config Configuration to initialize; NULL is ignored.
 */
void i2ctools_app_config_init(i2ctools_app_config_t *config);

/**
 * @brief Allocate an unconfigured backend.
 * @param[out] out_app Receives the new backend.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL output, or ESP_ERR_NO_MEM.
 */
esp_err_t i2ctools_app_create(i2ctools_app_t **out_app);

/**
 * @brief Configure or reconfigure an I2C master bus owned by the backend.
 * @param app Backend to configure.
 * @param config Bus configuration.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_configure(i2ctools_app_t *app,
                                 const i2ctools_app_config_t *config);

/**
 * @brief Borrow an initialized I2C bus; the caller keeps it valid until deinit or destroy detaches it.
 * @param app Backend to configure.
 * @param config Configuration corresponding to the bus.
 * @param bus Initialized bus to borrow.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_attach_bus(i2ctools_app_t *app,
                                  const i2ctools_app_config_t *config,
                                  i2c_master_bus_handle_t bus);

/**
 * @brief Delete an owned I2C bus, if configured, and release the backend.
 * @param app Backend to destroy.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_destroy(i2ctools_app_t *app);

/**
 * @brief Detach the bus, deleting it if owned, while retaining the backend for reuse.
 * @param app Backend to deinitialize.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_deinit(i2ctools_app_t *app);

/**
 * @brief Scan non-reserved 7-bit addresses from 0x08 through 0x77.
 * @param app Configured backend.
 * @param[out] result Probe states and the list of detected addresses.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_scan(i2ctools_app_t *app,
                            i2ctools_app_scan_result_t *result);

/**
 * @brief Read len bytes beginning at an 8-bit register address.
 * @param app Configured backend.
 * @param device_address 7-bit device address.
 * @param register_address First register to read.
 * @param[out] data Destination buffer.
 * @param len Number of bytes to read.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_read_registers(i2ctools_app_t *app,
                                      uint8_t device_address,
                                      uint8_t register_address, uint8_t *data,
                                      size_t len);

/**
 * @brief Write len bytes beginning at an 8-bit register address.
 * @param app Configured backend.
 * @param device_address 7-bit device address.
 * @param register_address First register to write.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t i2ctools_app_write_registers(i2ctools_app_t *app,
                                       uint8_t device_address,
                                       uint8_t register_address,
                                       const uint8_t *data, size_t len);

/**
 * @brief Read the 128-byte register space in 1-, 2-, or 4-byte chunks. Failed chunks remain invalid; return the
 * first error after attempting the complete dump.
 * @param app Configured backend.
 * @param device_address 7-bit device address.
 * @param chunk_size Number of bytes to read per transaction.
 * @param[out] result Dump data and validity flags.
 * @return ESP_OK on success or the first ESP-IDF error code encountered.
 */
esp_err_t i2ctools_app_dump_registers(i2ctools_app_t *app,
                                      uint8_t device_address,
                                      uint8_t chunk_size,
                                      i2ctools_app_dump_result_t *result);

/**
 * @brief Return the ESP-IDF textual name for an operation result.
 * @param error Operation result.
 * @return Textual name of the error code.
 */
const char *i2ctools_app_error_name(esp_err_t error);

#ifdef __cplusplus
}
#endif
