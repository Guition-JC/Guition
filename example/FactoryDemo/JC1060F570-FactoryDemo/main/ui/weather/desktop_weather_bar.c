#include "desktop_weather_bar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "desktop_font.h"
#include "desktop_language.h"
#include "assets_storage.h"
#include "weather_rules.h"
#include "weather_service.h"

#define TAG "WeatherBar"
#define L(key) desktop_language_lookup(key)

#define WEATHER_BAR_LOCK_TIMEOUT_MS 100
#define WEATHER_BAR_INSET_X 22
#define WEATHER_BAR_ICON_SIZE 24
#define WEATHER_BAR_GAP 8

#define WEATHER_BAR_TEXT_MAX 64

#define WEATHER_BAR_ICON_DIRECTORY "A:/assets/icon/weather/"
#define WEATHER_BAR_ICON_SUFFIX ".png"

#define C_SYSTEM_TEXT lv_color_hex(0xF8FBFF)

static lv_obj_t *s_row;
static lv_obj_t *s_place;
static lv_obj_t *s_icon;
static lv_obj_t *s_reading;
static lv_obj_t *s_arrow;
static lv_obj_t *s_wind;
static bool s_registered;
static volatile bool s_pending;

static const char *const s_condition_icons[WEATHER_CONDITION_COUNT] = {
    "unknown", "clear", "partly-cloudy", "cloudy", "overcast",
    "rain",    "thunder", "snow",         "fog",   "haze",
    "wind",
};

static const char *const s_wind_arrows[WEATHER_WIND_COUNT] = {
    "arrow-n", "arrow-ne", "arrow-e",  "arrow-se",
    "arrow-s", "arrow-sw", "arrow-w",  "arrow-nw",
};

static desktop_text_key_t condition_key(weather_condition_t condition)
{
    static const desktop_text_key_t keys[WEATHER_CONDITION_COUNT] = {
        DESKTOP_TEXT_WEATHER_UNKNOWN, DESKTOP_TEXT_WEATHER_CLEAR,  DESKTOP_TEXT_WEATHER_PARTLY_CLOUDY,
        DESKTOP_TEXT_WEATHER_CLOUDY,  DESKTOP_TEXT_WEATHER_OVERCAST, DESKTOP_TEXT_WEATHER_RAIN,
        DESKTOP_TEXT_WEATHER_THUNDER, DESKTOP_TEXT_WEATHER_SNOW,   DESKTOP_TEXT_WEATHER_FOG,
        DESKTOP_TEXT_WEATHER_HAZE,    DESKTOP_TEXT_WEATHER_WIND,
    };
    return condition < WEATHER_CONDITION_COUNT ? keys[condition] : DESKTOP_TEXT_WEATHER_UNKNOWN;
}

static desktop_text_key_t wind_key(weather_wind_dir_t direction)
{
    static const desktop_text_key_t keys[WEATHER_WIND_COUNT] = {
        DESKTOP_TEXT_WEATHER_WIND_N,  DESKTOP_TEXT_WEATHER_WIND_NE, DESKTOP_TEXT_WEATHER_WIND_E,
        DESKTOP_TEXT_WEATHER_WIND_SE, DESKTOP_TEXT_WEATHER_WIND_S,  DESKTOP_TEXT_WEATHER_WIND_SW,
        DESKTOP_TEXT_WEATHER_WIND_W,  DESKTOP_TEXT_WEATHER_WIND_NW,
    };
    return direction < WEATHER_WIND_COUNT ? keys[direction] : DESKTOP_TEXT_WEATHER_WIND_N;
}

/**
 * @brief 设置天气图标；调用方须持有 LVGL 适配器锁。
 * @param image 要更新的图像对象。
 * @param name 天气图标名称。
 * @param visible 是否显示图标。
 */
static void weather_bar_image_apply(lv_obj_t *image, const char *name, bool visible)
{
    if (image == NULL) {
        return;
    }
    if (!visible || !assets_storage_icons_ready()) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s%s%s", WEATHER_BAR_ICON_DIRECTORY, name,
             WEATHER_BAR_ICON_SUFFIX);
    lv_image_set_src(image, path);
    if (lv_image_get_src(image) == NULL) {
        ESP_LOGW(TAG, "weather icon unavailable: %s", path);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 更新天气文字；调用方须持有 LVGL 适配器锁。
 * @param label 要更新的标签。
 * @param text 要显示的文本。
 * @param length 文本长度。
 */
static void weather_bar_label_apply(lv_obj_t *label, const char *text, size_t length)
{
    if (label == NULL) {
        return;
    }
    if (length == 0u) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 刷新已创建的天气区域；调用方须持有 LVGL 适配器锁。
 */
static void weather_bar_apply(void)
{
    s_pending = false;
    if (s_row == NULL) {
        return;
    }

    weather_config_t config;
    weather_snapshot_t snapshot;
    if (weather_service_get_config(&config) != ESP_OK ||
        weather_service_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }

    if (!snapshot.valid) {
        weather_bar_label_apply(s_place, "", 0u);
        weather_bar_image_apply(s_icon, s_condition_icons[WEATHER_CONDITION_UNKNOWN], false);
        weather_bar_label_apply(s_reading, "", 0u);
        weather_bar_image_apply(s_arrow, s_wind_arrows[WEATHER_WIND_NORTH], false);
        weather_bar_label_apply(s_wind, "", 0u);
        return;
    }

    const weather_condition_t condition = (weather_condition_t)snapshot.condition;
    const weather_wind_dir_t direction = (weather_wind_dir_t)snapshot.wind_direction;
    const weather_display_t display = {
        .place = snapshot.place,
        .condition = L(condition_key(condition)),
        .wind_direction = L(wind_key(direction)),
        .temp_c = snapshot.temp_c,
        .humidity = snapshot.humidity,
        .wind_dmps = snapshot.wind_dmps,
    };

    char place[WEATHER_BAR_TEXT_MAX];
    char reading[WEATHER_BAR_TEXT_MAX];
    char wind[WEATHER_BAR_TEXT_MAX];
    const size_t place_length =
        weather_display_place_format(place, sizeof(place), config.fields, &display);
    const size_t reading_length =
        weather_display_text_format(reading, sizeof(reading), config.fields, &display);
    const size_t wind_length =
        weather_display_wind_format(wind, sizeof(wind), config.fields, &display);

    weather_bar_label_apply(s_place, place, place_length);
    weather_bar_image_apply(s_icon, s_condition_icons[condition],
                            (config.fields & WEATHER_FIELD_CONDITION_ICON) != 0u);
    weather_bar_label_apply(s_reading, reading, reading_length);
    weather_bar_image_apply(s_arrow, s_wind_arrows[direction],
                            (config.fields & WEATHER_FIELD_WIND_ARROW) != 0u);
    weather_bar_label_apply(s_wind, wind, wind_length);
}

/**
 * @brief 在服务工作线程中尝试获取显示锁，未获取时延后更新。
 * @param context 监听器上下文。
 */
static void weather_service_listener(void *context)
{
    (void)context;
    if (s_row == NULL) {
        s_pending = true;
        return;
    }
    if (!lvgl_display_lock(WEATHER_BAR_LOCK_TIMEOUT_MS)) {
        s_pending = true;
        return;
    }
    weather_bar_apply();
    lvgl_display_unlock();
}

static lv_obj_t *weather_bar_label_create(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_style_text_font(label, desktop_font_get_artist(), 0);
    lv_obj_set_style_text_color(label, C_SYSTEM_TEXT, 0);
    lv_label_set_text(label, "");
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

void desktop_weather_bar_build(lv_obj_t *system_bar)
{
    if (system_bar == NULL) {
        return;
    }

    s_row = lv_obj_create(system_bar);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_style_bg_opa(s_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_row, 0, 0);
    lv_obj_set_style_pad_column(s_row, WEATHER_BAR_GAP, 0);
    lv_obj_clear_flag(s_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_size(s_row, LV_SIZE_CONTENT, WEATHER_BAR_ICON_SIZE);
    lv_obj_align(s_row, LV_ALIGN_LEFT_MID, WEATHER_BAR_INSET_X, 0);

    s_place = weather_bar_label_create(s_row);
    s_icon = lv_image_create(s_row);
    s_reading = weather_bar_label_create(s_row);
    s_arrow = lv_image_create(s_row);
    s_wind = weather_bar_label_create(s_row);

    weather_bar_apply();
}

void desktop_weather_bar_reset(void)
{
    s_row = NULL;
    s_place = NULL;
    s_icon = NULL;
    s_reading = NULL;
    s_arrow = NULL;
    s_wind = NULL;
    s_pending = false;
}

void desktop_weather_bar_register(void)
{
    if (s_registered) {
        return;
    }
    if (weather_service_register_listener(weather_service_listener, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "weather listener unavailable");
        return;
    }
    s_registered = true;

    s_pending = true;
}

void desktop_weather_bar_tick(void)
{
    if (s_pending) {
        weather_bar_apply();
    }
}
