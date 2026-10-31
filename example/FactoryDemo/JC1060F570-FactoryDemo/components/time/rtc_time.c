/** SPDX-License-Identifier: GPL-2.0-only */
#include "rtc_time.h"

#include <string.h>

#ifndef TIME_RTC_HOST_TEST
#include "esp_board_manager.h"
#include "esp_log.h"

static const char *TAG = "rtc_time";
#define RTC_TIME_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define RTC_TIME_LOGW(...) ((void)0)
#endif

/** @brief Board wiring is owned by esp_board_manager's i2c_master peripheral. */
#define RTC_TIME_I2C_BUS_NAME "i2c_master"
#define RTC_TIME_DEVICE_ADDRESS 0x32U

static rtc_time_ops_t s_ops;
static rx8025_handle_t s_handle;
static bool s_ops_configured;
static rtc_time_result_t s_result;

static time_policy_calendar_t to_policy_calendar(const rx8025_datetime_t *time)
{
    return (time_policy_calendar_t){
        .year = time->year,
        .month = time->month,
        .day = time->day,
        .weekday = time->weekday,
        .hour = time->hour,
        .minute = time->minute,
        .second = time->second,
    };
}

static rx8025_datetime_t to_rx8025_datetime(const time_policy_calendar_t *calendar)
{
    return (rx8025_datetime_t){
        .year = calendar->year,
        .month = calendar->month,
        .day = calendar->day,
        .weekday = calendar->weekday,
        .hour = calendar->hour,
        .minute = calendar->minute,
        .second = calendar->second,
    };
}

static time_policy_rtc_observation_t make_observation(const rx8025_status_t *status,
                                                       const rx8025_datetime_t *time,
                                                       bool calendar_read_ok,
                                                       bool calendar_valid)
{
    time_policy_rtc_observation_t observation = {
        .available = true,
        .status_read_ok = true,
        .calendar_read_ok = calendar_read_ok,
        .calendar_valid = calendar_valid,
        .calendar = {0},
    };
    observation.power_on_reset = status->power_on_reset;
    observation.oscillator_stopped = status->oscillator_stopped;
    observation.voltage_drop = status->voltage_drop;
    if (time != NULL) {
        observation.calendar = to_policy_calendar(time);
    }
    return observation;
}

static bool is_repairable_calendar_error(esp_err_t error)
{
    /** RX8025 uses these for rejected BCD data and its PON/XST validity gate. */
    return error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_INVALID_STATE;
}

#ifdef TIME_RTC_HOST_TEST

static void log_rtc_initial_status(esp_err_t result, const rx8025_status_t *status)
{
    (void)result;
    (void)status;
}

static void log_rtc_initial_read(esp_err_t result, const rx8025_datetime_t *time)
{
    (void)result;
    (void)time;
}

static void log_rtc_repair(esp_err_t result)
{
    (void)result;
}

static void log_rtc_readback_status(esp_err_t result, const rx8025_status_t *status)
{
    (void)result;
    (void)status;
}

static void log_rtc_readback(esp_err_t result, const rx8025_datetime_t *time,
                             int64_t synchronized_epoch, rtc_state_t state)
{
    (void)result;
    (void)time;
    (void)synchronized_epoch;
    (void)state;
}

#else

static void log_rtc_initial_status(esp_err_t result, const rx8025_status_t *status)
{
    ESP_LOGI(TAG, "rtc initial status: result=%s raw_ctrl2=0x%02x pon=%d xst=%d vdet=%d",
             esp_err_to_name(result), status->raw_control2, status->power_on_reset,
             status->oscillator_stopped, status->voltage_drop);
}

static void log_rtc_initial_read(esp_err_t result, const rx8025_datetime_t *time)
{
    ESP_LOGI(TAG, "rtc initial read: result=%s year=%u month=%u day=%u hour=%u minute=%u second=%u",
             esp_err_to_name(result), time->year, time->month, time->day, time->hour,
             time->minute, time->second);
}

static void log_rtc_repair(esp_err_t result)
{
    ESP_LOGI(TAG, "rtc repair: set_time result=%s", esp_err_to_name(result));
}

static void log_rtc_readback_status(esp_err_t result, const rx8025_status_t *status)
{
    ESP_LOGI(TAG, "rtc readback status: result=%s raw_ctrl2=0x%02x pon=%d xst=%d vdet=%d",
             esp_err_to_name(result), status->raw_control2, status->power_on_reset,
             status->oscillator_stopped, status->voltage_drop);
}

static const char *rtc_state_name(rtc_state_t state)
{
    switch (state) {
    case RTC_STATE_HEALTHY:
        return "HEALTHY";
    case RTC_STATE_REPAIR_REQUIRED:
        return "REPAIR_REQUIRED";
    case RTC_STATE_ERROR:
        return "ERROR";
    case RTC_STATE_UNAVAILABLE:
        return "ERROR";
    default:
        return "ERROR";
    }
}

static void log_rtc_readback(esp_err_t result, const rx8025_datetime_t *time,
                             int64_t synchronized_epoch, rtc_state_t state)
{
    int64_t drift_seconds = -1;
    const time_policy_calendar_t calendar = to_policy_calendar(time);
    int64_t readback_epoch = 0;
    if (result == ESP_OK && time_policy_calendar_to_epoch(&calendar, &readback_epoch)) {
        drift_seconds = time_policy_absolute_drift_seconds(readback_epoch, synchronized_epoch);
    }
    ESP_LOGI(TAG, "rtc readback: result=%s drift_seconds=%lld final_state=%s",
             esp_err_to_name(result), (long long)drift_seconds, rtc_state_name(state));
}

#endif

static void inspect_initial_state(void)
{
    rx8025_status_t status = {0};
    esp_err_t ret = s_ops.read_status(s_handle, &status, s_ops.context);
    log_rtc_initial_status(ret, &status);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        return;
    }

    rx8025_datetime_t time = {0};
    ret = s_ops.read_time(s_handle, &time, s_ops.context);
    log_rtc_initial_read(ret, &time);
    const bool calendar_read_ok = ret == ESP_OK || is_repairable_calendar_error(ret);
    if (!calendar_read_ok) {
        s_result.state = RTC_STATE_ERROR;
        return;
    }

    const time_policy_calendar_t calendar = to_policy_calendar(&time);
    const bool calendar_valid = ret == ESP_OK && time_policy_calendar_is_valid(&calendar);
    s_result.state = !calendar_valid || status.power_on_reset || status.oscillator_stopped ||
                             status.voltage_drop
                         ? RTC_STATE_REPAIR_REQUIRED
                         : RTC_STATE_HEALTHY;
}

#ifdef TIME_RTC_HOST_TEST

static esp_err_t default_get_bus(const char *name, void **bus, void *context)
{
    (void)name;
    (void)bus;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t default_new_device(const rx8025_config_t *config, rx8025_handle_t *handle, void *context)
{
    (void)config;
    (void)handle;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t default_read_status(rx8025_handle_t handle, rx8025_status_t *status, void *context)
{
    (void)handle;
    (void)status;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t default_read_time(rx8025_handle_t handle, rx8025_datetime_t *time, void *context)
{
    (void)handle;
    (void)time;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t default_set_time(rx8025_handle_t handle, const rx8025_datetime_t *time, void *context)
{
    (void)handle;
    (void)time;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t default_delete_device(rx8025_handle_t handle, void *context)
{
    (void)handle;
    (void)context;
    return ESP_ERR_NOT_FOUND;
}

#else

static esp_err_t default_get_bus(const char *name, void **bus, void *context)
{
    (void)context;
    return esp_board_manager_get_periph_handle(name, bus);
}

static esp_err_t default_new_device(const rx8025_config_t *config, rx8025_handle_t *handle, void *context)
{
    (void)context;
    return rx8025_new(config, handle);
}

static esp_err_t default_read_status(rx8025_handle_t handle, rx8025_status_t *status, void *context)
{
    (void)context;
    return rx8025_read_status(handle, status);
}

static esp_err_t default_read_time(rx8025_handle_t handle, rx8025_datetime_t *time, void *context)
{
    (void)context;
    return rx8025_read_time(handle, time);
}

static esp_err_t default_set_time(rx8025_handle_t handle, const rx8025_datetime_t *time, void *context)
{
    (void)context;
    return rx8025_set_time(handle, time);
}

static esp_err_t default_delete_device(rx8025_handle_t handle, void *context)
{
    (void)context;
    return rx8025_delete(handle);
}

#endif

static void install_default_ops(void)
{
    if (s_ops_configured) {
        return;
    }
    s_ops = (rtc_time_ops_t){
        .get_bus = default_get_bus,
        .new_device = default_new_device,
        .read_status = default_read_status,
        .read_time = default_read_time,
        .set_time = default_set_time,
        .delete_device = default_delete_device,
    };
    s_ops_configured = true;
}

esp_err_t rtc_time_probe(rtc_time_probe_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    install_default_ops();

    void *bus = NULL;
    esp_err_t ret = s_ops.get_bus(RTC_TIME_I2C_BUS_NAME, &bus, s_ops.context);
    if (ret != ESP_OK || bus == NULL) {
        result->last_error = ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
        return result->last_error;
    }
    result->bus_available = true;

    const rx8025_config_t config = {
        .bus_handle = (i2c_master_bus_handle_t)bus,
        .device_address = RTC_TIME_DEVICE_ADDRESS,
        .scl_speed_hz = RX8025_DEFAULT_SCL_SPEED_HZ,
        .timeout_ms = RX8025_DEFAULT_TIMEOUT_MS,
        .model = RX8025_MODEL_RX8025,
    };
    rx8025_handle_t handle = NULL;
    ret = s_ops.new_device(&config, &handle, s_ops.context);
    if (ret != ESP_OK || handle == NULL) {
        result->last_error = ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
        return result->last_error;
    }
    result->device_opened = true;

    /** Use a status-register read to check RTC communication. */
    ret = s_ops.read_status(handle, &result->status, s_ops.context);
    if (ret == ESP_OK) {
        result->responded = true;
        if (s_ops.read_time(handle, &result->time, s_ops.context) == ESP_OK) {
            result->calendar_read = true;
        }
    }
    result->last_error = ret;

    if (s_ops.delete_device != NULL) {
        const esp_err_t delete_ret = s_ops.delete_device(handle, s_ops.context);
        if (delete_ret != ESP_OK) {
            RTC_TIME_LOGW("probe device delete failed: %d", (int)delete_ret);
        }
    }
    return ret;
}

esp_err_t rtc_time_set_ops(const rtc_time_ops_t *ops)
{
    if (ops == NULL || ops->get_bus == NULL || ops->new_device == NULL ||
        ops->read_status == NULL || ops->read_time == NULL || ops->set_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = *ops;
    s_ops_configured = true;
    s_handle = NULL;
    memset(&s_result, 0, sizeof(s_result));
    return ESP_OK;
}

void rtc_time_reset_ops(void)
{
    memset(&s_ops, 0, sizeof(s_ops));
    s_ops_configured = false;
    s_handle = NULL;
    memset(&s_result, 0, sizeof(s_result));
}

esp_err_t rtc_time_init(void)
{
    install_default_ops();
    if (s_result.initialized) {
        return s_result.available ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    s_result.initialized = true;
    void *bus = NULL;
    esp_err_t ret = s_ops.get_bus(RTC_TIME_I2C_BUS_NAME, &bus, s_ops.context);
    if (ret != ESP_OK || bus == NULL) {
        s_result.state = RTC_STATE_UNAVAILABLE;
        s_result.available = false;
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }

    const rx8025_config_t config = {
        .bus_handle = (i2c_master_bus_handle_t)bus,
        .device_address = RTC_TIME_DEVICE_ADDRESS,
        .scl_speed_hz = RX8025_DEFAULT_SCL_SPEED_HZ,
        .timeout_ms = RX8025_DEFAULT_TIMEOUT_MS,
        .model = RX8025_MODEL_RX8025,
    };
    ret = s_ops.new_device(&config, &s_handle, s_ops.context);
    if (ret != ESP_OK || s_handle == NULL) {
        s_handle = NULL;
        s_result.state = RTC_STATE_UNAVAILABLE;
        s_result.available = false;
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }
    s_result.available = true;
    inspect_initial_state();
    return ESP_OK;
}

esp_err_t rtc_time_reconcile(int64_t synchronized_epoch)
{
    /** This gate is deliberately before init/bus access: an unsynchronized system never writes. */
    if (!time_policy_is_sane_system_epoch(synchronized_epoch)) {
        s_result.state = RTC_STATE_ERROR;
        s_result.repaired = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_result.initialized) {
        const esp_err_t init_ret = rtc_time_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }
    if (!s_result.available || s_handle == NULL) {
        s_result.state = RTC_STATE_UNAVAILABLE;
        s_result.repaired = false;
        return ESP_ERR_NOT_FOUND;
    }

    s_result.repaired = false;
    rx8025_status_t status = {0};
    esp_err_t ret = s_ops.read_status(s_handle, &status, s_ops.context);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        return ret;
    }

    rx8025_datetime_t time = {0};
    ret = s_ops.read_time(s_handle, &time, s_ops.context);
    const bool calendar_read_ok = ret == ESP_OK || is_repairable_calendar_error(ret);
    if (!calendar_read_ok) {
        s_result.state = RTC_STATE_ERROR;
        return ret;
    }
    const time_policy_calendar_t observed_calendar = to_policy_calendar(&time);
    const bool calendar_valid = ret == ESP_OK && time_policy_calendar_is_valid(&observed_calendar);
    const time_policy_rtc_observation_t observation = make_observation(
        &status, &time, calendar_read_ok, calendar_valid);
    const time_policy_rtc_decision_t decision = time_policy_decide_rtc(&observation, synchronized_epoch);
    if (decision == TIME_POLICY_RTC_HEALTHY) {
        s_result.state = RTC_STATE_HEALTHY;
        return ESP_OK;
    }
    if (decision != TIME_POLICY_RTC_REPAIR_REQUIRED) {
        s_result.state = decision == TIME_POLICY_RTC_UNAVAILABLE
            ? RTC_STATE_UNAVAILABLE : RTC_STATE_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    time_policy_calendar_t synchronized_calendar = {0};
    if (!time_policy_epoch_to_calendar(synchronized_epoch, &synchronized_calendar)) {
        s_result.state = RTC_STATE_ERROR;
        return ESP_ERR_INVALID_STATE;
    }
    const rx8025_datetime_t replacement = to_rx8025_datetime(&synchronized_calendar);
    ++s_result.repair_attempts;
    s_result.last_written_time = replacement;
    ret = s_ops.set_time(s_handle, &replacement, s_ops.context);
    log_rtc_repair(ret);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        return ret;
    }

    ++s_result.readback_attempts;
    rx8025_status_t readback_status = {0};
    ret = s_ops.read_status(s_handle, &readback_status, s_ops.context);
    log_rtc_readback_status(ret, &readback_status);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        log_rtc_readback(ret, &(rx8025_datetime_t){0}, synchronized_epoch, s_result.state);
        return ret;
    }
    rx8025_datetime_t readback_time = {0};
    ret = s_ops.read_time(s_handle, &readback_time, s_ops.context);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        log_rtc_readback(ret, &readback_time, synchronized_epoch, s_result.state);
        return ret;
    }
    const time_policy_calendar_t readback_calendar = to_policy_calendar(&readback_time);
    const bool readback_calendar_valid = time_policy_calendar_is_valid(&readback_calendar);
    const time_policy_rtc_observation_t readback = make_observation(
        &readback_status, &readback_time, true, readback_calendar_valid);
    int64_t readback_epoch = 0;
    const bool readback_time_valid = readback_calendar_valid &&
                                     time_policy_calendar_to_epoch(&readback_calendar, &readback_epoch);
    const bool ctrl2_ff_time_valid = readback_status.raw_control2 == 0xffU && readback_time_valid &&
                                     time_policy_absolute_drift_seconds(readback_epoch, synchronized_epoch) <=
                                         TIME_POLICY_RTC_READBACK_TOLERANCE_SECONDS;
    if (!time_policy_is_rtc_readback_acceptable(&readback, synchronized_epoch) &&
        !ctrl2_ff_time_valid) {
        s_result.state = RTC_STATE_ERROR;
        log_rtc_readback(ESP_ERR_INVALID_STATE, &readback_time, synchronized_epoch, s_result.state);
        return ESP_ERR_INVALID_STATE;
    }
    s_result.state = RTC_STATE_HEALTHY;
    s_result.repaired = true;
    log_rtc_readback(ESP_OK, &readback_time, synchronized_epoch, s_result.state);
    return ESP_OK;
}

esp_err_t rtc_time_read_now(int64_t *epoch)
{
    if (epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_result.initialized) {
        const esp_err_t init_ret = rtc_time_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }
    if (!s_result.available || s_handle == NULL) {
        s_result.state = RTC_STATE_UNAVAILABLE;
        return ESP_ERR_NOT_FOUND;
    }

    rx8025_status_t status = {0};
    esp_err_t ret = s_ops.read_status(s_handle, &status, s_ops.context);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        return ret;
    }
    rx8025_datetime_t time = {0};
    ret = s_ops.read_time(s_handle, &time, s_ops.context);
    if (ret != ESP_OK) {
        s_result.state = RTC_STATE_ERROR;
        return ret;
    }
    const time_policy_calendar_t calendar = to_policy_calendar(&time);
    /** Treat control register 0xff as unavailable validity flags and validate the calendar separately. */
    const bool validity_known = status.raw_control2 != 0xffU;
    if (!time_policy_calendar_is_valid(&calendar) ||
        (validity_known &&
         (status.power_on_reset || status.oscillator_stopped || status.voltage_drop))) {
        s_result.state = RTC_STATE_REPAIR_REQUIRED;
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_policy_calendar_to_epoch(&calendar, epoch) ||
        !time_policy_is_sane_system_epoch(*epoch)) {
        s_result.state = RTC_STATE_ERROR;
        return ESP_ERR_INVALID_STATE;
    }
    s_result.state = RTC_STATE_HEALTHY;
    return ESP_OK;
}

esp_err_t rtc_time_get_result(rtc_time_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = s_result;
    return ESP_OK;
}
