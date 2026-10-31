/** SPDX-License-Identifier: GPL-2.0-only */
/** Public RX8025/RX8035 RTC operations for ESP-IDF. */
#include "rx8025_internal.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rx8025";

static esp_err_t rx8025_validate_time(const rx8025_datetime_t *time)
{
    ESP_RETURN_ON_FALSE(time != NULL && time->year >= 2000U && time->year <= 2099U,
                        ESP_ERR_INVALID_ARG, TAG, "year must be in [2000, 2099]");
    ESP_RETURN_ON_FALSE(time->month >= 1U && time->month <= 12U && time->day >= 1U && time->day <= 31U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid date");
    ESP_RETURN_ON_FALSE(time->weekday <= 6U && time->hour <= 23U && time->minute <= 59U && time->second <= 59U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid time");
    return ESP_OK;
}

static esp_err_t rx8025_validate_alarm(const rx8025_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(alarm != NULL && alarm->hour <= 23U && alarm->minute <= 59U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid alarm");
    return ESP_OK;
}

static uint8_t rx8025_encode_hour(const struct rx8025_dev_t *handle, uint8_t hour)
{
    if (handle->is_24_hour) {
        return rx8025_bin_to_bcd(hour);
    }
    return (uint8_t)((hour >= 12U ? 0x20U : 0U) | rx8025_bin_to_bcd((uint8_t)((hour + 11U) % 12U + 1U)));
}

static bool rx8025_valid_bcd(uint8_t raw, uint8_t mask, uint8_t maximum)
{
    const uint8_t value = raw & mask;
    return (value & 0x0fU) <= 9U && (value >> 4U) <= 9U && rx8025_bcd_to_bin(value) <= maximum;
}

static uint8_t rx8025_decode_hour(const struct rx8025_dev_t *handle, uint8_t raw)
{
    if (handle->is_24_hour) {
        return rx8025_bcd_to_bin(raw & 0x3fU);
    }
    return (uint8_t)(rx8025_bcd_to_bin(raw & 0x1fU) % 12U + ((raw & 0x20U) != 0 ? 12U : 0U));
}

static esp_err_t rx8025_init_device(struct rx8025_dev_t *handle)
{
    uint8_t control[2] = {0};
    esp_err_t ret = rx8025_read_registers(handle, RX8025_REG_CTRL1, control, sizeof(control));
    if (ret != ESP_OK) {
        return ret;
    }

    handle->ctrl1 = (uint8_t)(control[0] & (uint8_t)~RX8025_BIT_CTRL1_TEST);
    if (handle->ctrl1 != control[0]) {
        ret = rx8025_write_register(handle, RX8025_REG_CTRL1, handle->ctrl1);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if ((control[1] & (RX8025_BIT_CTRL2_DAFG | RX8025_BIT_CTRL2_WAFG | RX8025_BIT_CTRL2_CTFG)) != 0) {
        ret = rx8025_clear_interrupt_flags(handle);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (handle->model == RX8025_MODEL_RX8035) {
        uint8_t hour = 0;
        ret = rx8025_read_register(handle, RX8025_REG_HOUR, &hour);
        if (ret != ESP_OK) {
            return ret;
        }
        handle->is_24_hour = (hour & RX8035_BIT_HOUR_1224) != 0;
    } else {
        handle->is_24_hour = (handle->ctrl1 & RX8025_BIT_CTRL1_1224) != 0;
    }
    return ESP_OK;
}

esp_err_t rx8025_new(const rx8025_config_t *config, rx8025_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && ret_handle != NULL && config->bus_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid configuration");
    ESP_RETURN_ON_FALSE(config->device_address > 0U && config->device_address < 0x80U,
                        ESP_ERR_INVALID_ARG, TAG, "device address must be 7-bit");
    ESP_RETURN_ON_FALSE(config->model == RX8025_MODEL_RX8025 || config->model == RX8025_MODEL_RX8035,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported model");
    *ret_handle = NULL;

    struct rx8025_dev_t *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate handle");
    handle->model = config->model;
    handle->timeout_ms = config->timeout_ms > 0U ? config->timeout_ms : RX8025_DEFAULT_TIMEOUT_MS;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->device_address,
        .scl_speed_hz = config->scl_speed_hz > 0U ? config->scl_speed_hz : RX8025_DEFAULT_SCL_SPEED_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(config->bus_handle, &device_config, &handle->i2c_device);
    if (ret != ESP_OK) {
        free(handle);
        return ret;
    }

    ret = rx8025_init_device(handle);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(handle->i2c_device);
        free(handle);
        return ret;
    }
    *ret_handle = handle;
    ESP_LOGI(TAG, "RX-%s device added at 0x%02x", handle->model == RX8025_MODEL_RX8025 ? "8025" : "8035", config->device_address);
    return ESP_OK;
}

esp_err_t rx8025_delete(rx8025_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t ret = i2c_master_bus_rm_device(handle->i2c_device);
    free(handle);
    return ret;
}

esp_err_t rx8025_read_time(rx8025_handle_t handle, rx8025_datetime_t *time)
{
    ESP_RETURN_ON_FALSE(handle != NULL && time != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid time output");
    rx8025_status_t status = {0};
    ESP_RETURN_ON_ERROR(rx8025_read_status(handle, &status), TAG, "read status failed");
    if ((status.power_on_reset || status.oscillator_stopped) && status.raw_control2 != 0xffU) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t date[7] = {0};
    ESP_RETURN_ON_ERROR(rx8025_read_registers(handle, RX8025_REG_SEC, date, sizeof(date)), TAG, "read time failed");
    const bool valid_hour = handle->is_24_hour
        ? rx8025_valid_bcd(date[RX8025_REG_HOUR], 0x3fU, 23U)
        : rx8025_valid_bcd(date[RX8025_REG_HOUR], 0x1fU, 12U);
    if (!rx8025_valid_bcd(date[RX8025_REG_SEC], 0x7fU, 59U) ||
        !rx8025_valid_bcd(date[RX8025_REG_MIN], 0x7fU, 59U) ||
        !valid_hour ||
        !rx8025_valid_bcd(date[RX8025_REG_MDAY], 0x3fU, 31U) ||
        !rx8025_valid_bcd(date[RX8025_REG_MONTH], 0x1fU, 12U) ||
        !rx8025_valid_bcd(date[RX8025_REG_YEAR], 0xffU, 99U)) {
        ESP_LOGW(TAG, "invalid time registers: %02x %02x %02x %02x %02x %02x %02x",
                 date[0], date[1], date[2], date[3], date[4], date[5], date[6]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    time->second = rx8025_bcd_to_bin(date[RX8025_REG_SEC] & 0x7fU);
    time->minute = rx8025_bcd_to_bin(date[RX8025_REG_MIN] & 0x7fU);
    time->hour = rx8025_decode_hour(handle, date[RX8025_REG_HOUR]);
    time->weekday = rx8025_bcd_to_bin(date[RX8025_REG_WDAY] & 0x07U);
    time->day = rx8025_bcd_to_bin(date[RX8025_REG_MDAY] & 0x3fU);
    time->month = rx8025_bcd_to_bin(date[RX8025_REG_MONTH] & 0x1fU);
    time->year = (uint16_t)(2000U + rx8025_bcd_to_bin(date[RX8025_REG_YEAR]));
    return ESP_OK;
}

esp_err_t rx8025_set_time(rx8025_handle_t handle, const rx8025_datetime_t *time)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_ERROR(rx8025_validate_time(time), TAG, "invalid time");
    uint8_t date[7] = {
        rx8025_bin_to_bcd(time->second),
        rx8025_bin_to_bcd(time->minute),
        rx8025_encode_hour(handle, time->hour),
        rx8025_bin_to_bcd(time->weekday),
        rx8025_bin_to_bcd(time->day),
        rx8025_bin_to_bcd(time->month),
        rx8025_bin_to_bcd((uint8_t)(time->year - 2000U)),
    };
    ESP_RETURN_ON_ERROR(rx8025_write_registers(handle, RX8025_REG_SEC, date, sizeof(date)), TAG, "write time failed");
    return rx8025_reset_validity(handle);
}

esp_err_t rx8025_read_alarm(rx8025_handle_t handle, rx8025_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(handle != NULL && alarm != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid alarm output");
    uint8_t values[2] = {0};
    ESP_RETURN_ON_ERROR(rx8025_read_registers(handle, RX8025_REG_ALDMIN, values, sizeof(values)), TAG, "read alarm failed");
    rx8025_status_t status = {0};
    ESP_RETURN_ON_ERROR(rx8025_read_status(handle, &status), TAG, "read alarm status failed");
    const bool valid_hour = handle->is_24_hour
        ? rx8025_valid_bcd(values[1], 0x3fU, 23U)
        : rx8025_valid_bcd(values[1], 0x1fU, 12U);
    if (!rx8025_valid_bcd(values[0], 0x7fU, 59U) || !valid_hour) {
        ESP_LOGW(TAG, "invalid alarm registers: %02x %02x", values[0], values[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    alarm->raw_minute = values[0];
    alarm->raw_hour = values[1];
    alarm->minute = rx8025_bcd_to_bin(values[0] & 0x7fU);
    alarm->hour = rx8025_decode_hour(handle, values[1]);
    alarm->enabled = (handle->ctrl1 & RX8025_BIT_CTRL1_DALE) != 0;
    alarm->pending = status.alarm_flag && alarm->enabled;
    return ESP_OK;
}

esp_err_t rx8025_set_alarm(rx8025_handle_t handle, const rx8025_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_ERROR(rx8025_validate_alarm(alarm), TAG, "invalid alarm");
    const uint8_t values[2] = {rx8025_bin_to_bcd(alarm->minute), rx8025_encode_hour(handle, alarm->hour)};
    const uint8_t old_ctrl1 = handle->ctrl1;
    const uint8_t disabled_ctrl1 = (uint8_t)(old_ctrl1 & (uint8_t)~RX8025_BIT_CTRL1_DALE);
    if (old_ctrl1 != disabled_ctrl1) {
        ESP_RETURN_ON_ERROR(rx8025_write_register(handle, RX8025_REG_CTRL1, disabled_ctrl1), TAG, "disable alarm failed");
    }
    ESP_RETURN_ON_ERROR(rx8025_write_registers(handle, RX8025_REG_ALDMIN, values, sizeof(values)), TAG, "write alarm failed");
    const uint8_t next_ctrl1 = alarm->enabled ? (uint8_t)(disabled_ctrl1 | RX8025_BIT_CTRL1_DALE) : disabled_ctrl1;
    if (next_ctrl1 != disabled_ctrl1) {
        ESP_RETURN_ON_ERROR(rx8025_write_register(handle, RX8025_REG_CTRL1, next_ctrl1), TAG, "enable alarm failed");
    }
    handle->ctrl1 = next_ctrl1;
    return ESP_OK;
}

esp_err_t rx8025_alarm_enable(rx8025_handle_t handle, bool enabled)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    const uint8_t next_ctrl1 = enabled ? (uint8_t)(handle->ctrl1 | RX8025_BIT_CTRL1_DALE) : (uint8_t)(handle->ctrl1 & (uint8_t)~RX8025_BIT_CTRL1_DALE);
    if (next_ctrl1 == handle->ctrl1) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(rx8025_write_register(handle, RX8025_REG_CTRL1, next_ctrl1), TAG, "update alarm enable failed");
    handle->ctrl1 = next_ctrl1;
    return ESP_OK;
}
