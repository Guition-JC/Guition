#include "desktop_ui.h"

#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "display.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_sleep_view.h"
#include "power_idle.h"
#include "power_sleep.h"
#include "sleep_timeout.h"
#include "assets_storage.h"
#include "desktop_language.h"
#include "music/app_music_view.h"
#include "video/app_video_player_view.h"
#include "video/desktop_boot_video.h"
#include "app_wifi_view.h"
#include "app_ethernet_view.h"
#include "serial/app_serial_ui.h"
#include "music_player_service.h"
#include "video_player_service.h"
#include "usb_msc_device.h"
#include "usb_msc_host.h"
#include "usb_storage_controller.h"
#include "usb_stick_app.h"
#include "i2ctools_view.h"
#include "clock/app_clock_view.h"
#include "weather/desktop_weather_bar.h"
#include "settings/app_settings_view.h"
#include "factorytest/app_factory_test_view.h"
#include "aichat/aichat_view.h"
#include "aichat_audio_gmf.h"
#include "aichat_audio_playback.h"
#include "aichat_xiaozhi.h"
#include "touch/app_touch_view.h"
#include "touch/app_touch_gesture.h"
#include "photo/app_photo_view.h"
#include "desktop_wallpaper.h"
#include "wifi_service.h"
#include "aichat_app.h"
#include "spectrum/spectrum_app.h"
#include "time_policy.h"
#include "time_service.h"
#include "weather_service.h"
#include "esp_gmf_afe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define L(key) desktop_language_lookup(key)

#define PAD_W 1024
#define PAD_H 600
#define DESKTOP_MARGIN_X 32
#define DESKTOP_TOP 72
#define DESKTOP_BOTTOM 64
#define DESKTOP_TILE_GAP_X 12
#define DESKTOP_TILE_GAP_Y 12
/** @brief Desktop pages contain five columns and two rows. */
#define DESKTOP_COLUMNS 5
#define DESKTOP_ROWS 2
/** @brief Launch-overlay icon size in pixels. */
#define LAUNCH_ICON_SIZE 96

/** @brief Wi-Fi listener lock timeout and periodic state refresh interval, in milliseconds. */
#define WIFI_INDICATOR_LOCK_TIMEOUT_MS 100
#define WIFI_INDICATOR_REFRESH_PERIOD_MS 60000

/** @brief LVGL lock timeout for scheduling a language rebuild from another task. */
#define LANGUAGE_SWITCH_LOCK_TIMEOUT_MS 1000

/** @brief Poll desktop idle time and update the sleep clock once per second. */
#define IDLE_POLL_PERIOD_MS 1000

/** @brief Shared fullscreen workspace insets in pixels. */
#define FULLSCREEN_INSET_X 40
#define FULLSCREEN_INSET_Y 24

static const char *const USB_UI_TAG = "USBStickUI";

static const char *usb_ui_role_name(usb_storage_role_t role)
{
    return role == USB_STORAGE_ROLE_DEVICE ? "device" :
           role == USB_STORAGE_ROLE_HOST ? "host" : "none";
}

static const char *usb_ui_intent_name(usb_stick_ui_intent_kind_t kind)
{
    switch (kind) {
    case USB_STICK_UI_INTENT_SELECT_DEVICE: return "role-device";
    case USB_STICK_UI_INTENT_SELECT_HOST: return "role-host";
    case USB_STICK_UI_INTENT_START: return "start";
    case USB_STICK_UI_INTENT_READ: return "read";
    case USB_STICK_UI_INTENT_WRITE: return "write";
    case USB_STICK_UI_INTENT_STOP: return "stop";
    case USB_STICK_UI_INTENT_CLOSE: return "close";
    default: return "unknown";
    }
}

#define C_CANVAS      lv_color_hex(0xDCE8F5)
#define C_BLACK       lv_color_hex(0x000000)
/** @brief Match the clock page canvas when painting its fullscreen host. */
#define C_CLOCK_CANVAS lv_color_hex(0x0E1420)
#define C_SURFACE     lv_color_hex(0xF7FAFD)
#define C_MUTED       lv_color_hex(0xE2ECF6)
#define C_INK         lv_color_hex(0x122033)
#define C_INK_MUTED   lv_color_hex(0x53667B)
#define C_SYSTEM      lv_color_hex(0x101A2B)
#define C_SYSTEM_TEXT lv_color_hex(0xF8FBFF)
#define C_ACCENT      lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_SUCCESS     lv_color_hex(0x22A96B)
#define C_WARNING     lv_color_hex(0xED9D33)
#define C_DANGER      lv_color_hex(0xE05151)
#define C_DIVIDER     lv_color_hex(0xD9E0E7)

#define C_DESKTOP_LABEL_TEXT lv_color_hex(0xFFFFFF)

typedef enum {
    APP_STATUS,
    APP_TOUCH,
    APP_SYSTEM,
    APP_SETTINGS,
    APP_FILES,
    APP_ABOUT,
    APP_REFRESH,
    APP_POWER,
    APP_WIFI,
    APP_ETHERNET,
    APP_CALCULATOR,
    APP_MUSIC,
    APP_VIDEO_PLAYER,
    APP_GPIO,
    APP_SERIAL,
    APP_DIAGNOSTICS,
    APP_USBSTICK,
    APP_I2CTOOLS,
    APP_CLOCK,
    APP_AICHAT,
    APP_SPECTRUM,
    APP_ALBUM,
    APP_FACTORY_TEST,
} app_id_t;

typedef struct {
    app_id_t id;
    const char *key;
    desktop_text_key_t label_key;
    desktop_text_key_t icon_key;
    const char *icon_path;
    uint32_t color_hex;
} app_entry_t;

typedef struct {
    const char *label;
    const char *value;
} info_pair_t;

typedef struct {
    desktop_text_key_t label_key;
    desktop_text_key_t value_key;
    const char *literal_value;
} localized_info_pair_t;

typedef enum {
    GPIO_PIN_4,
    GPIO_PIN_7,
    GPIO_PIN_8,
} gpio_pin_id_t;

#define INFO_TEXT(label, value) {label, value, NULL}
#define INFO_LITERAL(label, value) {label, DESKTOP_TEXT_KEY_COUNT, value}

static const localized_info_pair_t s_status_rows[] = {
    INFO_TEXT(DESKTOP_TEXT_STATUS_RUNNING, DESKTOP_TEXT_STATUS_NORMAL),
    INFO_TEXT(DESKTOP_TEXT_STATUS_BOOT_TIME, DESKTOP_TEXT_BOOT_TIME),
    INFO_LITERAL(DESKTOP_TEXT_STATUS_LOAD, "18%"),
    INFO_LITERAL(DESKTOP_TEXT_STATUS_PANEL, "1024 × 600"),
};

static const localized_info_pair_t s_touch_rows[] = {
    INFO_LITERAL(DESKTOP_TEXT_TOUCH_CHIP, "GT911"),
    INFO_TEXT(DESKTOP_TEXT_TOUCH_CONNECTION, DESKTOP_TEXT_WIFI_CONNECTED),
    INFO_LITERAL(DESKTOP_TEXT_TOUCH_RATE, "100 Hz"),
    INFO_TEXT(DESKTOP_TEXT_TOUCH_CALIBRATION, DESKTOP_TEXT_REFRESH_COMPLETE),
};

static const localized_info_pair_t s_system_rows[] = {
    INFO_LITERAL(DESKTOP_TEXT_SYSTEM_VERSION, "Desktop 1.0.4"),
    INFO_LITERAL(DESKTOP_TEXT_SYSTEM_UPTIME, "01:29:16"),
    INFO_LITERAL(DESKTOP_TEXT_SYSTEM_STORAGE, "3.2 GB"),
    INFO_LITERAL(DESKTOP_TEXT_SYSTEM_TEMPERATURE, "36°C"),
};

static const localized_info_pair_t s_files_rows[] = {
    INFO_TEXT(DESKTOP_TEXT_FILES_LOG, DESKTOP_TEXT_REFRESH_COMPLETE),
    INFO_TEXT(DESKTOP_TEXT_FILES_RESOURCES, DESKTOP_TEXT_RESOURCE_COUNT),
    INFO_TEXT(DESKTOP_TEXT_FILES_UPDATED, DESKTOP_TEXT_LAST_UPDATED_TIME),
    INFO_TEXT(DESKTOP_TEXT_FILES_AVAILABLE, DESKTOP_TEXT_STORAGE_AVAILABLE),
};

static const localized_info_pair_t s_about_rows[] = {
    INFO_LITERAL(DESKTOP_TEXT_ABOUT_NAME, "JC1060F570C"),
    INFO_LITERAL(DESKTOP_TEXT_ABOUT_PLATFORM, "ESP32-S3"),
    INFO_LITERAL(DESKTOP_TEXT_ABOUT_FIRMWARE, "FactoryDemo 1.0"),
    INFO_TEXT(DESKTOP_TEXT_ABOUT_LICENSE, DESKTOP_TEXT_DEMO_MODE),
};

static const localized_info_pair_t s_refresh_rows[] = {
    INFO_TEXT(DESKTOP_TEXT_REFRESH_RECENT, DESKTOP_TEXT_DIAGNOSTICS_RECENT),
    INFO_TEXT(DESKTOP_TEXT_REFRESH_SCOPE, DESKTOP_TEXT_REFRESH_SCOPE_VALUE),
    INFO_TEXT(DESKTOP_TEXT_REFRESH_RESULT, DESKTOP_TEXT_REFRESH_COMPLETE),
};

static const app_entry_t s_apps[] = {
    {APP_STATUS, "status", DESKTOP_TEXT_APP_STATUS, DESKTOP_TEXT_APP_STATUS, "A:/assets/icon/img_setting.png", 0x2A8FF5},
    {APP_TOUCH, "touch", DESKTOP_TEXT_APP_TOUCH, DESKTOP_TEXT_APP_TOUCH, "A:/assets/icon/img_touch.png", 0x7C63D8},
    {APP_SYSTEM, "system", DESKTOP_TEXT_APP_SYSTEM, DESKTOP_TEXT_APP_SYSTEM, "A:/assets/icon/img_setting.png", 0xF4A340},
    {APP_SETTINGS, "settings", DESKTOP_TEXT_APP_SETTINGS, DESKTOP_TEXT_APP_SETTINGS, "A:/assets/icon/img_setting.png", 0x687C93},
    {APP_FILES, "files", DESKTOP_TEXT_APP_FILES, DESKTOP_TEXT_APP_FILES, "A:/assets/icon/img_usbstick.png", 0xDF8A3B},
    {APP_ABOUT, "about", DESKTOP_TEXT_APP_ABOUT, DESKTOP_TEXT_APP_ABOUT, "A:/assets/icon/img_setting.png", 0xEC6E87},
    {APP_REFRESH, "refresh", DESKTOP_TEXT_APP_REFRESH, DESKTOP_TEXT_REFRESH_ACTION, "A:/assets/icon/img_setting.png", 0x25A66F},
    {APP_POWER, "power", DESKTOP_TEXT_APP_POWER, DESKTOP_TEXT_APP_POWER, "A:/assets/icon/img_setting.png", 0xE35555},
    {APP_WIFI, "wifi", DESKTOP_TEXT_APP_WIFI, DESKTOP_TEXT_APP_WIFI, "A:/assets/icon/img_wifi.png", 0x2F96D2},
    {APP_ETHERNET, "ethernet", DESKTOP_TEXT_APP_ETHERNET, DESKTOP_TEXT_SERIAL_NETWORK_BUTTON, "A:/assets/icon/img_ethernet.png", 0x3F7AB8},
    {APP_CALCULATOR, "calculator", DESKTOP_TEXT_APP_CALCULATOR, DESKTOP_TEXT_APP_CALCULATOR, "A:/assets/icon/img_calculator.png", 0x6B78C9},
    {APP_MUSIC, "music", DESKTOP_TEXT_APP_MUSIC, DESKTOP_TEXT_APP_MUSIC, "A:/assets/icon/img_music.png", 0xB05AC5},
    {APP_VIDEO_PLAYER, "video", DESKTOP_TEXT_APP_VIDEO_PLAYER, DESKTOP_TEXT_APP_VIDEO_PLAYER, "A:/assets/icon/image_video.png", 0xD84A6A},
    {APP_GPIO, "gpio", DESKTOP_TEXT_APP_GPIO, DESKTOP_TEXT_APP_GPIO, "A:/assets/icon/img_gpio.png", 0x2B9D91},
    {APP_SERIAL, "serial", DESKTOP_TEXT_APP_SERIAL, DESKTOP_TEXT_APP_SERIAL, "A:/assets/icon/image_serial.png", 0x65768D},
    {APP_DIAGNOSTICS, "diagnostics", DESKTOP_TEXT_APP_DIAGNOSTICS, DESKTOP_TEXT_APP_DIAGNOSTICS, "A:/assets/icon/img_setting.png", 0x4D9E69},
    {APP_USBSTICK, "usbstick", DESKTOP_TEXT_APP_USBSTICK, DESKTOP_TEXT_APP_USBSTICK, "A:/assets/icon/img_usbstick.png", 0x1F8A70},
    {APP_I2CTOOLS, "i2ctools", DESKTOP_TEXT_APP_I2CTOOLS, DESKTOP_TEXT_I2CTOOLS_ICON, "A:/assets/icon/img_i2ctools.png", 0x0B4F6C},
    {APP_CLOCK, "clock", DESKTOP_TEXT_FULLSCREEN_CLOCK, DESKTOP_TEXT_FULLSCREEN_CLOCK, "A:/assets/icon/img_clock.png", 0x147EFB},
    {APP_AICHAT, "aichat", DESKTOP_TEXT_APP_AICHAT, DESKTOP_TEXT_APP_AICHAT, "A:/assets/icon/img_aichat.png", 0x147EFB},
    {APP_SPECTRUM, "spectrum", DESKTOP_TEXT_APP_SPECTRUM, DESKTOP_TEXT_APP_SPECTRUM, "A:/assets/icon/img_spectrum.png", 0x2C9DFF},
    {APP_ALBUM, "album", DESKTOP_TEXT_APP_ALBUM, DESKTOP_TEXT_APP_ALBUM, "A:/assets/icon/img_picture.png", 0x2D7DD2},
    {APP_FACTORY_TEST, "factorytest", DESKTOP_TEXT_APP_FACTORY_TEST, DESKTOP_TEXT_APP_FACTORY_TEST,
     "A:/assets/icon/img_test.png", 0x1F7A6B},
};

#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
LV_IMAGE_DECLARE(image_serial);
LV_IMAGE_DECLARE(image_video);
LV_IMAGE_DECLARE(img_aichat);
LV_IMAGE_DECLARE(img_calculator);
LV_IMAGE_DECLARE(img_clock);
LV_IMAGE_DECLARE(img_ethernet);
LV_IMAGE_DECLARE(img_gpio);
LV_IMAGE_DECLARE(img_i2ctools);
LV_IMAGE_DECLARE(img_music);
LV_IMAGE_DECLARE(img_picture);
LV_IMAGE_DECLARE(img_setting);
LV_IMAGE_DECLARE(img_spectrum);
LV_IMAGE_DECLARE(img_test);
LV_IMAGE_DECLARE(img_touch);
LV_IMAGE_DECLARE(img_usbstick);
LV_IMAGE_DECLARE(img_wifi);
#endif

static bool desktop_icons_ready(void)
{
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
    return true;
#else
    return assets_storage_icons_ready();
#endif
}

static const void *app_icon_source(const app_entry_t *app)
{
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
    switch (app->id) {
    case APP_STATUS:
    case APP_SYSTEM:
    case APP_SETTINGS:
    case APP_ABOUT:
    case APP_REFRESH:
    case APP_POWER:
    case APP_DIAGNOSTICS: return &img_setting;
    case APP_TOUCH: return &img_touch;
    case APP_FILES:
    case APP_USBSTICK: return &img_usbstick;
    case APP_WIFI: return &img_wifi;
    case APP_ETHERNET: return &img_ethernet;
    case APP_CALCULATOR: return &img_calculator;
    case APP_MUSIC: return &img_music;
    case APP_VIDEO_PLAYER: return &image_video;
    case APP_SERIAL: return &image_serial;
    case APP_I2CTOOLS: return &img_i2ctools;
    case APP_CLOCK: return &img_clock;
    case APP_AICHAT: return &img_aichat;
    case APP_SPECTRUM: return &img_spectrum;
    case APP_ALBUM: return &img_picture;
    case APP_FACTORY_TEST: return &img_test;
    case APP_GPIO: return &img_gpio;
    }
    return NULL;
#else
    return app->icon_path;
#endif
}

#define DESKTOP_PAGE_COUNT 2U
#define DESKTOP_APPS_PER_PAGE (DESKTOP_COLUMNS * DESKTOP_ROWS)

/** @brief Desktop shortcut order and page membership. */
static const uint8_t s_pages[DESKTOP_PAGE_COUNT][DESKTOP_APPS_PER_PAGE] = {
    {APP_TOUCH, APP_SETTINGS, APP_WIFI,
#if CONFIG_ETHERNET_PHY_IP101
     APP_ETHERNET,
#else
     APP_GPIO,
#endif
     APP_CALCULATOR,
     APP_MUSIC, APP_VIDEO_PLAYER, APP_SERIAL, APP_USBSTICK, APP_I2CTOOLS},
    /** Only s_page_counts entries are active on each page. */
    {APP_CLOCK, APP_AICHAT, APP_SPECTRUM, APP_ALBUM, APP_FACTORY_TEST},
};

static const uint8_t s_page_counts[DESKTOP_PAGE_COUNT] = {10, 5};
static const desktop_text_key_t s_page_label_keys[DESKTOP_PAGE_COUNT][DESKTOP_APPS_PER_PAGE] = {
    {DESKTOP_TEXT_APP_TOUCH, DESKTOP_TEXT_APP_SETTINGS, DESKTOP_TEXT_APP_WIFI,
#if CONFIG_ETHERNET_PHY_IP101
     DESKTOP_TEXT_APP_ETHERNET,
#else
     DESKTOP_TEXT_APP_GPIO,
#endif
     DESKTOP_TEXT_APP_CALCULATOR, DESKTOP_TEXT_APP_MUSIC,
     DESKTOP_TEXT_APP_VIDEO_PLAYER, DESKTOP_TEXT_APP_SERIAL, DESKTOP_TEXT_APP_USBSTICK,
     DESKTOP_TEXT_I2CTOOLS_ICON},
    {DESKTOP_TEXT_FULLSCREEN_CLOCK, DESKTOP_TEXT_APP_AICHAT, DESKTOP_TEXT_APP_SPECTRUM,
     DESKTOP_TEXT_APP_ALBUM, DESKTOP_TEXT_APP_FACTORY_TEST},
};
static lv_obj_t *s_root;
/** @brief Own desktop widgets in a container that can be hidden during boot playback. */
static lv_obj_t *s_desktop;
static bool s_desktop_hidden;
static uint16_t s_screen_width;
static uint16_t s_screen_height;
static lv_obj_t *s_grid;
static lv_obj_t *s_page_dots[DESKTOP_PAGE_COUNT];
static lv_obj_t *s_clock_label;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_bt_icon;
/** @brief Cache the Bluetooth icon state; the LVGL clock timer polls for changes. */
static bool s_bt_indicator_shown;
static lv_timer_t *s_wifi_timer;
static bool s_wifi_indicator_registered;

/** @brief Queue desktop labels while boot video is visible and apply them in bounded batches. */
#define DEFERRED_TEXT_MAX 48U
#define DEFERRED_TEXT_LEN 72U
#define DEFERRED_TEXT_PER_TICK 6U
#define DEFERRED_TEXT_TICK_MS 10U
/** @brief Boot-video completion polling interval in milliseconds. */
#define DESKTOP_REVEAL_POLL_MS 20U

typedef struct {
    lv_obj_t *label;
    char text[DEFERRED_TEXT_LEN];
} deferred_text_t;

static deferred_text_t s_deferred[DEFERRED_TEXT_MAX];
static size_t s_deferred_count;
static lv_timer_t *s_deferred_timer;
static bool s_defer_text;

static int64_t s_build_mark;

static void deferred_tiles_drop(void);
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
static void deferred_tiles_flush(void);
#endif

static void build_phase(const char *name)
{
    if (!s_defer_text) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    ESP_LOGI("desktop_ui", "build phase %-16s %4d ms", name, (int)((now - s_build_mark) / 1000));
    s_build_mark = now;
}

/**
 * @brief Discard pending text before its labels are destroyed.
 */
static void deferred_text_drop(void)
{
    s_deferred_count = 0U;
    if (s_deferred_timer != NULL) {
        lv_timer_delete(s_deferred_timer);
        s_deferred_timer = NULL;
    }
}

/**
 * @brief Set text immediately or defer it while boot video covers the desktop.
 * @param label Label to update.
 * @param text Text to display.
 */
static void set_label_text(lv_obj_t *label, const char *text)
{
    const char *value = text == NULL ? "" : text;
    const size_t length = strlen(value);
    if (!s_defer_text || length == 0U || length >= DEFERRED_TEXT_LEN ||
        s_deferred_count >= DEFERRED_TEXT_MAX) {
        lv_label_set_text(label, value);
        return;
    }
    deferred_text_t *entry = &s_deferred[s_deferred_count++];
    entry->label = label;
    memcpy(entry->text, value, length + 1U);
    /** Initialize deferred labels with empty text. */
    lv_label_set_text(label, "");
}

static void deferred_text_apply(lv_timer_t *timer)
{
    /** Wait for boot-video coverage to end before applying deferred text. */
    if (desktop_boot_video_is_playing()) {
        return;
    }
    size_t applied = 0U;
    while (s_deferred_count > 0U && applied < DEFERRED_TEXT_PER_TICK) {
        const deferred_text_t entry = s_deferred[0];
        --s_deferred_count;
        memmove(&s_deferred[0], &s_deferred[1], s_deferred_count * sizeof(s_deferred[0]));
        /** Preserve text already supplied by a live status callback. */
        if (lv_label_get_text(entry.label)[0] == '\0') {
            lv_label_set_text(entry.label, entry.text);
            /** Recompute label bounds before repainting newly assigned text. */
            lv_obj_update_layout(entry.label);
            lv_obj_invalidate(entry.label);
        }
        ++applied;
    }
    if (s_deferred_count == 0U) {
        lv_timer_delete(timer);
        s_deferred_timer = NULL;
    }
}

/**
 * @brief Create the desktop container and hide it while boot video covers the display.
 * @return The desktop root container.
 */
static lv_obj_t *desktop_root(void)
{
    if (s_desktop == NULL) {
        /** Use a black screen background during the transition from boot video. */
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        s_desktop = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(s_desktop, 0, 0);
        lv_obj_set_size(s_desktop, s_screen_width, s_screen_height);
        lv_obj_clear_flag(s_desktop, LV_OBJ_FLAG_SCROLLABLE);
        /** Resolve container dimensions before building size-dependent children. */
        lv_obj_mark_layout_as_dirty(s_desktop);
        lv_obj_update_layout(s_desktop);
    }
    return s_desktop;
}

/**
 * @brief Reveal SPIFFS tiles near video completion and embedded tiles after video teardown.
 * @param timer LVGL reveal timer.
 */
static void desktop_reveal_cb(lv_timer_t *timer)
{
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED

    if (desktop_boot_video_is_playing()) {
        return;
    }
    deferred_tiles_flush();
#endif
    if (desktop_boot_video_covers_desktop()) {
        return;
    }
    lv_timer_delete(timer);
    if (s_desktop == NULL || !s_desktop_hidden) {
        return;
    }
    s_desktop_hidden = false;
    lv_obj_remove_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI("desktop_ui", "desktop shown");
}

/** @brief Written by the Wi-Fi event task and read by the LVGL task. */
static volatile bool s_wifi_indicator_connected;
static volatile bool s_wifi_indicator_pending;
static lv_obj_t *s_modal;
static lv_obj_t *s_modal_panel;
static lv_obj_t *s_modal_title;
static lv_obj_t *s_modal_subtitle;
static lv_obj_t *s_modal_content;
static lv_obj_t *s_full;
static bool s_app_launching;
static lv_obj_t *s_full_header;
static lv_obj_t *s_full_title;
static lv_obj_t *s_full_subtitle;
static lv_obj_t *s_full_body;
static lv_obj_t *s_full_close_button;
static lv_obj_t *s_full_exit_strip;
static app_touch_gesture_t s_full_exit_gesture;
static bool s_full_exit_tracking;
static bool s_full_exit_requested;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_timer_t *s_clock_timer;
static lv_timer_t *s_idle_timer;
static bool s_idle_activity_hooked;
static bool s_sleeping;
static power_idle_state_t s_idle_policy;
static power_sleep_state_t s_sleep_power;
static lv_timer_t *s_usb_poll_timer;
static usb_stick_app_t *s_usb_page;
static usb_storage_controller_t *s_usb_controller;
static usb_storage_role_t s_usb_selected_role = USB_STORAGE_ROLE_HOST;
static bool s_usb_app_open;
static bool s_usb_close_requested;
static desktop_language_t s_usb_saved_language = DESKTOP_LANGUAGE_ENGLISH;
static bool s_usb_language_overridden;
static bool s_usb_msc_session_started;
static aichat_app_t *s_aichat_app;
static aichat_view_t *s_aichat_view;
static aichat_audio_gmf_t *s_aichat_audio;
static aichat_audio_playback_t *s_aichat_playback;
static aichat_xiaozhi_t *s_aichat_xiaozhi;
static QueueHandle_t s_aichat_event_queue;
static lv_timer_t *s_aichat_poll_timer;
static bool s_aichat_accept_events;
static bool s_aichat_app_open;

typedef enum {
    AICHAT_UI_EVENT_CONNECTED = 0,
    AICHAT_UI_EVENT_DISCONNECTED,
    AICHAT_UI_EVENT_AUDIO_OPENED,
    AICHAT_UI_EVENT_AUDIO_CLOSED,
    AICHAT_UI_EVENT_TEXT,
    AICHAT_UI_EVENT_TTS_START,
    AICHAT_UI_EVENT_TTS_STOP,
    AICHAT_UI_EVENT_ERROR,
    AICHAT_UI_EVENT_WAKEUP,
    AICHAT_UI_EVENT_VAD_END,
    AICHAT_UI_EVENT_WAKEUP_END,
    AICHAT_UI_EVENT_SERVER_GOODBYE,
} aichat_ui_event_kind_t;

typedef struct {
    aichat_ui_event_kind_t kind;
    esp_xiaozhi_chat_text_role_t role;
    esp_err_t error;
    char text[192];
} aichat_ui_event_t;

typedef struct {
    usb_msc_device_handle_t handle;
    usb_storage_backend_event_cb_t callback;
    void *callback_arg;
    uint32_t generation;
    bool initialized;
} usb_ui_device_backend_t;

typedef struct {
    usb_storage_backend_event_cb_t callback;
    void *callback_arg;
    uint32_t generation;
    bool initialized;
    bool media_present;
    bool mounted;
} usb_ui_host_backend_t;

static usb_ui_device_backend_t s_usb_device_backend;
static usb_ui_host_backend_t s_usb_host_backend;
static pthread_mutex_t s_usb_notification_mutex = PTHREAD_MUTEX_INITIALIZER;
static usb_storage_controller_notification_t s_usb_notification;
static bool s_usb_notification_valid;
static bool s_language_switch_pending;
static bool s_font_reload_pending;
/** @brief Keep font-notice state across desktop rebuilds. */
static bool s_font_notice_shown;
/** @brief Preserve the font-selection result across the settings-page rebuild. */
static char s_font_toast[DESKTOP_FONT_NAME_MAX + 64U];
static uint8_t s_current_page;
/** @brief Limit calculation history to the newest entries owned by the current page. */
#define CALC_HISTORY_MAX 20U

#define CALC_BUBBLE_TEXT_MAX 96U
static char s_calc_text[32] = "0";
static double s_calc_accumulator;
static char s_calc_operator;
static bool s_calc_new_value = true;
static lv_obj_t *s_calc_display;
static lv_obj_t *s_calc_history;
static bool usb_ui_cleanup_if_safe(void);
static bool usb_ui_finish_close(void);
static void usb_ui_render_current(void);
static void usb_ui_poll_cb(lv_timer_t *timer);
static void render_page(uint8_t page);
static void usb_ui_intent_cb(const usb_stick_ui_intent_t *intent, void *user_data);
static void aichat_ui_intent_cb(aichat_view_intent_t intent, void *user_data);
static void aichat_ui_cleanup(void);
static void aichat_audio_pcm_cb(const uint8_t *data, size_t len, void *context);
static void aichat_audio_afe_cb(int event_type, void *event_data, void *context);
static void aichat_xiaozhi_event_cb(aichat_xiaozhi_event_t event, const void *data, void *context);
static void aichat_ui_poll_cb(lv_timer_t *timer);
static void aichat_ui_render(void);
static void aichat_ui_set_binding_notice(const aichat_xiaozhi_status_t *status,
                                         bool transport_failure);
static void close_full_cb(lv_event_t *event);
static void sleep_enter(void);
static void touch_view_close_cb(void *user_data);
static void full_exit_strip_cb(lv_event_t *event);
static void settings_language_cb(desktop_language_t language);
static void request_language_switch(desktop_language_t language);
static void settings_font_cb(const char *file_name, bool applied);

static void usb_ui_rebind_fonts(void)
{
    const lv_font_t *font = desktop_font_get_primary();
    if (s_root != NULL && font != NULL) {
        lv_obj_set_style_text_font(s_root, font, 0);
    }
    /** Rebind desktop text and tile captions before releasing SD fonts. */
    if (s_clock_label != NULL) {
        lv_obj_set_style_text_font(s_clock_label, desktop_font_get_artist(), 0);
    }
    if (s_grid != NULL) {
        render_page(s_current_page);
    }
    usb_stick_app_refresh_fonts(s_usb_page);
}

static void usb_ui_set_usb_header_language(void)
{
    if (s_full_title != NULL) {
        lv_label_set_text(s_full_title, L(DESKTOP_TEXT_APP_USBSTICK));
    }
    if (s_full_subtitle != NULL) {
        lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_USB_SUBTITLE));
    }
}

static bool usb_ui_prepare_msc_start(void)
{
    if (!s_usb_language_overridden) {
        s_usb_saved_language = desktop_language_current();
        s_usb_language_overridden = true;
    }
    (void)desktop_language_select(DESKTOP_LANGUAGE_ENGLISH);
    desktop_font_prepare_for_sd_unmount();
    usb_ui_rebind_fonts();
    usb_ui_set_usb_header_language();

    const esp_err_t result = desktop_font_release_sd_resources();
    ESP_LOGI(USB_UI_TAG, "font-sd-release result=%s", esp_err_to_name(result));
    return result == ESP_OK;
}

static void usb_ui_restore_msc_resources(void)
{
    if (!s_usb_language_overridden) {
        return;
    }
    desktop_font_init();
    desktop_language_t language = s_usb_saved_language;
    if (language == DESKTOP_LANGUAGE_CHINESE && !desktop_font_has_chinese()) {
        language = DESKTOP_LANGUAGE_ENGLISH;
    }
    if (!desktop_language_select(language)) {
        language = DESKTOP_LANGUAGE_ENGLISH;
        (void)desktop_language_select(language);
    }
    usb_ui_rebind_fonts();
    usb_ui_set_usb_header_language();
    s_usb_language_overridden = false;
    s_usb_msc_session_started = false;
    ESP_LOGI(USB_UI_TAG, "font-language-restored language=%s",
             language == DESKTOP_LANGUAGE_CHINESE ? "chinese" : "english");
}

static void usb_ui_restore_msc_resources_if_idle(void)
{
    if (!s_usb_msc_session_started || s_usb_controller == NULL) {
        return;
    }
    usb_storage_controller_snapshot_t snapshot;
    if (usb_storage_controller_get_snapshot(s_usb_controller, &snapshot) !=
        USB_STORAGE_CONTROLLER_OK) {
        return;
    }
    if (snapshot.generation == 0U || snapshot.state.lifecycle != USB_STORAGE_LIFECYCLE_IDLE ||
        snapshot.backend_active || snapshot.mounted) {
        return;
    }
    usb_ui_restore_msc_resources();
}

static const app_entry_t *app_by_id(app_id_t id)
{
    for (size_t i = 0; i < sizeof(s_apps) / sizeof(s_apps[0]); i++) {
        if (s_apps[i].id == id) {
            return &s_apps[i];
        }
    }
    return &s_apps[0];
}

static const char *app_label(const app_entry_t *app)
{
    return L(app->label_key);
}

static void style_plain(lv_obj_t *obj, lv_color_t bg, int radius)
{
    /** Yield the display lock to boot-video rendering during desktop construction. */
    desktop_boot_video_yield();
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void style_surface(lv_obj_t *obj, lv_color_t bg, int radius)
{
    style_plain(obj, bg, radius);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, C_DIVIDER, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color, int x, int y)
{
    desktop_boot_video_yield();
    lv_obj_t *label = lv_label_create(parent);
    set_label_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                             lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    style_surface(button, bg, 12);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 2, LV_STATE_FOCUSED);
    if (cb != NULL) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *label = lv_label_create(button);
    set_label_text(label, text);
    lv_obj_set_style_text_color(label, C_INK, 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_surface(card, C_SURFACE, 16);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/**
 * @brief Centre white captions below icons; double-strike only built-in bitmap fonts.
 * @param tile Tile that owns the caption.
 * @param icon Icon above the caption.
 * @param text Caption text.
 * @param tile_width Tile width in pixels.
 */
static void make_icon_caption(lv_obj_t *tile, lv_obj_t *icon, const char *text, int tile_width)
{
    const lv_font_t *font = desktop_font_get_artist();
    lv_obj_t *caption = make_label(tile, text, C_DESKTOP_LABEL_TEXT, 0, 0);
    lv_obj_set_style_text_font(caption, font, 0);
    lv_obj_set_width(caption, tile_width);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(caption, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(caption, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    if (desktop_font_using_sd_font()) {
        return;
    }

    lv_obj_t *weight = make_label(tile, text, C_DESKTOP_LABEL_TEXT, 0, 0);
    lv_obj_set_style_text_font(weight, font, 0);
    lv_obj_set_width(weight, tile_width);
    lv_obj_set_style_text_align(weight, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(weight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(weight, caption, LV_ALIGN_TOP_LEFT, 1, 0);
}

static void toast_hide_cb(lv_timer_t *timer)
{
    if (s_toast != NULL) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_pause(timer);
}

static void show_toast(const char *message)
{
    lv_label_set_text(s_toast, message);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer == NULL) {
        s_toast_timer = lv_timer_create(toast_hide_cb, 2600, NULL);
    } else {
        lv_timer_set_period(s_toast_timer, 2600);
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
    }
}

static void set_text_pair(lv_obj_t *parent, int y, const char *label, const char *value)
{
    lv_obj_t *row = make_card(parent, 0, y, 456, 48);
    make_label(row, label, C_INK_MUTED, 14, 15);
    lv_obj_t *value_label = make_label(row, value, C_INK, 220, 15);
    lv_obj_set_width(value_label, 220);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
}

static void populate_rows(const info_pair_t *rows, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        set_text_pair(s_modal_content, (int)i * 54, rows[i].label, rows[i].value);
    }
}

static void populate_localized_rows(const localized_info_pair_t *rows, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const char *value = rows[i].value_key == DESKTOP_TEXT_KEY_COUNT
                                ? rows[i].literal_value
                                : L(rows[i].value_key);
        set_text_pair(s_modal_content, (int)i * 54, L(rows[i].label_key), value);
    }
}

static void close_modal_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void touch_view_close_cb(void *user_data)
{
    (void)user_data;
    close_full_cb(NULL);
}

/**
 * @brief Hide the shared exit strip in factory-test subviews and restore it at the menu.
 * @param covered Whether a diagnostic subview covers the menu.
 * @param user_data Callback context (unused).
 */
static void factory_test_cover_cb(bool covered, void *user_data)
{
    (void)user_data;
    if (s_full_exit_strip == NULL) {
        return;
    }
    if (covered) {
        lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_full_exit_strip);
    }
}

static void full_exit_strip_cb(lv_event_t *event)
{
    if (event == NULL || s_full_exit_strip == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        if (lv_indev_active() != NULL) {
            lv_point_t point;
            lv_indev_get_point(lv_indev_active(), &point);
            if (point.x >= 0 && point.y >= 0 && point.x < s_screen_width &&
                point.y < s_screen_height) {
                s_full_exit_tracking = app_touch_gesture_begin(
                    &s_full_exit_gesture, (uint16_t)point.x, (uint16_t)point.y);
                s_full_exit_requested = false;
            }
        }
    } else if (code == LV_EVENT_PRESSING && s_full_exit_tracking &&
               !s_full_exit_requested && lv_indev_active() != NULL) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        if (point.x >= 0 && point.y >= 0 && point.x < s_screen_width &&
            point.y < s_screen_height &&
            app_touch_gesture_update(&s_full_exit_gesture, (uint16_t)point.x,
                                       (uint16_t)point.y) == APP_TOUCH_GESTURE_EXIT) {
            s_full_exit_requested = true;
            close_full_cb(NULL);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_full_exit_tracking = false;
        app_touch_gesture_end(&s_full_exit_gesture);
    }

    lv_event_stop_bubbling(event);
}

static void close_full_cb(lv_event_t *event)
{
    (void)event;
    if (s_usb_app_open) {
        const usb_storage_controller_result_t result =
            usb_storage_controller_request_close(s_usb_controller);
        if (result != USB_STORAGE_CONTROLLER_OK) {
            s_usb_close_requested = result == USB_STORAGE_CONTROLLER_BUSY;
            usb_ui_render_current();
            if (result != USB_STORAGE_CONTROLLER_BUSY) {
                show_toast(L(DESKTOP_TEXT_USB_SERVICE_UNAVAILABLE));
            }
            return;
        }
        (void)usb_ui_finish_close();
        return;
    }

    s_full_exit_tracking = false;
    s_full_exit_requested = false;
    app_touch_gesture_end(&s_full_exit_gesture);
    if (s_full_exit_strip != NULL) {
        lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
    }

    const int64_t exit_started_us = esp_timer_get_time();
    app_touch_view_cleanup();
    app_wifi_view_cleanup();
    i2ctools_view_cleanup();
    app_ethernet_view_cleanup();
    app_serial_ui_destroy();
    app_photo_view_cleanup();
    app_settings_view_destroy();
    app_factory_test_view_cleanup();
    (void)video_player_service_exit();
    app_video_player_view_cleanup();
    app_music_view_cleanup();
    aichat_ui_cleanup();
    spectrum_app_stop();
    const esp_err_t music_exit_ret = music_player_service_return_home();
    if (music_exit_ret != ESP_OK) {
        show_toast(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
    }
    app_clock_view_destroy();
    lv_obj_add_flag(s_full, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI("desktop_ui", "app-exit total_ms=%" PRId64,
             (esp_timer_get_time() - exit_started_us) / 1000);
}

static void modal_backdrop_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_modal) {
        close_modal_cb(event);
    }
}

static void prepare_modal(const app_entry_t *app, const char *subtitle)
{
    lv_label_set_text(s_modal_title, app_label(app));
    lv_label_set_text(s_modal_subtitle, subtitle);
    lv_obj_clean(s_modal_content);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}

static void power_action_cb(lv_event_t *event)
{
    desktop_text_key_t action_key =
        (desktop_text_key_t)(uintptr_t)lv_event_get_user_data(event);
    /** Use the shared sleep-entry path for explicit power-menu requests. */
    if (action_key == DESKTOP_TEXT_POWER_SLEEP) {
        close_modal_cb(event);
        sleep_enter();
        return;
    }
    const char *action = L(action_key);
    char message[64];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_ACTION_SENT), action);
    close_modal_cb(event);
    show_toast(message);
}

static void open_power(void)
{
    const info_pair_t rows[] = {
        {L(DESKTOP_TEXT_POWER_BATTERY), "76%"},
        {L(DESKTOP_TEXT_POWER_EXTERNAL), L(DESKTOP_TEXT_POWER_UNCONNECTED)},
        {L(DESKTOP_TEXT_POWER_SAVING), L(DESKTOP_TEXT_POWER_NOT_ENABLED)},
    };
    prepare_modal(app_by_id(APP_POWER), L(DESKTOP_TEXT_POWER_TITLE));
    populate_rows(rows, sizeof(rows) / sizeof(rows[0]));
    make_button(s_modal_content, L(DESKTOP_TEXT_POWER_SLEEP), 0, 174, 142, 66, C_MUTED,
                power_action_cb, (void *)(uintptr_t)DESKTOP_TEXT_POWER_SLEEP);
    make_button(s_modal_content, L(DESKTOP_TEXT_POWER_RESTART), 157, 174, 142, 66, C_MUTED,
                power_action_cb, (void *)(uintptr_t)DESKTOP_TEXT_POWER_RESTART);
    make_button(s_modal_content, L(DESKTOP_TEXT_POWER_SHUTDOWN), 314, 174, 142, 66, C_MUTED,
                power_action_cb, (void *)(uintptr_t)DESKTOP_TEXT_POWER_SHUTDOWN);
}

/**
 * @brief Persist dismissal of the font notice and close the dialog.
 * @param event LVGL button event.
 */
static void font_notice_never_cb(lv_event_t *event)
{
    const esp_err_t result = desktop_font_disable_boot_notice();
    if (result != ESP_OK) {
        ESP_LOGW("desktop_ui", "could not store the font notice choice: %s",
                 esp_err_to_name(result));
    }
    close_modal_cb(event);
}

static void show_font_notice(void)
{
    lv_label_set_text(s_modal_title, L(DESKTOP_TEXT_FONT_NOTICE_TITLE));
    lv_label_set_text(s_modal_subtitle, "");
    lv_obj_clean(s_modal_content);

    lv_obj_t *body = lv_label_create(s_modal_content);
    lv_label_set_text(body, L(DESKTOP_TEXT_FONT_NOTICE_BODY));
    lv_obj_set_style_text_color(body, C_INK, 0);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    make_button(s_modal_content, L(DESKTOP_TEXT_FONT_NOTICE_NEVER), 0, 150, 224, 66, C_MUTED,
                font_notice_never_cb, NULL);
    make_button(s_modal_content, L(DESKTOP_TEXT_FONT_NOTICE_OK), 248, 150, 224, 66, C_ACCENT_SOFT,
                close_modal_cb, NULL);

    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}

static void open_info_modal(app_id_t id)
{
    const localized_info_pair_t *rows = s_status_rows;
    size_t count = 4;
    desktop_text_key_t subtitle_key = DESKTOP_TEXT_STATUS_SUBTITLE;

    if (id == APP_TOUCH) {
        rows = s_touch_rows;
        subtitle_key = DESKTOP_TEXT_TOUCH_SUBTITLE;
    } else if (id == APP_SYSTEM) {
        rows = s_system_rows;
        subtitle_key = DESKTOP_TEXT_SYSTEM_SUBTITLE;
    } else if (id == APP_FILES) {
        rows = s_files_rows;
        subtitle_key = DESKTOP_TEXT_FILES_SUBTITLE;
    } else if (id == APP_ABOUT) {
        rows = s_about_rows;
        subtitle_key = DESKTOP_TEXT_ABOUT_SUBTITLE;
    } else if (id == APP_REFRESH) {
        show_toast(L(DESKTOP_TEXT_REFRESH_DONE));
        rows = s_refresh_rows;
        count = sizeof(s_refresh_rows) / sizeof(s_refresh_rows[0]);
        subtitle_key = DESKTOP_TEXT_REFRESH_SUBTITLE;
    }
    prepare_modal(app_by_id(id), L(subtitle_key));
    populate_localized_rows(rows, count);
}

static void build_wifi(void)
{
    app_wifi_view_build(s_full_body, show_toast);
}

static void build_ethernet(void)
{
    app_ethernet_view_build(s_full_body, show_toast);
}

static void calc_apply(void)
{
    double current = strtod(s_calc_text, NULL);
    if (s_calc_operator == '+') {
        s_calc_accumulator += current;
    } else if (s_calc_operator == '-') {
        s_calc_accumulator -= current;
    } else if (s_calc_operator == '*') {
        s_calc_accumulator *= current;
    } else if (s_calc_operator == '/' && current != 0.0) {
        s_calc_accumulator /= current;
    } else {
        s_calc_accumulator = current;
    }
}

/**
 * @brief Append a calculation and discard the oldest row when history is full.
 * @param expression Calculation text to append.
 */
static void calc_history_push(const char *expression)
{
    if (s_calc_history == NULL) {
        return;
    }
    lv_obj_t *bubble = lv_obj_create(s_calc_history);
    style_plain(bubble, C_ACCENT_SOFT, 12);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(bubble, 12, 0);
    lv_obj_set_style_pad_ver(bubble, 6, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_label(bubble, expression, C_INK, 0, 0);
    while (lv_obj_get_child_count(s_calc_history) > CALC_HISTORY_MAX) {
        lv_obj_delete(lv_obj_get_child(s_calc_history, 0));
    }
    lv_obj_scroll_to_y(s_calc_history, LV_COORD_MAX, LV_ANIM_OFF);
}

static void calc_key_cb(lv_event_t *event)
{
    const char *key = lv_event_get_user_data(event);
    size_t length = strlen(s_calc_text);

    if (strcmp(key, "AC") == 0) {
        strcpy(s_calc_text, "0");
        s_calc_accumulator = 0;
        s_calc_operator = 0;
        s_calc_new_value = true;
    } else if (strcmp(key, "%") == 0) {
        double percent = strtod(s_calc_text, NULL) / 100.0;
        snprintf(s_calc_text, sizeof(s_calc_text), "%.8g", percent);
        s_calc_new_value = true;
    } else if (strchr("+-*/", key[0]) != NULL && key[1] == '\0') {
        /** Keep the intermediate value on the entry line until equals is pressed. */
        calc_apply();
        s_calc_operator = key[0];
        s_calc_new_value = true;
    } else if (strcmp(key, "=") == 0) {
        char expression[CALC_BUBBLE_TEXT_MAX];
        const char operator_char = s_calc_operator;
        const double left = s_calc_accumulator;
        const double right = strtod(s_calc_text, NULL);
        calc_apply();
        snprintf(s_calc_text, sizeof(s_calc_text), "%.8g", s_calc_accumulator);
        s_calc_operator = 0;
        s_calc_new_value = true;
        if (operator_char != 0) {
            snprintf(expression, sizeof(expression), "%.8g %c %.8g = %s", left, operator_char,
                     right, s_calc_text);
            calc_history_push(expression);
        }
    } else if (strcmp(key, "<") == 0) {
        if (length > 1) {
            s_calc_text[length - 1] = '\0';
        } else {
            strcpy(s_calc_text, "0");
        }
    } else if ((key[0] >= '0' && key[0] <= '9') || key[0] == '.') {
        if (s_calc_new_value) {
            strcpy(s_calc_text, key[0] == '.' ? "0." : key);
            s_calc_new_value = false;
        } else if (length < sizeof(s_calc_text) - 2 &&
                   !(key[0] == '.' && strchr(s_calc_text, '.') != NULL)) {
            strcat(s_calc_text, key);
        }
    }
    lv_label_set_text(s_calc_display, s_calc_text);
}

static bool build_standard_layout(app_id_t id, desktop_fullscreen_layout_t *layout)
{
    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = C_CANVAS,
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };
    if (app_by_id(id) == NULL ||
        !desktop_fullscreen_layout_create(s_full_body, &colors, "", "", "", layout)) {
        return false;
    }

    desktop_fullscreen_layout_remove_page_header(layout);
    return true;
}

static lv_obj_t *flex_button(lv_obj_t *parent, const char *text, int32_t width_percent,
                             int32_t height, lv_color_t color, lv_event_cb_t callback,
                             void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    style_plain(button, color, 12);
    lv_obj_set_width(button, LV_PCT(width_percent));
    lv_obj_set_height(button, height);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    if (callback != NULL) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *caption = lv_label_create(button);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_color(caption, C_INK, 0);
    lv_obj_center(caption);
    return button;
}

static lv_obj_t *flex_info_card(lv_obj_t *parent, const char *title, const char *value,
                                int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_plain(card, C_SURFACE, 14);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, height);
    lv_obj_t *title_label = make_label(card, title, C_INK_MUTED, 0, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 16, 12);
    lv_obj_t *value_label = make_label(card, value, C_INK, 0, 0);
    lv_obj_align_to(value_label, title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    return card;
}

static void build_calculator(void)
{
    static const char *const keys[] = {
        "AC", "%", "/", "<", "7", "8", "9", "*", "4", "5", "6", "-",
        "1", "2", "3", "+", "0", ".", "=",
    };
    desktop_fullscreen_layout_t layout;
    if (!build_standard_layout(APP_CALCULATOR, &layout)) {
        return;
    }

    /** Reset entry and history when opening the calculator. */
    strcpy(s_calc_text, "0");
    s_calc_accumulator = 0;
    s_calc_operator = 0;
    s_calc_new_value = true;
    s_calc_history = NULL;

    /** Place history and entry above the keypad. */
    lv_obj_set_flex_flow(layout.content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *display_panel = desktop_fullscreen_layout_create_panel(
        layout.content, C_SURFACE, 100, 16, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_height(display_panel, LV_PCT(38));
    lv_obj_set_style_pad_all(display_panel, 16, 0);
    lv_obj_set_style_pad_row(display_panel, 8, 0);

    s_calc_history = lv_obj_create(display_panel);
    style_plain(s_calc_history, C_SURFACE, 0);
    lv_obj_set_width(s_calc_history, LV_PCT(100));
    lv_obj_set_flex_grow(s_calc_history, 1);
    lv_obj_set_flex_flow(s_calc_history, LV_FLEX_FLOW_COLUMN);
    /** Arrange calculation history from oldest to newest, aligned right. */
    lv_obj_set_flex_align(s_calc_history, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(s_calc_history, 6, 0);
    lv_obj_add_flag(s_calc_history, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_calc_history, LV_DIR_VER);

    s_calc_display = make_label(display_panel, s_calc_text, C_INK, 0, 0);
    lv_obj_set_width(s_calc_display, LV_PCT(100));
    lv_obj_set_style_text_color(s_calc_display, C_INK, 0);
    lv_obj_set_style_text_align(s_calc_display, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_calc_display, desktop_font_get_title(), 0);

    lv_obj_t *keypad = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 100, 0, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_height(keypad, LV_PCT(58));
    lv_obj_set_style_pad_row(keypad, 6, 0);
    lv_obj_set_style_pad_column(keypad, 8, 0);
    lv_obj_set_flex_align(keypad, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    /** Build an ASCII keypad with four columns and a double-width zero key. */
    for (int i = 0; i < 19; ++i) {
        const int32_t width = i == 16 ? 46 : 22;
        const lv_color_t bg = strchr("+-*/=%", keys[i][0]) != NULL || strcmp(keys[i], "AC") == 0
                                  ? C_ACCENT_SOFT
                                  : C_MUTED;
        lv_obj_t *key = flex_button(keypad, keys[i], width, 58, bg, calc_key_cb,
                                    (void *)keys[i]);

        lv_obj_set_height(key, LV_PCT(18));
    }
}

static void gpio_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    gpio_pin_id_t pin_id = (gpio_pin_id_t)(uintptr_t)lv_event_get_user_data(event);
    const char *pin = pin_id == GPIO_PIN_7 ? "GPIO 7" : pin_id == GPIO_PIN_8 ? "GPIO 8" : "GPIO 4";
    char message[48];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_GPIO_OUTPUT_FEEDBACK), pin,
             lv_obj_has_state(sw, LV_STATE_CHECKED) ? L(DESKTOP_TEXT_SETTINGS_ENABLED) : L(DESKTOP_TEXT_CLOSE));
    show_toast(message);
}

static void gpio_row(lv_obj_t *parent, gpio_pin_id_t pin_id, const char *pin,
                     const char *description, bool checked)
{
    lv_obj_t *row = lv_obj_create(parent);
    style_plain(row, C_SURFACE, 14);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 72);
    lv_obj_t *pin_label = make_label(row, pin, C_INK, 0, 0);
    lv_obj_align(pin_label, LV_ALIGN_LEFT_MID, 16, -12);
    lv_obj_t *description_label = make_label(row, description, C_INK_MUTED, 0, 0);
    lv_obj_align_to(description_label, pin_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 48, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, gpio_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)pin_id);
}

static void build_gpio(void)
{
    desktop_fullscreen_layout_t layout;
    if (!build_standard_layout(APP_GPIO, &layout)) {
        return;
    }
    lv_obj_t *summary = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 38, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(summary, 8, 0);
    flex_info_card(summary, L(DESKTOP_TEXT_GPIO_INPUT), "GPIO 4 / 5", 84);
    flex_info_card(summary, L(DESKTOP_TEXT_GPIO_OUTPUT), "GPIO 7 / 8", 84);
    flex_info_card(summary, L(DESKTOP_TEXT_GPIO_PWM), L(DESKTOP_TEXT_GPIO_CHANNEL_COUNT), 84);
    flex_info_card(summary, L(DESKTOP_TEXT_GPIO_ADC), "12 bit", 84);

    lv_obj_t *controls = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 61, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(controls, 10, 0);
    gpio_row(controls, GPIO_PIN_4, "GPIO 4", L(DESKTOP_TEXT_GPIO_4), true);
    gpio_row(controls, GPIO_PIN_7, "GPIO 7", L(DESKTOP_TEXT_GPIO_7), true);
    gpio_row(controls, GPIO_PIN_8, "GPIO 8", L(DESKTOP_TEXT_GPIO_8), false);
}

static void diagnostics_run_cb(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_user_data(event);
    lv_label_set_text(label, L(DESKTOP_TEXT_DIAGNOSTICS_RECENT));
    show_toast(L(DESKTOP_TEXT_DIAGNOSTICS_DONE));
}

static void build_diagnostics(void)
{
    desktop_fullscreen_layout_t layout;
    if (!build_standard_layout(APP_DIAGNOSTICS, &layout)) {
        return;
    }
    lv_obj_t *summary_panel = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 38, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(summary_panel, 12, 0);
    lv_obj_t *summary = flex_info_card(summary_panel, L(DESKTOP_TEXT_DIAGNOSTICS_RECENT_TODAY),
                                       L(DESKTOP_TEXT_NOTIFICATION_ALL_NORMAL), 112);
    lv_obj_t *summary_label = lv_obj_get_child(summary, 1);
    flex_button(summary_panel, L(DESKTOP_TEXT_APP_DIAGNOSTICS), 100, 54, C_ACCENT_SOFT,
                diagnostics_run_cb, summary_label);

    lv_obj_t *cards = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 61, 0, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(cards, 10, 0);
    lv_obj_set_style_pad_column(cards, 10, 0);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    const struct { const char *title; const char *value; } entries[] = {
        {L(DESKTOP_TEXT_STATUS_PANEL), L(DESKTOP_TEXT_DIAGNOSTICS_DISPLAY_VALUE)},
        {L(DESKTOP_TEXT_DIAGNOSTICS_TOUCH), L(DESKTOP_TEXT_DIAGNOSTICS_TOUCH_VALUE)},
        {L(DESKTOP_TEXT_DIAGNOSTICS_NETWORK), L(DESKTOP_TEXT_DIAGNOSTICS_NETWORK_VALUE)},
        {L(DESKTOP_TEXT_FILES_AVAILABLE), L(DESKTOP_TEXT_DIAGNOSTICS_STORAGE_VALUE)},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        lv_obj_t *card = lv_obj_create(cards);
        style_plain(card, C_SURFACE, 14);
        lv_obj_set_size(card, LV_PCT(49), 112);
        lv_obj_t *title = make_label(card, entries[i].title, C_INK_MUTED, 0, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);
        lv_obj_t *value = make_label(card, entries[i].value, C_INK, 0, 0);
        lv_obj_align_to(value, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
    }
}

typedef struct {
    usb_storage_backend_event_cb_t callback;
    void *callback_arg;
    uint32_t generation;
} usb_ui_backend_call_t;

static void usb_ui_emit_backend_event(const usb_ui_backend_call_t *call,
                                      usb_storage_role_t role,
                                      usb_storage_backend_event_kind_t kind,
                                      usb_storage_operation_t operation,
                                      int result)
{
    if (call == NULL || call->callback == NULL) {
        return;
    }
    const usb_storage_backend_event_t event = {
        .kind = kind,
        .role = role,
        .operation = operation,
        .generation = call->generation,
        .result = result,
    };
    ESP_LOGI(USB_UI_TAG, "backend-event role=%s generation=%u kind=%d operation=%d result=%d",
             usb_ui_role_name(role), (unsigned int)call->generation, (int)kind, (int)operation, result);
    call->callback(&event, call->callback_arg);
}

static void usb_ui_device_event_cb(const usb_msc_device_event_t *event, void *arg)
{
    usb_ui_device_backend_t *backend = (usb_ui_device_backend_t *)arg;
    if (event != NULL) {
        ESP_LOGI(USB_UI_TAG, "device-event generation=%u kind=%d lifecycle=%d attached=%d error=%d",
                 (unsigned int)event->generation, (int)event->kind, (int)event->lifecycle,
                 event->attached, (int)event->error);
    }
    if (backend == NULL || event == NULL || backend->callback == NULL) {
        ESP_LOGW(USB_UI_TAG, "device-event dropped callback-present=%d",
                 backend != NULL && backend->callback != NULL);
        return;
    }
    const usb_ui_backend_call_t call = {
        .callback = backend->callback,
        .callback_arg = backend->callback_arg,
        .generation = backend->generation,
    };
    if (event->kind == USB_MSC_DEVICE_EVENT_ERROR ||
        event->kind == USB_MSC_DEVICE_EVENT_MOUNT_FAILED) {
        usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_DEVICE,
                                  USB_STORAGE_BACKEND_EVENT_FAILED,
                                  USB_STORAGE_OPERATION_NONE, (int)event->error);
    }
}

static int usb_ui_device_start(void *context, usb_storage_role_t role, uint32_t generation,
                               usb_storage_backend_event_cb_t callback, void *callback_arg)
{
    (void)role;
    usb_ui_device_backend_t *backend = (usb_ui_device_backend_t *)context;
    if (backend == NULL || backend->handle == NULL) {
        return -1;
    }
    backend->callback = callback;
    backend->callback_arg = callback_arg;
    backend->generation = generation;
    ESP_LOGI(USB_UI_TAG, "device-start role=device generation=%u", (unsigned int)generation);
    const esp_err_t result = usb_msc_device_start(backend->handle);
    ESP_LOGI(USB_UI_TAG, "device-start-result role=device generation=%u result=%s",
             (unsigned int)generation, esp_err_to_name(result));
    if (result != ESP_OK) {
        return (int)result;
    }
    const usb_ui_backend_call_t call = {callback, callback_arg, generation};
    usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_DEVICE,
                              USB_STORAGE_BACKEND_EVENT_STARTED,
                              USB_STORAGE_OPERATION_NONE, 0);
    return 0;
}

static int usb_ui_device_stop(void *context, usb_storage_role_t role, uint32_t generation)
{
    (void)role;
    usb_ui_device_backend_t *backend = (usb_ui_device_backend_t *)context;
    if (backend == NULL || backend->handle == NULL) {
        return -1;
    }
    ESP_LOGI(USB_UI_TAG, "device-stop role=device generation=%u", (unsigned int)generation);
    const esp_err_t result = usb_msc_device_stop(backend->handle);
    ESP_LOGI(USB_UI_TAG, "device-stop-result role=device generation=%u result=%s",
             (unsigned int)generation, esp_err_to_name(result));
    if (result == ESP_OK) {
        const usb_ui_backend_call_t call = {backend->callback, backend->callback_arg, generation};
        usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_DEVICE,
                                  USB_STORAGE_BACKEND_EVENT_STOP_COMPLETE,
                                  USB_STORAGE_OPERATION_NONE, 0);
    }
    return (int)result;
}

static int usb_ui_host_start(void *context, usb_storage_role_t role, uint32_t generation,
                             usb_storage_backend_event_cb_t callback, void *callback_arg)
{
    (void)role;
    usb_ui_host_backend_t *backend = (usb_ui_host_backend_t *)context;
    if (backend == NULL || !backend->initialized) {
        return -1;
    }
    backend->callback = callback;
    backend->callback_arg = callback_arg;
    backend->generation = generation;
    backend->media_present = false;
    backend->mounted = false;
    ESP_LOGI(USB_UI_TAG, "host-start role=host generation=%u", (unsigned int)generation);
    const esp_err_t result = usb_msc_host_start();
    ESP_LOGI(USB_UI_TAG, "host-start-result role=host generation=%u result=%s",
             (unsigned int)generation, esp_err_to_name(result));
    if (result != ESP_OK) {
        return (int)result;
    }
    const usb_ui_backend_call_t call = {callback, callback_arg, generation};
    usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_HOST,
                              USB_STORAGE_BACKEND_EVENT_STARTED,
                              USB_STORAGE_OPERATION_NONE, 0);
    return 0;
}

static int usb_ui_host_stop(void *context, usb_storage_role_t role, uint32_t generation)
{
    (void)role;
    usb_ui_host_backend_t *backend = (usb_ui_host_backend_t *)context;
    if (backend == NULL || !backend->initialized) {
        return -1;
    }
    ESP_LOGI(USB_UI_TAG, "host-stop role=host generation=%u", (unsigned int)generation);
    const esp_err_t result = usb_msc_host_stop();
    ESP_LOGI(USB_UI_TAG, "host-stop-result role=host generation=%u result=%s",
             (unsigned int)generation, esp_err_to_name(result));
    if (result == ESP_OK) {
        backend->media_present = false;
        backend->mounted = false;
        const usb_ui_backend_call_t call = {backend->callback, backend->callback_arg, generation};
        usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_HOST,
                                  USB_STORAGE_BACKEND_EVENT_STOP_COMPLETE,
                                  USB_STORAGE_OPERATION_NONE, 0);
    }
    return (int)result;
}

static int usb_ui_host_probe(void *context, uint32_t generation,
                             usb_storage_operation_t operation)
{
    usb_ui_host_backend_t *backend = (usb_ui_host_backend_t *)context;
    ESP_LOGI(USB_UI_TAG, "host-%s role=host generation=%u",
             operation == USB_STORAGE_OPERATION_READ ? "read" : "write",
             (unsigned int)generation);
    if (backend == NULL || !backend->initialized) {
        return -1;
    }
    const esp_err_t result = operation == USB_STORAGE_OPERATION_READ
                                 ? usb_msc_host_read_capacity()
                                 : usb_msc_host_write_hello_world();
    ESP_LOGI(USB_UI_TAG, "host-%s-result role=host generation=%u result=%s",
             operation == USB_STORAGE_OPERATION_READ ? "read" : "write",
             (unsigned int)generation, esp_err_to_name(result));
    const usb_ui_backend_call_t call = {
        .callback = backend->callback,
        .callback_arg = backend->callback_arg,
        .generation = generation,
    };
    usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_HOST,
                              USB_STORAGE_BACKEND_EVENT_OPERATION_COMPLETE,
                              operation, (int)result);
    return (int)result;
}

static int usb_ui_host_read(void *context, uint32_t generation)
{
    return usb_ui_host_probe(context, generation, USB_STORAGE_OPERATION_READ);
}

static int usb_ui_host_write(void *context, uint32_t generation)
{
    return usb_ui_host_probe(context, generation, USB_STORAGE_OPERATION_WRITE);
}

static void usb_ui_controller_notify(const usb_storage_controller_notification_t *notification,
                                     void *arg)
{
    (void)arg;
    if (notification == NULL) {
        ESP_LOGW(USB_UI_TAG, "controller-notify dropped null notification");
        return;
    }
    ESP_LOGI(USB_UI_TAG, "controller-notify role=%s generation=%u kind=%d lifecycle=%d result=%d",
             usb_ui_role_name(notification->role), (unsigned int)notification->generation,
             (int)notification->kind, (int)notification->state.lifecycle, notification->result);
    (void)pthread_mutex_lock(&s_usb_notification_mutex);
    s_usb_notification = *notification;
    s_usb_notification_valid = true;
    (void)pthread_mutex_unlock(&s_usb_notification_mutex);
}

static bool usb_ui_backend_init(void)
{
    if (!s_usb_device_backend.initialized) {
        usb_msc_device_config_t config = USB_MSC_DEVICE_CONFIG_DEFAULT();
        config.event_callback = usb_ui_device_event_cb;
        config.callback_arg = &s_usb_device_backend;
        const esp_err_t result = usb_msc_device_init(&config, &s_usb_device_backend.handle);
        ESP_LOGI(USB_UI_TAG, "backend-init role=device result=%s", esp_err_to_name(result));
        if (result != ESP_OK) {
            return false;
        }
        s_usb_device_backend.initialized = true;
    }
    if (!s_usb_host_backend.initialized) {
        const esp_err_t result = usb_msc_host_init();
        ESP_LOGI(USB_UI_TAG, "backend-init role=host result=%s", esp_err_to_name(result));
        if (result != ESP_OK) {
            (void)usb_msc_device_deinit(s_usb_device_backend.handle);
            memset(&s_usb_device_backend, 0, sizeof(s_usb_device_backend));
            return false;
        }
        s_usb_host_backend.initialized = true;
    }
    return true;
}

static void usb_ui_backend_deinit(void)
{
    if (s_usb_host_backend.initialized) {
        (void)usb_msc_host_deinit();
        memset(&s_usb_host_backend, 0, sizeof(s_usb_host_backend));
    }
    if (s_usb_device_backend.initialized) {
        (void)usb_msc_device_deinit(s_usb_device_backend.handle);
        memset(&s_usb_device_backend, 0, sizeof(s_usb_device_backend));
    }
}

static bool usb_ui_controller_create(void)
{
    if (!usb_ui_backend_init()) {
        ESP_LOGE(USB_UI_TAG, "backend-init result=failed");
        return false;
    }
    ESP_LOGI(USB_UI_TAG, "backend-init result=ok");
    const usb_storage_controller_config_t config = {
        .device = {
            .context = &s_usb_device_backend,
            .start = usb_ui_device_start,
            .stop = usb_ui_device_stop,
            .read = NULL,
            .write = NULL,
        },
        .host = {
            .context = &s_usb_host_backend,
            .start = usb_ui_host_start,
            .stop = usb_ui_host_stop,
            .read = usb_ui_host_read,
            .write = usb_ui_host_write,
        },
        .notify = usb_ui_controller_notify,
        .notify_arg = NULL,
        .queue_capacity = 32U,
    };
    const usb_storage_controller_result_t result =
        usb_storage_controller_create(&config, &s_usb_controller);
    ESP_LOGI(USB_UI_TAG, "controller-init result=%s", usb_storage_controller_result_name(result));
    if (result != USB_STORAGE_CONTROLLER_OK) {
        usb_ui_backend_deinit();
        return false;
    }
    return true;
}

static usb_stick_ui_state_t usb_ui_state(usb_storage_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case USB_STORAGE_LIFECYCLE_STARTING:
        return USB_STICK_UI_STATE_STARTING;
    case USB_STORAGE_LIFECYCLE_ACTIVE:
    case USB_STORAGE_LIFECYCLE_BUSY:
        return USB_STICK_UI_STATE_ACTIVE;
    case USB_STORAGE_LIFECYCLE_STOPPING:
        return USB_STICK_UI_STATE_STOPPING;
    case USB_STORAGE_LIFECYCLE_ERROR:
        return USB_STICK_UI_STATE_FAILED;
    case USB_STORAGE_LIFECYCLE_IDLE:
    default:
        return USB_STICK_UI_STATE_IDLE;
    }
}

static usb_stick_ui_role_t usb_ui_role(usb_storage_role_t role)
{
    if (role == USB_STORAGE_ROLE_DEVICE) return USB_STICK_UI_ROLE_DEVICE;
    if (role == USB_STORAGE_ROLE_HOST) return USB_STICK_UI_ROLE_HOST;
    return USB_STICK_UI_ROLE_NONE;
}

static void usb_ui_render_notification(const usb_storage_controller_notification_t *notification)
{
    if (s_usb_page == NULL || notification == NULL) {
        return;
    }
    usb_storage_block_reason_t reason = USB_STORAGE_BLOCK_NONE;
    const bool can_close = usb_storage_can_close(&notification->state, &reason);
    if (notification->kind == USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_BLOCKED) {
        reason = notification->block_reason;
    }
    usb_stick_ui_role_t role = usb_ui_role(notification->role);
    if (role == USB_STICK_UI_ROLE_NONE) {
        role = usb_ui_role(s_usb_selected_role);
    }
    char storage_info[96] = {0};
    if (role == USB_STICK_UI_ROLE_HOST && notification->mounted) {
        usb_msc_host_snapshot_t host_snapshot;
        if (usb_msc_host_get_snapshot(&host_snapshot) == ESP_OK && host_snapshot.capacity_valid) {
            (void)snprintf(storage_info, sizeof(storage_info),
                           "Total: %" PRIu64 " MiB  Free: %" PRIu64 " MiB",
                           host_snapshot.total_bytes / (1024U * 1024U),
                           host_snapshot.free_bytes / (1024U * 1024U));
        }
    }
    const usb_stick_ui_view_model_t model = {
        .generation = notification->generation,
        .role = role,
        .state = usb_ui_state(notification->state.lifecycle),
        .status = notification->state.last_error == USB_STORAGE_ERROR_NONE
                      ? (reason == USB_STORAGE_BLOCK_NONE ? "READY"
                                                           : usb_storage_block_reason_name(reason))
                      : usb_storage_error_name(notification->state.last_error),
        .storage_info = storage_info[0] == '\0' ? NULL : storage_info,
        .error = notification->state.last_error == USB_STORAGE_ERROR_NONE
                     ? NULL
                     : usb_storage_error_name(notification->state.last_error),
        .blocking_reason = reason == USB_STORAGE_BLOCK_NONE
                               ? NULL
                               : usb_storage_block_reason_name(reason),
        .media_mounted = notification->mounted,
        .operation_busy = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_BUSY ||
                          notification->operation != USB_STORAGE_OPERATION_NONE,
        .permissions = {
            .select_device = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_IDLE,
            .select_host = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_IDLE,
            .start = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_IDLE &&
                     s_usb_selected_role != USB_STORAGE_ROLE_NONE,
            .read = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE &&
                    s_usb_selected_role == USB_STORAGE_ROLE_HOST && notification->mounted,
            .write = notification->state.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE &&
                     s_usb_selected_role == USB_STORAGE_ROLE_HOST && notification->mounted,
            .stop = notification->state.lifecycle != USB_STORAGE_LIFECYCLE_IDLE,
            .close = can_close,
        },
    };
    (void)usb_stick_app_render(s_usb_page, &model);
}

static bool usb_ui_read_notification(usb_storage_controller_notification_t *notification)
{
    bool valid;
    (void)pthread_mutex_lock(&s_usb_notification_mutex);
    valid = s_usb_notification_valid;
    if (valid && notification != NULL) {
        *notification = s_usb_notification;
    }
    (void)pthread_mutex_unlock(&s_usb_notification_mutex);
    return valid;
}

static void usb_ui_render_current(void)
{
    usb_storage_controller_notification_t notification;
    if (usb_ui_read_notification(&notification)) {
        usb_ui_render_notification(&notification);
        return;
    }
    if (s_usb_controller != NULL) {
        usb_storage_controller_snapshot_t snapshot;
        if (usb_storage_controller_get_snapshot(s_usb_controller, &snapshot) !=
            USB_STORAGE_CONTROLLER_OK) {
            return;
        }
        notification = (usb_storage_controller_notification_t) {
            .kind = USB_STORAGE_CONTROLLER_NOTIFICATION_STATE,
            .state = snapshot.state,
            .role = snapshot.active_role,
            .operation = snapshot.operation,
            .generation = snapshot.generation,
            .media_present = snapshot.media_present,
            .mounted = snapshot.mounted,
            .backend_active = snapshot.backend_active,
            .stop_requested = snapshot.stop_requested,
            .close_requested = snapshot.close_requested,
        };
        usb_ui_render_notification(&notification);
    }
}

static void usb_ui_poll_host_media(void)
{
    if (s_usb_controller == NULL || !s_usb_host_backend.initialized) {
        return;
    }
    usb_storage_controller_snapshot_t controller_snapshot;
    if (usb_storage_controller_get_snapshot(s_usb_controller, &controller_snapshot) !=
        USB_STORAGE_CONTROLLER_OK || controller_snapshot.active_role != USB_STORAGE_ROLE_HOST ||
        controller_snapshot.generation == 0U) {
        return;
    }
    usb_msc_host_snapshot_t host_snapshot;
    if (usb_msc_host_get_snapshot(&host_snapshot) != ESP_OK) {
        return;
    }
    const usb_ui_backend_call_t call = {
        .callback = s_usb_host_backend.callback,
        .callback_arg = s_usb_host_backend.callback_arg,
        .generation = controller_snapshot.generation,
    };
    if (host_snapshot.media_present && !s_usb_host_backend.media_present) {
        usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_HOST,
                                  USB_STORAGE_BACKEND_EVENT_MEDIA_ATTACHED,
                                  USB_STORAGE_OPERATION_NONE, 0);
    }
    if (host_snapshot.mounted && !s_usb_host_backend.mounted) {
        usb_ui_emit_backend_event(&call, USB_STORAGE_ROLE_HOST,
                                  USB_STORAGE_BACKEND_EVENT_MEDIA_MOUNTED,
                                  USB_STORAGE_OPERATION_MOUNT, 0);
    }
    s_usb_host_backend.media_present = host_snapshot.media_present;
    s_usb_host_backend.mounted = host_snapshot.mounted;
}

static bool usb_ui_finish_close(void)
{
    if (!s_usb_app_open) {
        return true;
    }
    if (s_usb_controller != NULL) {
        const usb_storage_controller_result_t result =
            usb_storage_controller_destroy(s_usb_controller);
        if (result != USB_STORAGE_CONTROLLER_OK) {
            usb_ui_render_current();
            return false;
        }
        s_usb_controller = NULL;
    }
    usb_ui_restore_msc_resources();
    if (s_usb_poll_timer != NULL) {
        lv_timer_delete(s_usb_poll_timer);
        s_usb_poll_timer = NULL;
    }
    if (s_usb_page != NULL) {
        usb_stick_app_destroy(s_usb_page);
        s_usb_page = NULL;
    }
    usb_ui_backend_deinit();
    (void)pthread_mutex_lock(&s_usb_notification_mutex);
    memset(&s_usb_notification, 0, sizeof(s_usb_notification));
    s_usb_notification_valid = false;
    (void)pthread_mutex_unlock(&s_usb_notification_mutex);
    s_usb_close_requested = false;
    s_usb_app_open = false;
    s_full_exit_tracking = false;
    s_full_exit_requested = false;
    app_touch_gesture_end(&s_full_exit_gesture);
    if (s_full_exit_strip != NULL) {
        lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_full, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static bool usb_ui_cleanup_if_safe(void)
{
    if (!s_usb_app_open) {
        return true;
    }
    const usb_storage_controller_result_t result =
        usb_storage_controller_request_close(s_usb_controller);
    if (result != USB_STORAGE_CONTROLLER_OK) {
        s_usb_close_requested = result == USB_STORAGE_CONTROLLER_BUSY;
        usb_ui_render_current();
        return false;
    }
    return usb_ui_finish_close();
}

static void usb_ui_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_usb_app_open) {
        return;
    }
    usb_ui_poll_host_media();
    usb_storage_controller_notification_t notification;
    if (usb_ui_read_notification(&notification) &&
        notification.kind == USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_READY &&
        s_usb_close_requested) {
        (void)usb_ui_finish_close();
        return;
    }
    usb_ui_restore_msc_resources_if_idle();
    usb_ui_render_current();
}

static void usb_ui_intent_cb(const usb_stick_ui_intent_t *intent, void *user_data)
{
    (void)user_data;
    if (intent == NULL || s_usb_controller == NULL) {
        return;
    }
    usb_storage_controller_snapshot_t snapshot;
    if (usb_storage_controller_get_snapshot(s_usb_controller, &snapshot) !=
        USB_STORAGE_CONTROLLER_OK) {
        ESP_LOGW(USB_UI_TAG, "intent dropped generation=%u reason=snapshot-unavailable",
                 (unsigned int)intent->generation);
        return;
    }
    if (intent->generation != 0U && intent->generation != snapshot.generation) {
        ESP_LOGW(USB_UI_TAG, "intent dropped generation=%u current-generation=%u reason=stale",
                 (unsigned int)intent->generation, (unsigned int)snapshot.generation);
        return;
    }
    ESP_LOGI(USB_UI_TAG, "intent=%s role=%s generation=%u", usb_ui_intent_name(intent->kind),
             usb_ui_role_name(s_usb_selected_role), (unsigned int)intent->generation);
    usb_storage_controller_result_t result = USB_STORAGE_CONTROLLER_OK;
    switch (intent->kind) {
    case USB_STICK_UI_INTENT_SELECT_DEVICE:
    case USB_STICK_UI_INTENT_SELECT_HOST:
        s_usb_selected_role = intent->role == USB_STICK_UI_ROLE_DEVICE
                                  ? USB_STORAGE_ROLE_DEVICE
                                  : USB_STORAGE_ROLE_HOST;
        ESP_LOGI(USB_UI_TAG, "role-selected role=%s generation=%u; selection only, START activates backend",
                 usb_ui_role_name(s_usb_selected_role), (unsigned int)intent->generation);
        usb_ui_render_current();
        return;
    case USB_STICK_UI_INTENT_START:
        ESP_LOGI(USB_UI_TAG, "start-request role=%s generation=%u; START activates backend",
                 usb_ui_role_name(s_usb_selected_role), (unsigned int)intent->generation);
        if (s_usb_selected_role == USB_STORAGE_ROLE_DEVICE ||
            s_usb_selected_role == USB_STORAGE_ROLE_HOST) {
            if (!usb_ui_prepare_msc_start()) {
                usb_ui_restore_msc_resources();
                show_toast(L(DESKTOP_TEXT_USB_SERVICE_UNAVAILABLE));
                return;
            }
        }
        result = usb_storage_controller_start(s_usb_controller, s_usb_selected_role);
        if (s_usb_selected_role == USB_STORAGE_ROLE_DEVICE ||
            s_usb_selected_role == USB_STORAGE_ROLE_HOST) {
            if (result == USB_STORAGE_CONTROLLER_OK || result == USB_STORAGE_CONTROLLER_BUSY) {
                s_usb_msc_session_started = true;
            } else {
                usb_ui_restore_msc_resources();
            }
        }
        break;
    case USB_STICK_UI_INTENT_READ:
        result = usb_storage_controller_read(s_usb_controller);
        break;
    case USB_STICK_UI_INTENT_WRITE:
        result = usb_storage_controller_write(s_usb_controller);
        break;
    case USB_STICK_UI_INTENT_STOP:
        result = usb_storage_controller_stop(s_usb_controller);
        break;
    case USB_STICK_UI_INTENT_CLOSE:
        result = usb_storage_controller_request_close(s_usb_controller);
        ESP_LOGI(USB_UI_TAG, "intent-result intent=close role=%s generation=%u result=%s",
                 usb_ui_role_name(s_usb_selected_role), (unsigned int)intent->generation,
                 usb_storage_controller_result_name(result));
        if (result == USB_STORAGE_CONTROLLER_OK) {
            (void)usb_ui_finish_close();
            return;
        }
        s_usb_close_requested = result == USB_STORAGE_CONTROLLER_BUSY;
        usb_ui_render_current();
        return;
    default:
        return;
    }
    ESP_LOGI(USB_UI_TAG, "intent-result intent=%s role=%s generation=%u result=%s",
             usb_ui_intent_name(intent->kind), usb_ui_role_name(s_usb_selected_role),
             (unsigned int)intent->generation, usb_storage_controller_result_name(result));
    if (result != USB_STORAGE_CONTROLLER_OK && result != USB_STORAGE_CONTROLLER_BUSY) {
        show_toast(L(DESKTOP_TEXT_USB_SERVICE_UNAVAILABLE));
    }
}

static void build_usbstick(void)
{
    ESP_LOGI(USB_UI_TAG, "page-open build role-default=host");
    s_usb_close_requested = false;
    s_usb_msc_session_started = false;
    s_usb_selected_role = USB_STORAGE_ROLE_HOST;
    if (!usb_ui_controller_create()) {
        show_toast(L(DESKTOP_TEXT_USB_SERVICE_UNAVAILABLE));
        return;
    }
    s_usb_page = usb_stick_app_create(s_full_body, usb_ui_intent_cb, NULL);
    if (s_usb_page == NULL) {
        (void)usb_storage_controller_destroy(s_usb_controller);
        s_usb_controller = NULL;
        usb_ui_backend_deinit();
        show_toast(L(DESKTOP_TEXT_USB_SERVICE_UNAVAILABLE));
        return;
    }
    s_usb_app_open = true;
    (void)pthread_mutex_lock(&s_usb_notification_mutex);
    memset(&s_usb_notification, 0, sizeof(s_usb_notification));
    s_usb_notification_valid = false;
    (void)pthread_mutex_unlock(&s_usb_notification_mutex);
    usb_ui_render_current();
    s_usb_poll_timer = lv_timer_create(usb_ui_poll_cb, 100, NULL);
}

static void aichat_ui_set_uplink_enabled(bool enabled)
{
    if (s_aichat_audio != NULL) {
        aichat_audio_gmf_set_uplink_enabled(s_aichat_audio, enabled);
        if (aichat_audio_gmf_is_running(s_aichat_audio) &&
            aichat_audio_gmf_set_listening_profile(s_aichat_audio, enabled) != ESP_OK) {
            ESP_LOGW("AIChatUI", "Failed to apply AFE profile listening=%d", enabled);
        }
    }
    if (s_aichat_xiaozhi != NULL) {
        aichat_xiaozhi_set_uplink_enabled(s_aichat_xiaozhi, enabled);
    }
}

static void aichat_ui_post_event(const aichat_ui_event_t *event)
{
    if (event == NULL || s_aichat_event_queue == NULL || !s_aichat_accept_events) {
        return;
    }
    if (xQueueSend(s_aichat_event_queue, event, 0) != pdTRUE) {
        ESP_LOGW("AIChatUI", "event queue full, dropping event=%d", event->kind);
    }
}

static void aichat_audio_pcm_cb(const uint8_t *data, size_t len, void *context)
{
    static uint32_t queued_frames;
    static uint32_t ignored_frames;
    (void)context;
    if (data == NULL || len == 0U || !s_aichat_accept_events ||
        s_aichat_xiaozhi == NULL || s_aichat_app == NULL) {
        return;
    }
    if (aichat_app_state(s_aichat_app) != AICHAT_STATE_LISTENING) {
        ignored_frames++;
        if ((ignored_frames % 25U) == 0U) {
            ESP_LOGI("AIChatUI", "AFE Opus ignored while state=%s, frames=%" PRIu32,
                     aichat_state_name(aichat_app_state(s_aichat_app)), ignored_frames);
        }
        return;
    }
    const esp_err_t ret = aichat_xiaozhi_enqueue_audio(s_aichat_xiaozhi, data, len);
    if (ret == ESP_OK) {
        queued_frames++;
        if ((queued_frames % 25U) == 0U) {
            ESP_LOGI("AIChatUI", "AFE Opus queued to Xiaozhi, frames=%" PRIu32
                     ", last_bytes=%u", queued_frames, (unsigned)len);
        }
    } else if (ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW("AIChatUI", "failed to queue Opus audio: %s", esp_err_to_name(ret));
    }
}

static void aichat_audio_afe_cb(int event_type, void *event_data, void *context)
{
    (void)event_data;
    (void)context;
    ESP_LOGI("AIChatUI", "AFE event=%d", event_type);
    if (event_type == ESP_GMF_AFE_EVT_WAKEUP_START) {
        const aichat_ui_event_t event = {.kind = AICHAT_UI_EVENT_WAKEUP};
        aichat_ui_post_event(&event);
    } else if (event_type == ESP_GMF_AFE_EVT_VAD_END) {
        const aichat_ui_event_t event = {.kind = AICHAT_UI_EVENT_VAD_END};
        aichat_ui_post_event(&event);
    } else if (event_type == ESP_GMF_AFE_EVT_WAKEUP_END) {
        const aichat_ui_event_t event = {.kind = AICHAT_UI_EVENT_WAKEUP_END};
        aichat_ui_post_event(&event);
    }
}

static void aichat_xiaozhi_event_cb(aichat_xiaozhi_event_t event, const void *data,
                                    void *context)
{
    (void)context;
    aichat_ui_event_t ui_event = {0};
    switch (event) {
    case AICHAT_XIAOZHI_EVENT_CONNECTED:
        ui_event.kind = AICHAT_UI_EVENT_CONNECTED;
        break;
    case AICHAT_XIAOZHI_EVENT_DISCONNECTED:
        ui_event.kind = AICHAT_UI_EVENT_DISCONNECTED;
        break;
    case AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_OPENED:
        ui_event.kind = AICHAT_UI_EVENT_AUDIO_OPENED;
        break;
    case AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_CLOSED:
        ui_event.kind = AICHAT_UI_EVENT_AUDIO_CLOSED;
        break;
    case AICHAT_XIAOZHI_EVENT_SERVER_GOODBYE:
        ui_event.kind = AICHAT_UI_EVENT_SERVER_GOODBYE;
        break;
    case AICHAT_XIAOZHI_EVENT_CHAT_TEXT: {
        const esp_xiaozhi_chat_text_data_t *text = data;
        if (text == NULL || text->text == NULL) {
            return;
        }
        ui_event.kind = AICHAT_UI_EVENT_TEXT;
        ui_event.role = text->role;
        (void)snprintf(ui_event.text, sizeof(ui_event.text), "%s", text->text);
        break;
    }
    case AICHAT_XIAOZHI_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *tts = data;
        if (tts == NULL) {
            return;
        }
        if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            ui_event.kind = AICHAT_UI_EVENT_TTS_START;
        } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            ui_event.kind = AICHAT_UI_EVENT_TTS_STOP;
        } else {
            return;
        }
        break;
    }
    case AICHAT_XIAOZHI_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *error = data;
        ui_event.kind = AICHAT_UI_EVENT_ERROR;
        ui_event.error = error == NULL ? ESP_FAIL : error->code;
        break;
    }
    default:
        return;
    }
    aichat_ui_post_event(&ui_event);
}

static void aichat_ui_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_aichat_app == NULL || s_aichat_xiaozhi == NULL || s_aichat_event_queue == NULL) {
        return;
    }
    aichat_ui_event_t event = {0};
    for (unsigned int i = 0U; i < 8U &&
                            xQueueReceive(s_aichat_event_queue, &event, 0) == pdTRUE; ++i) {
        aichat_state_t state = aichat_app_state(s_aichat_app);
        switch (event.kind) {
        case AICHAT_UI_EVENT_CONNECTED:
            ESP_LOGI("AIChatUI", "Xiaozhi transport connected");
            aichat_ui_set_binding_notice(NULL, false);
            if (!aichat_xiaozhi_is_session_ready(s_aichat_xiaozhi)) {
                const esp_err_t ret = aichat_xiaozhi_open_audio(s_aichat_xiaozhi);
                if (ret != ESP_OK) {
                    ESP_LOGE("AIChatUI", "Xiaozhi audio channel open failed: %s",
                             esp_err_to_name(ret));
                    (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TRANSPORT_ERROR);
                }
            }
            break;
        case AICHAT_UI_EVENT_AUDIO_OPENED:
            if (state == AICHAT_STATE_CONNECTING) {
                (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_AUDIO_CHANNEL_OPENED);
            }
            break;
        case AICHAT_UI_EVENT_SERVER_GOODBYE:
            ESP_LOGI("AIChatUI", "Xiaozhi server closed the conversation");
            aichat_ui_set_uplink_enabled(false);
            if (s_aichat_playback != NULL) {
                (void)aichat_audio_playback_stop(s_aichat_playback);
            }
            if (s_aichat_audio != NULL) {
                const esp_err_t ret = aichat_audio_gmf_end_wake_session(s_aichat_audio);
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW("AIChatUI", "Failed to end local wake session: %s",
                             esp_err_to_name(ret));
                }
            }
            if (state != AICHAT_STATE_IDLE && state != AICHAT_STATE_STOPPING &&
                state != AICHAT_STATE_CLOSED) {
                (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_SERVER_GOODBYE);
            }
            break;
        case AICHAT_UI_EVENT_AUDIO_CLOSED:
        case AICHAT_UI_EVENT_DISCONNECTED:
        case AICHAT_UI_EVENT_ERROR:
            aichat_ui_set_uplink_enabled(false);
            aichat_ui_set_binding_notice(NULL, true);
            if (state != AICHAT_STATE_STOPPING && state != AICHAT_STATE_CLOSED &&
                state != AICHAT_STATE_ERROR) {
                (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TRANSPORT_ERROR);
            }
            break;
        case AICHAT_UI_EVENT_TEXT:
            (void)aichat_app_add_message(s_aichat_app,
                event.role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER
                    ? AICHAT_MESSAGE_USER : AICHAT_MESSAGE_AI, event.text);
            break;
        case AICHAT_UI_EVENT_TTS_START:
            /** Accept server speech before the delayed local end-of-speech event. */
            if (state == AICHAT_STATE_WAITING_RESPONSE ||
                state == AICHAT_STATE_LISTENING) {
                aichat_ui_set_uplink_enabled(false);
                if (aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TTS_START) !=
                    AICHAT_APP_RESULT_OK) {
                    ESP_LOGW("AIChatUI", "TTS start state transition rejected from %s",
                             aichat_state_name(state));
                }
            }
            break;
        case AICHAT_UI_EVENT_TTS_STOP:
            if (state == AICHAT_STATE_SPEAKING ||
                state == AICHAT_STATE_WAITING_RESPONSE) {
                if (aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TTS_STOP) !=
                    AICHAT_APP_RESULT_OK) {
                    break;
                }
                const esp_err_t ret =
                    aichat_xiaozhi_send_start_listening(s_aichat_xiaozhi);
                if (ret != ESP_OK) {
                    ESP_LOGW("AIChatUI", "TTS re-listen request failed: %s",
                             esp_err_to_name(ret));
                    (void)aichat_app_dispatch(s_aichat_app,
                                              AICHAT_EVENT_TRANSPORT_ERROR);
                } else {
                    aichat_ui_set_uplink_enabled(true);
                }
            }
            break;
        case AICHAT_UI_EVENT_VAD_END:
            if (state == AICHAT_STATE_LISTENING) {
                aichat_ui_set_uplink_enabled(false);
                const esp_err_t ret = aichat_xiaozhi_send_stop_listening(s_aichat_xiaozhi);
                (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_VAD_END);
                if (ret != ESP_OK) {
                    ESP_LOGW("AIChatUI", "VAD stop request failed: %s",
                             esp_err_to_name(ret));
                }
            }
            break;
        case AICHAT_UI_EVENT_WAKEUP:
            if (state == AICHAT_STATE_IDLE && aichat_xiaozhi_is_connected(s_aichat_xiaozhi)) {
                if (!aichat_xiaozhi_is_session_ready(s_aichat_xiaozhi)) {
                    const esp_err_t open_ret = aichat_xiaozhi_open_audio(s_aichat_xiaozhi);
                    if (open_ret != ESP_OK) {
                        ESP_LOGW("AIChatUI", "WakeWord audio re-arm failed: %s",
                                 esp_err_to_name(open_ret));
                        (void)aichat_app_dispatch(s_aichat_app,
                                                  AICHAT_EVENT_TRANSPORT_ERROR);
                        break;
                    }
                }
                (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_WAKE_WORD);
                const esp_err_t wake_ret = aichat_xiaozhi_send_wake_word(
                    s_aichat_xiaozhi, AICHAT_XIAOZHI_WAKE_WORD);
                const esp_err_t listen_ret = aichat_xiaozhi_send_start_listening(
                    s_aichat_xiaozhi);
                if (wake_ret != ESP_OK || listen_ret != ESP_OK) {
                    ESP_LOGW("AIChatUI", "WakeWord request failed: wake=%s listen=%s",
                             esp_err_to_name(wake_ret), esp_err_to_name(listen_ret));
                    (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TRANSPORT_ERROR);
                } else {
                    (void)aichat_app_dispatch(s_aichat_app,
                                              AICHAT_EVENT_AUDIO_CHANNEL_OPENED);
                    aichat_ui_set_uplink_enabled(true);
                }
            } else if (state == AICHAT_STATE_LISTENING ||
                       state == AICHAT_STATE_WAITING_RESPONSE ||
                       state == AICHAT_STATE_SPEAKING) {
                ESP_LOGI("AIChatUI", "WakeWord interrupts active conversation state=%s",
                         aichat_state_name(state));
                aichat_ui_set_uplink_enabled(false);
                const esp_err_t abort_ret = aichat_xiaozhi_abort_speaking(s_aichat_xiaozhi);
                const esp_err_t playback_ret = aichat_xiaozhi_interrupt_playback(
                    s_aichat_xiaozhi);
                const esp_err_t listen_ret = aichat_xiaozhi_send_start_listening(
                    s_aichat_xiaozhi);
                if (abort_ret != ESP_OK || playback_ret != ESP_OK || listen_ret != ESP_OK) {
                    ESP_LOGW("AIChatUI", "WakeWord interrupt failed: abort=%s playback=%s listen=%s",
                             esp_err_to_name(abort_ret), esp_err_to_name(playback_ret),
                             esp_err_to_name(listen_ret));
                    (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TRANSPORT_ERROR);
                } else {
                    (void)aichat_app_dispatch(s_aichat_app,
                                              AICHAT_EVENT_WAKE_WORD_INTERRUPT);
                    aichat_ui_set_uplink_enabled(true);
                }
            }
            break;
        case AICHAT_UI_EVENT_WAKEUP_END:
            /** Handle the local confirmation after the sleep request. */
            ESP_LOGI("AIChatUI", "Local WakeNet session ended");
            break;
        default:
            break;
        }
    }
    aichat_ui_render();
}

static void aichat_ui_render(void)
{
    if (s_aichat_view != NULL) {
        (void)aichat_view_render(s_aichat_view);
    }
}

static void aichat_ui_set_binding_notice(const aichat_xiaozhi_status_t *status,
                                         bool transport_failure)
{
    if (s_aichat_view == NULL) {
        return;
    }
    char notice[384];
    notice[0] = '\0';
    if (status != NULL && status->binding == AICHAT_XIAOZHI_BINDING_REQUIRED) {
        if (status->activation_code[0] != '\0' && status->activation_message[0] != '\0') {
            (void)snprintf(notice, sizeof(notice),
                           L(DESKTOP_TEXT_AICHAT_BINDING_MESSAGE_CODE),
                           status->activation_message, status->activation_code);
        } else if (status->activation_code[0] != '\0') {
            (void)snprintf(notice, sizeof(notice),
                           L(DESKTOP_TEXT_AICHAT_BINDING_CODE),
                           status->activation_code);
        } else if (status->activation_message[0] != '\0') {
            (void)snprintf(notice, sizeof(notice),
                           L(DESKTOP_TEXT_AICHAT_BINDING_MESSAGE),
                           status->activation_message);
        } else {
            (void)snprintf(notice, sizeof(notice),
                           L(DESKTOP_TEXT_AICHAT_BINDING_REQUIRED));
        }
    } else if (transport_failure) {
        (void)snprintf(notice, sizeof(notice),
                       L(DESKTOP_TEXT_AICHAT_BINDING_TRANSPORT));
    }
    (void)aichat_view_set_notice(s_aichat_view, notice);
}

static void aichat_ui_intent_cb(aichat_view_intent_t intent, void *user_data)
{
    (void)user_data;
    if (s_aichat_app == NULL || s_aichat_xiaozhi == NULL) {
        return;
    }
    const aichat_state_t state = aichat_app_state(s_aichat_app);
    aichat_app_result_t result = AICHAT_APP_RESULT_OK;
    esp_err_t transport_ret = ESP_OK;
    switch (intent) {
    case AICHAT_VIEW_INTENT_LISTEN:
        if (state == AICHAT_STATE_LISTENING) {
            aichat_ui_set_uplink_enabled(false);
            transport_ret = aichat_xiaozhi_send_stop_listening(s_aichat_xiaozhi);
            result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STOP_LISTENING);
        } else if (state == AICHAT_STATE_SPEAKING ||
                   state == AICHAT_STATE_WAITING_RESPONSE) {
            aichat_ui_set_uplink_enabled(false);
            transport_ret = aichat_xiaozhi_abort_speaking(s_aichat_xiaozhi);
            if (transport_ret == ESP_OK) {
                transport_ret = aichat_xiaozhi_interrupt_playback(s_aichat_xiaozhi);
            }
            if (transport_ret == ESP_OK) {
                transport_ret = aichat_xiaozhi_send_start_listening(s_aichat_xiaozhi);
            }
            result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_WAKE_WORD_INTERRUPT);
            if (result == AICHAT_APP_RESULT_OK && transport_ret == ESP_OK) {
                aichat_ui_set_uplink_enabled(true);
            }
        } else if (state == AICHAT_STATE_IDLE &&
                   aichat_xiaozhi_is_connected(s_aichat_xiaozhi)) {
            if (!aichat_xiaozhi_is_session_ready(s_aichat_xiaozhi)) {
                transport_ret = aichat_xiaozhi_open_audio(s_aichat_xiaozhi);
            }
            if (transport_ret == ESP_OK) {
                result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_START_LISTENING);
            }
            if (result == AICHAT_APP_RESULT_OK && transport_ret == ESP_OK) {
                transport_ret = aichat_xiaozhi_send_start_listening(s_aichat_xiaozhi);
            }
            if (result == AICHAT_APP_RESULT_OK && transport_ret == ESP_OK) {
                aichat_ui_set_uplink_enabled(true);
            }
        } else {
            result = AICHAT_APP_RESULT_INVALID_STATE;
        }
        break;
    case AICHAT_VIEW_INTENT_STOP:
        aichat_ui_set_uplink_enabled(false);
        transport_ret = aichat_xiaozhi_send_stop_listening(s_aichat_xiaozhi);
        if (state == AICHAT_STATE_LISTENING) {
            result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STOP_LISTENING);
        }
        break;
    case AICHAT_VIEW_INTENT_RETRY:
        result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_RETRY);
        if (result == AICHAT_APP_RESULT_OK) {
            aichat_ui_set_binding_notice(NULL, false);
            if (s_aichat_audio != NULL && !aichat_audio_gmf_is_running(s_aichat_audio)) {
                transport_ret = aichat_audio_gmf_start(s_aichat_audio);
            }
            (void)aichat_xiaozhi_stop(s_aichat_xiaozhi);
            if (transport_ret == ESP_OK) {
                transport_ret = aichat_xiaozhi_start(s_aichat_xiaozhi);
            }
            if (transport_ret == ESP_OK) {
                result = aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STARTUP_READY);
            }
        }
        break;
    case AICHAT_VIEW_INTENT_CLEAR:
        result = aichat_app_clear_history(s_aichat_app);
        break;
    default:
        result = AICHAT_APP_RESULT_INVALID_ARGUMENT;
        break;
    }
    if (transport_ret != ESP_OK || result != AICHAT_APP_RESULT_OK) {
        ESP_LOGW("AIChatUI", "intent failed intent=%d transport=%s app=%d",
                 intent, esp_err_to_name(transport_ret), result);
        if (state != AICHAT_STATE_ERROR && state != AICHAT_STATE_STOPPING) {
            (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_TRANSPORT_ERROR);
        }
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
    }
    aichat_ui_render();
}

static void aichat_ui_cleanup(void)
{
    if (!s_aichat_app_open && s_aichat_view == NULL && s_aichat_app == NULL &&
        s_aichat_audio == NULL && s_aichat_xiaozhi == NULL && s_aichat_playback == NULL) {
        return;
    }
    s_aichat_accept_events = false;
    if (s_aichat_poll_timer != NULL) {
        lv_timer_delete(s_aichat_poll_timer);
        s_aichat_poll_timer = NULL;
    }
    if (s_aichat_audio != NULL) {
        (void)aichat_audio_gmf_stop(s_aichat_audio);
        aichat_audio_gmf_destroy(s_aichat_audio);
        s_aichat_audio = NULL;
    }
    if (s_aichat_xiaozhi != NULL) {
        aichat_xiaozhi_destroy(s_aichat_xiaozhi);
        s_aichat_xiaozhi = NULL;
    }
    if (s_aichat_playback != NULL) {
        aichat_audio_playback_destroy(s_aichat_playback);
        s_aichat_playback = NULL;
    }
    if (s_aichat_event_queue != NULL) {
        vQueueDelete(s_aichat_event_queue);
        s_aichat_event_queue = NULL;
    }
    if (s_aichat_app != NULL) {
        const aichat_state_t state = aichat_app_state(s_aichat_app);
        if (state != AICHAT_STATE_CLOSED && state != AICHAT_STATE_STOPPING) {
            (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_UI_CLOSE);
        }
        if (aichat_app_state(s_aichat_app) == AICHAT_STATE_STOPPING) {
            (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_SHUTDOWN_COMPLETE);
        }
        aichat_app_destroy(s_aichat_app);
        s_aichat_app = NULL;
    }
    if (s_aichat_view != NULL) {
        aichat_view_destroy(s_aichat_view);
        s_aichat_view = NULL;
    }
    s_aichat_app_open = false;
}

static void build_aichat(void)
{
    const esp_err_t music_ret = music_player_service_return_home();
    if (music_ret != ESP_OK && music_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("AIChatUI", "Music handoff failed before AIChat open: %s",
                 esp_err_to_name(music_ret));
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        return;
    }
    ESP_LOGI("AIChatUI", "audio ownership handoff: Music -> AIChat");

    s_aichat_event_queue = xQueueCreate(16U, sizeof(aichat_ui_event_t));
    s_aichat_accept_events = s_aichat_event_queue != NULL;
    if (s_aichat_event_queue == NULL ||
        aichat_app_create(&s_aichat_app) != AICHAT_APP_RESULT_OK ||
        aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_UI_OPEN) != AICHAT_APP_RESULT_OK) {
        aichat_ui_cleanup();
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        return;
    }
    s_aichat_view = aichat_view_create(s_full_body, s_aichat_app,
                                       aichat_ui_intent_cb, NULL);
    if (s_aichat_view == NULL) {
        aichat_ui_cleanup();
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        return;
    }
    s_aichat_app_open = true;
    aichat_ui_render();

    s_aichat_audio = aichat_audio_gmf_create(aichat_audio_pcm_cb,
                                             aichat_audio_afe_cb, NULL);
    s_aichat_playback = aichat_audio_playback_create();
    s_aichat_xiaozhi = aichat_xiaozhi_create(NULL, aichat_xiaozhi_event_cb, NULL);
    if (s_aichat_audio == NULL || s_aichat_playback == NULL || s_aichat_xiaozhi == NULL ||
        aichat_xiaozhi_set_playback(s_aichat_xiaozhi, s_aichat_playback) != ESP_OK ||
        aichat_audio_gmf_start(s_aichat_audio) != ESP_OK) {
        ESP_LOGE("AIChatUI", "AIChat audio startup failed");
        (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STARTUP_FAILED);
        aichat_ui_render();
        aichat_ui_cleanup();
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        return;
    }
    s_aichat_poll_timer = lv_timer_create(aichat_ui_poll_cb, 100, NULL);
    if (s_aichat_poll_timer == NULL) {
        ESP_LOGE("AIChatUI", "Failed to create AIChat event poll timer");
        (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STARTUP_FAILED);
        aichat_ui_render();
        return;
    }
    aichat_xiaozhi_status_t xiaozhi_status = {0};
    const esp_err_t ret = aichat_xiaozhi_start(s_aichat_xiaozhi);
    (void)aichat_xiaozhi_get_status(s_aichat_xiaozhi, &xiaozhi_status);
    if (ret != ESP_OK) {
        ESP_LOGE("AIChatUI", "Xiaozhi unavailable: ret=%s binding=%d configured=%d code=%s message=%s",
                 esp_err_to_name(ret), xiaozhi_status.binding, xiaozhi_status.configured,
                 xiaozhi_status.activation_code, xiaozhi_status.activation_message);
        (void)aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STARTUP_FAILED);
        /** Stop audio processing when session initialization fails. */
        aichat_ui_set_uplink_enabled(false);
        if (s_aichat_audio != NULL) {
            (void)aichat_audio_gmf_stop(s_aichat_audio);
        }
        aichat_ui_set_binding_notice(&xiaozhi_status, false);
        aichat_ui_render();
        if (xiaozhi_status.binding == AICHAT_XIAOZHI_BINDING_REQUIRED) {
            show_toast(L(DESKTOP_TEXT_AICHAT_BINDING_REQUIRED));
        } else {
            show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        }
        return;
    }
    ESP_LOGI("AIChatUI", "Xiaozhi bound; waiting for transport connection");
    if (aichat_app_dispatch(s_aichat_app, AICHAT_EVENT_STARTUP_READY) !=
        AICHAT_APP_RESULT_OK) {
        aichat_ui_cleanup();
        show_toast(L(DESKTOP_TEXT_AICHAT_ERROR));
        return;
    }
    aichat_ui_render();
}

static void build_i2ctools(void)
{
    i2ctools_view_build(s_full_body, s_full, s_full_close_button);
}

/**
 * @brief Choose the fullscreen host canvas for the active application.
 * @param id Application whose background is selected.
 * @return Canvas color for the application.
 */
static lv_color_t app_page_canvas(app_id_t id)
{
    switch (id) {
    case APP_MUSIC:
    case APP_VIDEO_PLAYER:
    case APP_SPECTRUM:
    case APP_ALBUM:
        return C_BLACK;
    case APP_CLOCK:
        return C_CLOCK_CANVAS;
    default:
        return C_CANVAS;
    }
}

/**
 * @brief Apply the next page background to the shared fullscreen body.
 * @param id Application whose background is applied.
 */
static void fullscreen_apply_page_canvas(app_id_t id)
{
    if (s_full_body == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(s_full_body, app_page_canvas(id), 0);
    lv_obj_set_style_bg_opa(s_full_body, LV_OPA_COVER, 0);
}

/**
 * @brief Select display-filling geometry for fullscreen applications.
 */
static void fullscreen_apply_body_padding(void)
{
    if (s_full_body == NULL) {
        return;
    }

    /** Apply the shared workspace insets. */
    lv_obj_set_style_pad_top(s_full_body, FULLSCREEN_INSET_Y, 0);
    lv_obj_set_style_pad_bottom(s_full_body, FULLSCREEN_INSET_Y, 0);
    lv_obj_set_style_pad_left(s_full_body, FULLSCREEN_INSET_X, 0);
    lv_obj_set_style_pad_right(s_full_body, FULLSCREEN_INSET_X, 0);
}

static void fullscreen_apply_screen_geometry(bool touch_fullscreen)
{
    if (touch_fullscreen) {
        lv_obj_set_pos(s_full, 0, 0);
        lv_obj_set_size(s_full, s_screen_width, s_screen_height);
        lv_obj_set_style_transform_pivot_x(s_full, 0, 0);
        lv_obj_set_style_transform_pivot_y(s_full, 0, 0);
        lv_obj_set_style_transform_scale_x(s_full, 256, 0);
        lv_obj_set_style_transform_scale_y(s_full, 256, 0);
        lv_obj_set_pos(s_full_body, 0, 0);
        lv_obj_set_size(s_full_body, s_screen_width, s_screen_height);
        fullscreen_apply_body_padding();
        return;
    }

    lv_obj_set_pos(s_full, 0, 0);
    lv_obj_set_size(s_full, s_screen_width, s_screen_height);
    lv_obj_set_style_transform_pivot_x(s_full, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_full, 0, 0);
    lv_obj_set_style_transform_scale_x(s_full, 256, 0);
    lv_obj_set_style_transform_scale_y(s_full, 256, 0);
    lv_obj_set_pos(s_full_body, 0, 0);
    lv_obj_set_size(s_full_body, s_screen_width, s_screen_height);
    fullscreen_apply_body_padding();
}

static void build_full_screen(app_id_t id)
{
    const app_entry_t *app = app_by_id(id);
    if (!usb_ui_cleanup_if_safe()) {
        return;
    }
    app_wifi_view_cleanup();
    app_ethernet_view_cleanup();
    app_serial_ui_destroy();
    (void)video_player_service_exit();
    app_video_player_view_cleanup();
    app_music_view_cleanup();
    aichat_ui_cleanup();
    spectrum_app_stop();
    app_clock_view_destroy();
    app_settings_view_destroy();
    app_factory_test_view_cleanup();
    app_touch_view_cleanup();
    app_photo_view_cleanup();
    s_full_exit_tracking = false;
    s_full_exit_requested = false;
    app_touch_gesture_end(&s_full_exit_gesture);
    if (s_full_exit_strip != NULL) {
        lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
    }
    const bool touch_fullscreen = id == APP_TOUCH;

    fullscreen_apply_page_canvas(id);
    fullscreen_apply_screen_geometry(touch_fullscreen);
    /** Remove workspace insets from pages that occupy the whole display. */
    if (id == APP_VIDEO_PLAYER || id == APP_SPECTRUM || id == APP_ALBUM ||
        id == APP_CLOCK) {
        lv_obj_set_style_pad_all(s_full_body, 0, 0);
    }
    lv_label_set_text(s_full_title, app_label(app));
    lv_obj_clean(s_full_body);
    /** Clear the previous page layout before the new page configures the shared body. */
    lv_obj_set_layout(s_full_body, LV_LAYOUT_NONE);
    lv_obj_add_flag(s_full_header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_full_close_button, LV_OBJ_FLAG_HIDDEN);
    if (touch_fullscreen) {
        lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_TOUCH_DRAWING_SUBTITLE));
        if (!app_touch_view_build(s_full_body, s_screen_width, s_screen_height,
                                    touch_view_close_cb, NULL)) {
            ESP_LOGE("TouchUI", "Touch drawing view startup failed");
            lv_obj_add_flag(s_full, LV_OBJ_FLAG_HIDDEN);
            show_toast(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
            return;
        }
    } else {
        if (s_full_exit_strip != NULL) {
            lv_obj_remove_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_full_exit_strip);
        }
        if (id == APP_WIFI) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_WIFI_SUBTITLE));
            build_wifi();
        } else if (id == APP_ETHERNET) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_ETHERNET_SUBTITLE));
            build_ethernet();
        } else if (id == APP_CALCULATOR) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_CALCULATOR_SUBTITLE));
            build_calculator();
        } else if (id == APP_MUSIC) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_FULLSCREEN_MUSIC));
            esp_err_t music_ret = music_player_service_enter();
            if (music_ret != ESP_OK) {
                show_toast(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
            }
            app_music_view_build(s_full_body, show_toast);
        } else if (id == APP_VIDEO_PLAYER) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_FULLSCREEN_VIDEO));
            const esp_err_t music_ret = music_player_service_return_home();
            if (music_ret != ESP_OK && music_ret != ESP_ERR_INVALID_STATE) {
                show_toast(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
            }
            const esp_err_t video_ret = video_player_service_enter();
            if (video_ret != ESP_OK && video_ret != ESP_ERR_NOT_FOUND) {
                show_toast(L(DESKTOP_TEXT_VIDEO_ERROR));
            }
            app_video_player_view_build(s_full_body, show_toast);
        } else if (id == APP_GPIO) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_GPIO_SUBTITLE));
            build_gpio();
        } else if (id == APP_SERIAL) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_SERIAL_SUBTITLE));
            app_serial_ui_build(s_full_body, s_full, show_toast);
        } else if (id == APP_USBSTICK) {
            ESP_LOGI(USB_UI_TAG, "page-open app=USBStick");
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_USB_SUBTITLE));
            build_usbstick();
        } else if (id == APP_I2CTOOLS) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_I2CTOOLS_SUBTITLE));
            build_i2ctools();
        } else if (id == APP_CLOCK) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_CLOCK_SUBTITLE));
            app_clock_view_build(s_full_body, show_toast);
        } else if (id == APP_SETTINGS) {
            app_settings_view_build(s_full_body, show_toast, settings_language_cb,
                                      settings_font_cb);
        } else if (id == APP_FACTORY_TEST) {
            app_factory_test_view_build(s_full_body, show_toast,
                                          factory_test_cover_cb, NULL);
        } else if (id == APP_AICHAT) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_AICHAT_SUBTITLE));
            build_aichat();
        } else if (id == APP_SPECTRUM) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_SPECTRUM_SUBTITLE));
            const esp_err_t spectrum_ret = spectrum_app_start(s_full_body);
            if (spectrum_ret != ESP_OK) {
                ESP_LOGE("SpectrumUI", "Spectrum startup failed: %s",
                         esp_err_to_name(spectrum_ret));
                show_toast(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
            }
        } else if (id == APP_ALBUM) {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_ALBUM_SUBTITLE));
            if (!app_photo_view_build(s_full_body, show_toast)) {
                ESP_LOGE("PhotoAlbum", "album startup failed");
                lv_obj_add_flag(s_full, LV_OBJ_FLAG_HIDDEN);
                if (s_full_exit_strip != NULL) {
                    lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_HIDDEN);
                }
                show_toast(L(DESKTOP_TEXT_ALBUM_EMPTY));
                return;
            }
        } else {
            lv_label_set_text(s_full_subtitle, L(DESKTOP_TEXT_DIAGNOSTICS_SUBTITLE));
            build_diagnostics();
        }
    }
    lv_obj_remove_flag(s_full, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_full);
    if (touch_fullscreen) {
        lv_obj_move_foreground(s_full_body);
    } else if (s_full_exit_strip != NULL) {
        /** Raise the exit overlay above the fullscreen page for touch handling. */
        lv_obj_move_foreground(s_full_exit_strip);
    }
}

/**
 * @brief Show the app icon or fallback initial before synchronous application initialization.
 * @param app Application entry containing its icon and title.
 * @return Launch overlay object.
 */
static lv_obj_t *show_app_launch_screen(const app_entry_t *app)
{
    lv_obj_t *launch = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(s_root)));
    style_plain(launch, C_CANVAS, 0);
    lv_obj_set_size(launch, s_screen_width, s_screen_height);
    lv_obj_set_pos(launch, 0, 0);
    lv_obj_set_style_text_font(launch, lv_obj_get_style_text_font(s_root, 0), 0);
    lv_obj_clear_flag(launch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(launch, LV_OBJ_FLAG_CLICKABLE);

    const void *icon_source = app_icon_source(app);
    if (desktop_icons_ready() && icon_source != NULL) {
        lv_obj_t *icon = lv_image_create(launch);
        lv_image_set_src(icon, icon_source);
        lv_obj_set_size(icon, LAUNCH_ICON_SIZE, LAUNCH_ICON_SIZE);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -76);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *badge = lv_obj_create(launch);
        style_plain(badge, lv_color_hex(app->color_hex), 20);
        lv_obj_set_size(badge, 80, 80);
        lv_obj_align(badge, LV_ALIGN_CENTER, 0, -76);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char initial[2] = {app->key[0], '\0'};
        if (initial[0] >= 'a' && initial[0] <= 'z') {
            initial[0] -= 'a' - 'A';
        }
        lv_obj_t *symbol = make_label(badge, initial, C_SYSTEM_TEXT, 0, 0);
        lv_obj_set_style_text_font(symbol, lv_font_get_default(), 0);
        lv_obj_center(symbol);
    }

    lv_obj_t *title = make_label(launch, app_label(app), C_INK, 0, 0);
    lv_obj_set_width(title, s_screen_width - 40);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *status = make_label(launch, L(DESKTOP_TEXT_APP_LAUNCH_STARTING), C_INK, 0, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *hint = make_label(launch, L(DESKTOP_TEXT_APP_LAUNCH_HINT), C_INK_MUTED, 0, 0);
    lv_obj_set_width(hint, s_screen_width - 40);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 72);

    /** Flush the launch overlay under the LVGL lock before building the application. */
    lv_refr_now(lv_obj_get_display(s_root));
    return launch;
}

static void open_full_screen(app_id_t id)
{
    const app_entry_t *app = app_by_id(id);
    if (s_app_launching || app == NULL || s_root == NULL) {
        return;
    }

    s_app_launching = true;
    /** Discard queued text before rebuilding its fullscreen parent. */
    deferred_text_drop();
    deferred_tiles_drop();
    const uint32_t started = lv_tick_get();
    lv_obj_t *launch = show_app_launch_screen(app);
    const uint32_t feedback_ms = lv_tick_elaps(started);
    build_full_screen(id);
    /** Remove the launch overlay after every builder result. */
    lv_obj_delete(launch);
    if (lv_indev_active() != NULL) {
        lv_indev_wait_release(lv_indev_active());
    }
    lv_refr_now(lv_obj_get_display(s_root));
    s_app_launching = false;
    ESP_LOGI("desktop_ui", "app-launch app=%s feedback_ms=%" PRIu32 " total_ms=%" PRIu32,
             app->key, feedback_ms, lv_tick_elaps(started));
}

static bool app_is_full(app_id_t id)
{
    return id == APP_TOUCH || id == APP_SETTINGS || id >= APP_WIFI;
}

static void open_app(app_id_t id)
{
    if (s_app_launching) {
        return;
    }
    if (app_is_full(id)) {
        open_full_screen(id);
    } else if (id == APP_POWER) {
        open_power();
    } else {
        open_info_modal(id);
    }
}

static void reset_ui_object_pointers(void)
{
    s_root = NULL;
    s_grid = NULL;
    for (size_t i = 0; i < sizeof(s_page_dots) / sizeof(s_page_dots[0]); ++i) {
        s_page_dots[i] = NULL;
    }
    s_clock_label = NULL;
    s_wifi_icon = NULL;
    s_bt_icon = NULL;
    s_bt_indicator_shown = false;
    desktop_weather_bar_reset();
    s_modal = NULL;
    s_modal_panel = NULL;
    s_modal_title = NULL;
    s_modal_subtitle = NULL;
    s_modal_content = NULL;
    s_full = NULL;
    s_full_header = NULL;
    s_full_title = NULL;
    s_full_subtitle = NULL;
    s_full_body = NULL;
    s_full_close_button = NULL;
    s_full_exit_strip = NULL;
    s_full_exit_tracking = false;
    s_full_exit_requested = false;
    s_toast = NULL;
    s_calc_display = NULL;
    s_calc_history = NULL;
}

static void language_rebuild_async(void *user_data)
{
    desktop_language_t language = (desktop_language_t)(uintptr_t)user_data;
    s_language_switch_pending = false;
    if (!desktop_language_select(language)) {
        return;
    }
    /** Refetch weather using the selected language. */
    weather_service_set_language(language == DESKTOP_LANGUAGE_CHINESE);

    if (s_current_page >= DESKTOP_PAGE_COUNT) {
        s_current_page = 0;
    }
    desktop_ui_create();

    char message[96];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_SETTINGS_LANGUAGE_SWITCHED),
             language == DESKTOP_LANGUAGE_CHINESE
                 ? L(DESKTOP_TEXT_SETTINGS_LANGUAGE_CHINESE)
                 : L(DESKTOP_TEXT_SETTINGS_LANGUAGE_ENGLISH));
    show_toast(message);
}

static void request_language_switch(desktop_language_t language)
{
    if (s_language_switch_pending ||
        (language == DESKTOP_LANGUAGE_CHINESE && !desktop_language_chinese_available())) {
        return;
    }
    s_language_switch_pending = true;
    if (lv_async_call(language_rebuild_async, (void *)(uintptr_t)language) != LV_RESULT_OK) {
        s_language_switch_pending = false;
    }
}

/**
 * @brief Schedule the desktop rebuild requested by settings.
 * @param language Language requested in settings.
 */
static void settings_language_cb(desktop_language_t language)
{
    request_language_switch(language);
}

/**
 * @brief Acquire the LVGL adapter lock before scheduling a language rebuild from another task.
 * @param language Requested language.
 * @param user_data Callback context (unused).
 * @return true when the switch request is accepted; false if unavailable, pending or the lock cannot be acquired.
 */
static bool request_language_listener(desktop_language_t language, void *user_data)
{
    (void)user_data;
    if (s_language_switch_pending ||
        (language == DESKTOP_LANGUAGE_CHINESE && !desktop_language_chinese_available())) {
        return false;
    }
    if (!lvgl_display_lock(LANGUAGE_SWITCH_LOCK_TIMEOUT_MS)) {
        return false;
    }
    request_language_switch(language);
    lvgl_display_unlock();
    return true;
}

/**
 * @brief Rebuild widgets with the new font pool before releasing retired faces.
 * @param user_data LVGL asynchronous callback context (unused).
 */
static void font_reload_async(void *user_data)
{
    (void)user_data;
    s_font_reload_pending = false;
    desktop_ui_create();
    desktop_font_release_retired();
    show_toast(s_font_toast);
}

static void settings_font_cb(const char *file_name, bool applied)
{
    if (s_font_reload_pending) {
        return;
    }
    if (applied) {
        snprintf(s_font_toast, sizeof(s_font_toast), L(DESKTOP_TEXT_SETTINGS_FONT_SWITCHED),
                 file_name != NULL ? file_name : L(DESKTOP_TEXT_SETTINGS_FONT_BUILTIN));
    } else {
        snprintf(s_font_toast, sizeof(s_font_toast), "%s",
                 L(DESKTOP_TEXT_SETTINGS_FONT_FAILED));
    }
    s_font_reload_pending = true;
    if (lv_async_call(font_reload_async, NULL) != LV_RESULT_OK) {
        s_font_reload_pending = false;
    }
}

static void app_button_cb(lv_event_t *event)
{
    app_id_t id = (app_id_t)(uintptr_t)lv_event_get_user_data(event);
    open_app(id);
}

/** @brief Defer SPIFFS tile decoding by timer; fill embedded tiles after video teardown. */
#define DEFERRED_TILE_MAX 24U
#define DEFERRED_TILE_PER_TICK 1U
#define DEFERRED_TILE_TICK_MS 20U

typedef struct {
    lv_obj_t *tile;
    app_id_t id;
    const app_entry_t *app;
    const char *caption;
    int32_t tile_width;
    int32_t icon_size;
    bool use_image_icons;
} deferred_tile_t;

static deferred_tile_t s_deferred_tiles[DEFERRED_TILE_MAX];
static size_t s_deferred_tile_count;
static lv_timer_t *s_deferred_tile_timer;
static bool s_defer_tiles;

/**
 * @brief Build the icon, launch event and caption for one tile.
 * @param tile Tile object to populate.
 * @param id Application identifier used for the launch event.
 * @param app Application entry.
 * @param caption_text Text displayed under the icon.
 * @param tile_width Width of the tile in pixels.
 * @param icon_size Icon size in pixels.
 * @param use_image_icons Whether to use image icons.
 */
static void build_tile_content(lv_obj_t *tile, app_id_t id, const app_entry_t *app,
                               const char *caption_text, int32_t tile_width, int32_t icon_size,
                               bool use_image_icons)
{
    lv_obj_t *icon = NULL;
    if (use_image_icons) {
        icon = lv_image_create(tile);
        lv_image_set_src(icon, app_icon_source(app));
    } else {
        icon = lv_obj_create(tile);
        style_plain(icon, lv_color_hex(app->color_hex), 20);
        lv_obj_set_size(icon, icon_size, icon_size);
        lv_obj_t *glyph = make_label(icon, L(app->icon_key), C_SYSTEM_TEXT, 0, 0);
        lv_obj_center(glyph);
    }
    /** Restrict app launching to the icon bounds. */
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    /** Bubble icon gestures to the desktop page handler. */
    lv_obj_add_flag(icon, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(icon, app_button_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    /** Use icon opacity for press feedback. */
    lv_obj_set_style_opa(icon, LV_OPA_70, LV_STATE_PRESSED);
    /** Centre the icon and caption as one group. */
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -14);

    make_icon_caption(tile, icon, caption_text, tile_width);
}

static void deferred_tiles_drop(void)
{
    s_deferred_tile_count = 0U;
    if (s_deferred_tile_timer != NULL) {
        lv_timer_delete(s_deferred_tile_timer);
        s_deferred_tile_timer = NULL;
    }
}

static void deferred_tiles_apply(lv_timer_t *timer)
{
    /** Delay tile content while boot video covers the desktop. */
    if (desktop_boot_video_is_playing()) {
        return;
    }
    size_t applied = 0U;
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
    const size_t limit = DEFERRED_TILE_MAX;
#else
    const size_t limit = DEFERRED_TILE_PER_TICK;
#endif
    while (s_deferred_tile_count > 0U && applied < limit) {
        const deferred_tile_t entry = s_deferred_tiles[0];
        --s_deferred_tile_count;
        memmove(&s_deferred_tiles[0], &s_deferred_tiles[1],
                s_deferred_tile_count * sizeof(s_deferred_tiles[0]));
        build_tile_content(entry.tile, entry.id, entry.app, entry.caption, entry.tile_width,
                           entry.icon_size, entry.use_image_icons);
        ++applied;
    }
    if (s_deferred_tile_count == 0U) {
        lv_timer_delete(timer);
        s_deferred_tile_timer = NULL;
    }
}

#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
static void deferred_tiles_flush(void)
{
    if (s_deferred_tile_count == 0U) {
        return;
    }
    /** Finish pending tile content before revealing the desktop. */
    while (s_deferred_tile_count > 0U) {
        const deferred_tile_t entry = s_deferred_tiles[0];
        --s_deferred_tile_count;
        memmove(&s_deferred_tiles[0], &s_deferred_tiles[1],
                s_deferred_tile_count * sizeof(s_deferred_tiles[0]));
        build_tile_content(entry.tile, entry.id, entry.app, entry.caption, entry.tile_width,
                           entry.icon_size, entry.use_image_icons);
    }
    if (s_deferred_tile_timer != NULL) {
        lv_timer_delete(s_deferred_tile_timer);
        s_deferred_tile_timer = NULL;
    }
}
#endif

static void render_page(uint8_t page)
{
    /** Discard queued captions before rebuilding a desktop page. */
    if (!s_defer_text) {
        deferred_text_drop();
        deferred_tiles_drop();
    }
    const bool use_image_icons = desktop_icons_ready();
    const int grid_width = s_screen_width > (2U * DESKTOP_MARGIN_X)
                               ? s_screen_width - 2U * DESKTOP_MARGIN_X
                               : 1;
    const int grid_height = s_screen_height > (DESKTOP_TOP + DESKTOP_BOTTOM)
                                ? s_screen_height - DESKTOP_TOP - DESKTOP_BOTTOM
                                : 1;
    const int tile_width = (grid_width - (DESKTOP_COLUMNS - 1) * DESKTOP_TILE_GAP_X) / DESKTOP_COLUMNS;
    const int tile_height = (grid_height - (DESKTOP_ROWS - 1) * DESKTOP_TILE_GAP_Y) / DESKTOP_ROWS;
    const int icon_size = tile_width > 16 && tile_height > 56
                              ? (tile_width - 16 < 82 ? tile_width - 16 : 82)
                              : 1;

    s_current_page = page;
    lv_obj_clean(s_grid);
    for (uint8_t i = 0; i < s_page_counts[page]; i++) {
        const app_id_t id = (app_id_t)s_pages[page][i];
        const app_entry_t *app = app_by_id(id);
        const int row = i / DESKTOP_COLUMNS;
        const int col = i % DESKTOP_COLUMNS;
        lv_obj_t *tile = lv_button_create(s_grid);
        style_plain(tile, C_CANVAS, 18);
        /** Keep tiles transparent over the wallpaper. */
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
        /** Keep tile backgrounds outside the launch hit area. */
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(tile, col * (tile_width + DESKTOP_TILE_GAP_X),
                       row * (tile_height + DESKTOP_TILE_GAP_Y));
        lv_obj_set_size(tile, tile_width, tile_height);
        const char *caption = L(s_page_label_keys[page][i]);
        if (s_defer_tiles && s_deferred_tile_count < DEFERRED_TILE_MAX) {
            deferred_tile_t *entry = &s_deferred_tiles[s_deferred_tile_count++];
            entry->tile = tile;
            entry->id = id;
            entry->app = app;
            entry->caption = caption;
            entry->tile_width = tile_width;
            entry->icon_size = icon_size;
            entry->use_image_icons = use_image_icons;
        } else {
            build_tile_content(tile, id, app, caption, tile_width, icon_size, use_image_icons);
            /** Yield the display lock between tile builds during boot playback. */
            desktop_boot_video_yield();
        }
    }

    for (size_t i = 0; i < sizeof(s_page_dots) / sizeof(s_page_dots[0]); i++) {
        lv_obj_set_style_bg_color(s_page_dots[i], i == page ? C_ACCENT : C_MUTED, 0);
        lv_obj_set_width(s_page_dots[i], i == page ? 28 : 12);
    }
}

static void gesture_cb(lv_event_t *event)
{
    ESP_LOGD("desktop_ui", "gesture on %p (desktop root %p)",
             (void *)lv_event_get_current_target(event), (void *)s_root);
    if (!lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(s_full, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    uint8_t page = s_current_page;
    if (direction == LV_DIR_LEFT && page + 1U < DESKTOP_PAGE_COUNT) {
        page++;
    } else if (direction == LV_DIR_RIGHT && page > 0) {
        page--;
    } else {
        return;
    }

    render_page(page);
    char message[48];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_PAGE_SHOWN), page + 1);
    show_toast(message);
}

static void page_cb(lv_event_t *event)
{
    uint8_t page = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    render_page(page);
    char message[48];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_PAGE_SHOWN), page + 1);
    show_toast(message);
}

static void create_modal(void)
{
    s_modal = lv_obj_create(s_root);
    style_plain(s_modal, C_SYSTEM, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_50, 0);
    lv_obj_set_size(s_modal, PAD_W, PAD_H);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_add_event_cb(s_modal, modal_backdrop_cb, LV_EVENT_CLICKED, NULL);

    s_modal_panel = make_card(s_modal, 252, 60, 520, 480);
    lv_obj_add_flag(s_modal_panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    make_label(s_modal_panel, "Desktop", C_SYSTEM_TEXT, 24, 22);
    s_modal_title = make_label(s_modal_panel, L(DESKTOP_TEXT_APP_STATUS), C_INK, 72, 19);
    s_modal_subtitle = make_label(s_modal_panel, L(DESKTOP_TEXT_STATUS_SUBTITLE), C_INK_MUTED, 72, 43);
    make_button(s_modal_panel, L(DESKTOP_TEXT_CLOSE), 458, 16, 42, 42, C_MUTED, close_modal_cb, NULL);
    s_modal_content = lv_obj_create(s_modal_panel);
    style_plain(s_modal_content, C_SURFACE, 0);
    lv_obj_set_pos(s_modal_content, 24, 82);
    lv_obj_set_size(s_modal_content, 472, 396);
    lv_obj_clear_flag(s_modal_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void create_full_screen(void)
{
    s_full = lv_obj_create(s_root);
    style_plain(s_full, C_BLACK, 0);
    lv_obj_set_pos(s_full, 0, 0);
    lv_obj_set_size(s_full, s_screen_width, s_screen_height);
    /** Reset fullscreen scroll offsets before reopening an app. */
    lv_obj_clear_flag(s_full, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to(s_full, 0, 0, LV_ANIM_OFF);
    lv_obj_set_style_transform_pivot_x(s_full, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_full, 0, 0);
    s_full_header = lv_obj_create(s_full);
    lv_obj_t *header = s_full_header;
    style_plain(header, C_BLACK, 0);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, s_screen_width, 1);
    lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
    s_full_title = make_label(header, L(DESKTOP_TEXT_APPLICATIONS), C_SYSTEM_TEXT, 40, 14);
    s_full_subtitle = make_label(header, L(DESKTOP_TEXT_DESKTOP_APPLICATIONS), C_SYSTEM_TEXT, 40, 38);
    s_full_body = lv_obj_create(s_full);

    style_plain(s_full_body, C_CANVAS, 0);
    lv_obj_set_pos(s_full_body, 0, 0);
    lv_obj_set_size(s_full_body, s_screen_width, s_screen_height);
    fullscreen_apply_body_padding();
    lv_obj_clear_flag(s_full_body, LV_OBJ_FLAG_SCROLLABLE);
    s_full_close_button = make_button(s_full, L(DESKTOP_TEXT_BACK_HOME), 828, 12, 156, 44,
                                       C_SURFACE, close_full_cb, NULL);
    /** Provide a transparent bottom-edge swipe zone for leaving fullscreen apps. */
    s_full_exit_strip = lv_obj_create(s_root);
    style_plain(s_full_exit_strip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_full_exit_strip, LV_OPA_TRANSP, 0);
    const int32_t exit_strip_height = 10;
    lv_obj_set_size(s_full_exit_strip, s_screen_width, exit_strip_height);
    lv_obj_align(s_full_exit_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_full_exit_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_full_exit_strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_full_exit_strip, full_exit_strip_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_full_exit_strip, full_exit_strip_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_full_exit_strip, full_exit_strip_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_full_exit_strip, full_exit_strip_cb, LV_EVENT_PRESS_LOST, NULL);
    app_touch_gesture_init(&s_full_exit_gesture, s_screen_width, s_screen_height);
    lv_obj_add_flag(s_full, LV_OBJ_FLAG_HIDDEN);
}

static bool desktop_calendar(time_policy_calendar_t *calendar)
{
    time_service_snapshot_t snapshot;
    const time_t now = time(NULL);
    if (calendar == NULL || time_service_get_snapshot(&snapshot) != ESP_OK ||
        snapshot.sync_state != TIME_SYNC_SYNCHRONIZED ||
        !time_policy_is_sane_system_epoch(snapshot.synchronized_epoch) ||
        !time_policy_is_sane_system_epoch((int64_t)now) ||
        !time_policy_is_valid_offset(snapshot.timezone_offset_minutes)) {
        return false;
    }
    return time_policy_epoch_to_calendar((int64_t)now +
                                             (int64_t)snapshot.timezone_offset_minutes * 60,
                                         calendar);
}

/**
 * @brief Check whether the desktop can enter automatic sleep.
 * @return true when the desktop is visible and no launch or modal is active.
 */
static bool desktop_is_foreground(void)
{
    return !s_app_launching && s_full != NULL && s_modal != NULL &&
           lv_obj_has_flag(s_full, LV_OBJ_FLAG_HIDDEN) &&
           lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static int64_t idle_now_ms(void)
{
    /** Use monotonic time for inactivity tracking. */
    return esp_timer_get_time() / 1000;
}

/**
 * @brief Use the system clock for the sleep display.
 * @param context Callback context (unused).
 * @param [out] hour Receives the local hour.
 * @param [out] minute Receives the local minute.
 * @param [out] second Receives the local second.
 * @return true when local time is available and outputs are valid.
 */
static bool sleep_time_source(void *context, uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    time_policy_calendar_t calendar;
    (void)context;
    if (hour == NULL || minute == NULL || second == NULL || !desktop_calendar(&calendar)) {
        return false;
    }
    *hour = (uint8_t)calendar.hour;
    *minute = (uint8_t)calendar.minute;
    *second = (uint8_t)calendar.second;
    return true;
}

static void sleep_exit(void)
{
    if (!s_sleeping) {
        return;
    }
    /** Restore clock frequency and brightness before revealing the desktop. */
    power_sleep_exit(&s_sleep_power);
    if (s_clock_timer != NULL) {
        lv_timer_resume(s_clock_timer);
    }
    desktop_sleep_view_destroy();
    s_sleeping = false;
    power_idle_mark_awake(&s_idle_policy, idle_now_ms());
    ESP_LOGI("desktop_ui", "sleep-wake");
}

static void sleep_enter(void)
{
    if (s_sleeping || s_root == NULL || !s_idle_activity_hooked) {
        return;
    }
    /** Place the sleep screen on the system layer above app overlays. */
    desktop_sleep_view_build(lv_display_get_layer_sys(lv_obj_get_display(s_root)),
                           s_screen_width, s_screen_height, sleep_time_source, NULL);
    if (!desktop_sleep_view_is_built()) {
        ESP_LOGW("desktop_ui", "sleep screen unavailable; staying awake");
        return;
    }
    if (s_clock_timer != NULL) {
        /** Suspend status-bar refresh while the sleep screen covers it. */
        lv_timer_pause(s_clock_timer);
    }

    power_sleep_enter(&s_sleep_power);
    s_sleeping = true;
    power_idle_mark_sleeping(&s_idle_policy);
    ESP_LOGI("desktop_ui", "sleep-enter timeout=%" PRIu32 "s backlight=%s",
             power_idle_get_timeout_seconds(&s_idle_policy),
             s_sleep_power.saved_backlight_valid ? "dimmed" : "unchanged");
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_ms = idle_now_ms();
    /** Read the current sleep timeout from settings. */
    power_idle_set_timeout(&s_idle_policy, sleep_timeout_get());

    if (s_sleeping) {
        desktop_sleep_view_refresh();
        return;
    }
    if (!desktop_is_foreground()) {
        /** Restart the idle countdown while an application or modal is open. */
        power_idle_note_activity(&s_idle_policy, now_ms);
        return;
    }
    if (power_idle_should_sleep(&s_idle_policy, now_ms)) {
        sleep_enter();
    }
}

/**
 * @brief Handle wake input on the LVGL adapter task with its lock held.
 * @param event LVGL input event.
 */
static void touch_activity_cb(lv_event_t *event)
{
    (void)event;
    if (s_sleeping) {
        sleep_exit();
        return;
    }
    power_idle_note_activity(&s_idle_policy, idle_now_ms());
}

static void idle_activity_hook_install(void)
{
    if (s_idle_activity_hooked) {
        return;
    }
    lv_indev_t *touch = display_get_touch_indev();
    if (touch == NULL) {
        /** Require a touch wake callback before enabling sleep. */
        ESP_LOGW("desktop_ui", "touch device unavailable; idle sleep stays disabled");
        return;
    }
    lv_indev_add_event_cb(touch, touch_activity_cb, LV_EVENT_PRESSED, NULL);
    /** Treat a held touch as continued activity. */
    lv_indev_add_event_cb(touch, touch_activity_cb, LV_EVENT_PRESSING, NULL);
    power_idle_init(&s_idle_policy, sleep_timeout_get(), idle_now_ms());
    s_idle_activity_hooked = true;
    ESP_LOGI("desktop_ui", "idle sleep armed: timeout=%" PRIu32 "s", sleep_timeout_get());
}

/**
 * @brief Refresh the Wi-Fi icon under the LVGL adapter lock when the desktop exists.
 */
static void wifi_indicator_apply(void)
{
    s_wifi_indicator_pending = false;
    if (s_wifi_icon == NULL) {
        return;
    }
    if (s_wifi_indicator_connected) {
        lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Poll Wi-Fi state from the LVGL timer task.
 */
static void wifi_indicator_refresh(void)
{
    s_wifi_indicator_connected = wifi_service_get_state() == WIFI_SERVICE_STATE_CONNECTED;
    wifi_indicator_apply();
}

/**
 * @brief Poll the Bluetooth peer on the LVGL task and repaint only changed state.
 */
static void bt_indicator_refresh(void)
{
    if (s_bt_icon == NULL) {
        return;
    }
    const bool connected = music_player_service_bt_connected();
    if (connected == s_bt_indicator_shown) {
        return;
    }
    s_bt_indicator_shown = connected;
    ESP_LOGI("desktop_ui", "bluetooth link %s", connected ? "up" : "down");
    if (connected) {
        lv_obj_remove_flag(s_bt_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bt_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Acquire the LVGL lock for event-task updates; defer to the timer if it is busy.
 * @param connected Current Wi-Fi link state.
 * @param user_data Listener context (unused).
 */
static void wifi_link_listener(bool connected, void *user_data)
{
    (void)user_data;
    s_wifi_indicator_connected = connected;
    if (s_wifi_icon == NULL) {
        s_wifi_indicator_pending = true;
        return;
    }
    if (!lvgl_display_lock(WIFI_INDICATOR_LOCK_TIMEOUT_MS)) {
        s_wifi_indicator_pending = true;
        return;
    }
    wifi_indicator_apply();
    lvgl_display_unlock();
}

static void wifi_indicator_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    wifi_indicator_refresh();
}

void desktop_ui_register_wifi_indicator(void)
{
    if (s_wifi_indicator_registered) {
        return;
    }
    if (wifi_service_register_link_listener(wifi_link_listener, NULL) != ESP_OK) {
        ESP_LOGW("desktop_ui", "Wi-Fi status indicator listener unavailable");
        return;
    }
    s_wifi_indicator_registered = true;
    /** Seed cached Wi-Fi state from the service before the next periodic refresh. */
    s_wifi_indicator_connected = wifi_service_get_state() == WIFI_SERVICE_STATE_CONNECTED;
    s_wifi_indicator_pending = true;
}

void desktop_ui_register_weather_widget(void)
{
    /** Attach the weather listener once after service initialization. */
    desktop_weather_bar_register();
}

static void clock_cb(lv_timer_t *timer)
{
    (void)timer;
    char time_text[16];
    time_policy_calendar_t calendar;

    if (!desktop_calendar(&calendar)) {
        snprintf(time_text, sizeof(time_text), "--:--");
    } else {
        snprintf(time_text, sizeof(time_text), "%02u:%02u", calendar.hour, calendar.minute);
    }
    lv_label_set_text(s_clock_label, time_text);

    if (s_wifi_indicator_pending) {
        wifi_indicator_apply();
    }

    desktop_weather_bar_tick();

    bt_indicator_refresh();
}

void desktop_ui_create(void)
{
    uint8_t page = s_current_page < DESKTOP_PAGE_COUNT ? s_current_page : 0;
    /** Register the catalog listener for language-triggered desktop rebuilds. */
    desktop_language_register_listener(request_language_listener, NULL);
    /** Leave sleep before rebuilding desktop objects. */
    sleep_exit();
    if (display_get_size(&s_screen_width, &s_screen_height) != ESP_OK) {
        ESP_LOGE("desktop_ui", "Unable to obtain LCD size from esp_board_manager");
        return;
    }
    const lv_font_t *font = desktop_font_get_primary();
    if (font == NULL) {
        font = lv_font_get_default();
    }

    /** Reset deferred queues and enable text deferral during boot playback. */
    deferred_text_drop();
    deferred_tiles_drop();
    s_defer_text = desktop_boot_video_is_playing();
    s_defer_tiles = s_defer_text;
    s_build_mark = esp_timer_get_time();
    build_phase("start");

    app_clock_view_destroy();
    if (s_clock_timer != NULL) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
    if (s_wifi_timer != NULL) {
        lv_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
    }
    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (!usb_ui_cleanup_if_safe()) {
        return;
    }
    i2ctools_view_cleanup();
    app_serial_ui_destroy();
    if (!desktop_boot_video_is_playing()) {
        /** Preserve player resources while the boot video is active. */
        (void)video_player_service_exit();
        app_video_player_view_cleanup();
    }
    app_music_view_cleanup();
    spectrum_app_stop();
    app_ethernet_view_cleanup();
    app_touch_view_cleanup();
    app_photo_view_cleanup();

    lv_obj_t *root = desktop_root();
    if (desktop_boot_video_is_playing() && !s_desktop_hidden) {
        s_desktop_hidden = true;
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_timer_create(desktop_reveal_cb, DESKTOP_REVEAL_POLL_MS, NULL);
    }
    (void)music_player_service_exit();
    app_wifi_view_cleanup();
    /** Forget the wallpaper pointer before its desktop tree is destroyed. */
    desktop_wallpaper_detach();
    lv_obj_clean(root);
    reset_ui_object_pointers();
    s_root = root;
    s_current_page = page;
    style_plain(s_root, C_CANVAS, 0);
    lv_obj_set_size(s_root, s_screen_width, s_screen_height);
    lv_obj_set_style_text_font(s_root, font, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    build_phase("cleanup");

    if (!desktop_wallpaper_attach(s_root)) {
        ESP_LOGW("desktop_ui", "wallpaper layer unavailable; using plain desktop colour");
    }
    build_phase("wallpaper");

    lv_obj_t *system_bar = lv_obj_create(s_root);
    style_plain(system_bar, C_SYSTEM, 0);
    lv_obj_set_size(system_bar, s_screen_width, 54);
    /** Create the centred clock and right-aligned connection indicators. */
    s_clock_label = make_label(system_bar, "00:00", C_SYSTEM_TEXT, 0, 0);

    lv_obj_set_style_text_font(s_clock_label, desktop_font_get_artist(), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);
    /** Use the built-in symbol font for connection icons. */
    s_wifi_icon = make_label(system_bar, LV_SYMBOL_WIFI, C_SYSTEM_TEXT, 0, 0);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, -22, 0);
    /** Hide the Bluetooth indicator until a peer connects. */
    s_bt_icon = make_label(system_bar, LV_SYMBOL_BLUETOOTH, C_SYSTEM_TEXT, 0, 0);
    lv_obj_set_style_text_font(s_bt_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bt_icon, LV_ALIGN_RIGHT_MID, -54, 0);
    lv_obj_add_flag(s_bt_icon, LV_OBJ_FLAG_HIDDEN);
    /** Reserve the left status-bar area for weather fields. */
    desktop_weather_bar_build(system_bar);
    build_phase("system_bar");

    s_grid = lv_obj_create(s_root);
    style_plain(s_grid, C_CANVAS, 0);
    /** Place wallpaper beneath the transparent desktop grid. */
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    /** Bubble grid and tile gestures to the desktop handler. */
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /** Leave non-icon areas unclickable while the screen receives swipe input. */
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_grid, DESKTOP_MARGIN_X, DESKTOP_TOP);
    lv_obj_set_size(s_grid,
                    s_screen_width > (2U * DESKTOP_MARGIN_X)
                        ? s_screen_width - 2U * DESKTOP_MARGIN_X
                        : 1,
                    s_screen_height > (DESKTOP_TOP + DESKTOP_BOTTOM)
                        ? s_screen_height - DESKTOP_TOP - DESKTOP_BOTTOM
                        : 1);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < DESKTOP_PAGE_COUNT; i++) {
        s_page_dots[i] = lv_button_create(s_root);
        style_plain(s_page_dots[i], C_MUTED, 8);
        lv_obj_set_pos(s_page_dots[i],
                       s_screen_width / 2 - 24 + (int)i * 48,
                       s_screen_height > 34 ? s_screen_height - 34 : 0);
        lv_obj_set_size(s_page_dots[i], 12, 12);
        lv_obj_set_ext_click_area(s_page_dots[i], 16);
        lv_obj_add_event_cb(s_page_dots[i], page_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    build_phase("grid");

    create_modal();
    create_full_screen();
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);
    /** Handle page swipes that reach the screen directly. */
    lv_obj_add_event_cb(lv_screen_active(), gesture_cb, LV_EVENT_GESTURE, NULL);
    build_phase("shell");

    s_toast = lv_label_create(s_root);
    lv_label_set_text(s_toast, "");
    lv_obj_set_style_bg_color(s_toast, C_SYSTEM, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_toast, C_SYSTEM_TEXT, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 12, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    render_page(page);
    build_phase("tiles");
    clock_cb(NULL);
    s_clock_timer = lv_timer_create(clock_cb, 1000, NULL);
    /** Seed the Wi-Fi indicator and start periodic refresh. */
    wifi_indicator_refresh();
    s_bt_indicator_shown = false;
    bt_indicator_refresh();
    s_wifi_timer = lv_timer_create(wifi_indicator_timer_cb, WIFI_INDICATOR_REFRESH_PERIOD_MS, NULL);

    /** Create the persistent desktop idle timer once. */
    idle_activity_hook_install();
    if (s_idle_timer == NULL) {
        s_idle_timer = lv_timer_create(idle_timer_cb, IDLE_POLL_PERIOD_MS, NULL);
    }

    /** Release retired fonts after all widgets have been rebound. */
    desktop_font_release_retired();
    if (!s_font_notice_shown && desktop_font_boot_notice_wanted()) {
        s_font_notice_shown = true;
        show_font_notice();
    }
    build_phase("end");

    /** Schedule deferred labels in bounded batches after desktop construction. */
    if (s_deferred_count > 0U && s_deferred_timer == NULL) {
        s_deferred_timer = lv_timer_create(deferred_text_apply, DEFERRED_TEXT_TICK_MS, NULL);
    }
    if (s_deferred_tile_count > 0U && s_deferred_tile_timer == NULL) {
        s_deferred_tile_timer = lv_timer_create(deferred_tiles_apply, DEFERRED_TILE_TICK_MS, NULL);
    }
    /** Apply text immediately for objects created after this desktop build. */
    s_defer_text = false;
    s_defer_tiles = false;
}
