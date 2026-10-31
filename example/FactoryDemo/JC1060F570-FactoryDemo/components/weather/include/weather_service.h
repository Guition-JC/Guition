/** SPDX-License-Identifier: GPL-2.0-only */
/** Weather service: owns the provider settings, the last reading and the hourly refresh. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "weather_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Provider API keys are shorter than this; the field only bounds the copy. */
#define WEATHER_SERVICE_KEY_MAX 64
/** @brief Longest city the settings page accepts. */
#define WEATHER_SERVICE_CITY_MAX 48
/** @brief Longest place name a provider answers with (the city it resolved). */
#define WEATHER_SERVICE_PLACE_MAX 63

typedef enum {
    WEATHER_PROVIDER_OPENWEATHER = 0,
    WEATHER_PROVIDER_SENIVERSE = 1,
    WEATHER_PROVIDER_COUNT,
} weather_provider_t;

typedef enum {
    WEATHER_LOCATION_IP = 0,
    WEATHER_LOCATION_CITY = 1,
    WEATHER_LOCATION_COUNT,
} weather_location_mode_t;

/** @brief Weather provider, location and visible-field preferences. */
typedef struct {
    uint8_t provider;      /**< weather_provider_t */
    uint8_t location_mode; /**< weather_location_mode_t */
    uint8_t fields;        /**< weather_field_t mask for the status bar */
    char key[WEATHER_SERVICE_KEY_MAX + 1];
    char city[WEATHER_SERVICE_CITY_MAX + 1];
} weather_config_t;

/** @brief Latest weather reading and refresh status. */
typedef struct {
    bool valid;
    uint8_t condition;      /**< weather_condition_t */
    uint8_t wind_direction; /**< weather_wind_dir_t */
    int16_t temp_c;
    /** Unreported humidity and wind speed use -1. */
    int16_t wind_dmps; /**< Wind speed in 0.1 m/s; negative when unavailable. */
    int16_t humidity; /**< Relative humidity in percent; negative when unavailable. */
    char place[WEATHER_SERVICE_PLACE_MAX + 1];
    int64_t updated_epoch;
    esp_err_t last_error;
} weather_snapshot_t;

/** @brief Run on the weather worker; acquire the display lock before accessing LVGL. */
typedef void (*weather_service_listener_t)(void *context);

/**
 * @brief Fill `config` with the shipped defaults (OpenWeather, IP location, all fields).
 * @param[out] config Receives the default settings.
 */
void weather_service_default_config(weather_config_t *config);

/**
 * @brief Load the stored settings and last reading, then start the hourly worker.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_service_init(void);

/**
 * @brief Copy the stored settings.
 * @param[out] config Receives the stored settings.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_service_get_config(weather_config_t *config);

/**
 * @brief Store these settings, then refresh once with them.
 * @param config Configuration to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_service_set_config(const weather_config_t *config);

/**
 * @brief Copy the last reading; `valid` stays false until the first successful fetch.
 * @param[out] snapshot Receives the copied state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_service_get_snapshot(weather_snapshot_t *snapshot);

/**
 * @brief Ask the worker for an immediate refresh (web button, network came up).
 */
void weather_service_request_refresh(void);

/**
 * @brief Select provider response language and request refreshed localized place names.
 * @param chinese Whether Chinese is requested.
 */
void weather_service_set_language(bool chinese);

/**
 * @brief Register the single change listener; NULL unregisters. Idempotent.
 * @param listener Listener callback.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_service_register_listener(weather_service_listener_t listener, void *context);

/**
 * @brief Stable provider/mode names for the web API; unknown input answers COUNT.
 * @param provider Weather provider.
 * @return Stable provider name.
 */
const char *weather_provider_name(uint8_t provider);
/**
 * @brief Return the stable location-mode name.
 * @param mode Mode to use.
 * @return Stable location-mode name.
 */
const char *weather_location_mode_name(uint8_t mode);
/**
 * @brief Parse a provider name; unknown input returns WEATHER_PROVIDER_COUNT.
 * @param name Name to use.
 * @return Parsed provider, or WEATHER_PROVIDER_COUNT for an unknown name.
 */
uint8_t weather_provider_from_name(const char *name);
/**
 * @brief Parse a location mode; unknown input returns WEATHER_LOCATION_COUNT.
 * @param name Name to use.
 * @return Parsed mode, or WEATHER_LOCATION_COUNT for an unknown name.
 */
uint8_t weather_location_mode_from_name(const char *name);

#ifdef __cplusplus
}
#endif
