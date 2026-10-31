#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_board_manager.h"

#include "display.h"
#include "display_image_buffers.h"
#include "lvgl.h"
#include "desktop_font.h"
#include "desktop_language.h"
#include "desktop_ui.h"
#include "video/desktop_boot_video.h"
#include "wifi_service.h"
#include "ethernet_service.h"
#include "serial_terminal.h"
#include "memory_monitor.h"
#include "assets_storage.h"
#include "audio_runtime.h"
#include "music_player_service.h"
#include "sleep_timeout.h"
#include "standby_prefs.h"
#include "settings_server.h"
#include "weather_service.h"

#define TAG "FactoryDemo"

/** @brief Display-lock retry slice and total timeout, in milliseconds. */
#define UI_LOCK_SLICE_MS 1000U
#define UI_LOCK_TOTAL_MS 10000U

/**
 * @brief Acquire the display lock with a bounded retry timeout.
 * @param[out] waited_ms Optional elapsed wait time in milliseconds.
 * @return true when the lock is held by the caller.
 */
static bool ui_acquire_display_lock(uint32_t *waited_ms)
{
    const int64_t start_us = esp_timer_get_time();
    uint32_t waited = 0U;

    while (waited < UI_LOCK_TOTAL_MS) {
        const uint32_t remaining = UI_LOCK_TOTAL_MS - waited;
        const uint32_t slice = remaining < UI_LOCK_SLICE_MS ? remaining : UI_LOCK_SLICE_MS;
        if (lvgl_display_lock((int)slice)) {
            if (waited_ms != NULL) {
                *waited_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
            }
            return true;
        }
        waited += slice;
        if (waited < UI_LOCK_TOTAL_MS) {
            ESP_LOGW(TAG, "LVGL lock busy, waited %" PRIu32 " ms", waited);
        }
    }

    if (waited_ms != NULL) {
        *waited_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    }
    return false;
}

/**
 * @brief Initialize board services, restore settings and create the desktop.
 */
void app_main(void)
{
    /** Log the running firmware version and ELF digest. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc != NULL) {
        ESP_LOGI(TAG, "firmware %s (%s) elf_sha256=%02x%02x%02x%02x%02x%02x%02x%02x",
                 app_desc->version, app_desc->project_name,
                 app_desc->app_elf_sha256[0], app_desc->app_elf_sha256[1],
                 app_desc->app_elf_sha256[2], app_desc->app_elf_sha256[3],
                 app_desc->app_elf_sha256[4], app_desc->app_elf_sha256[5],
                 app_desc->app_elf_sha256[6], app_desc->app_elf_sha256[7]);
    }

    /** Initialize NVS before restoring persistent settings. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable; stored settings will not be restored: %s",
                 esp_err_to_name(nvs_ret));
    }

    esp_err_t ret = esp_board_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board manager initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    /** Keep the backlight off until a valid video or desktop frame is ready. */
    ret = display_backlight_off();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight blank failed: %s", esp_err_to_name(ret));
    }

    memory_monitor_report("boot");

    ret = memory_monitor_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Periodic memory report unavailable: %s", esp_err_to_name(ret));
    }

    ret = assets_storage_mount();
    if (ret != ESP_OK) {
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
        ESP_LOGW(TAG, "SPIFFS assets unavailable; file-backed images may be missing: %s",
                 esp_err_to_name(ret));
#else
        ESP_LOGW(TAG, "SPIFFS icon assets unavailable; using fallback app icons: %s",
                 esp_err_to_name(ret));
#endif
    }

    ret = audio_runtime_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio runtime initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    memory_monitor_report("after_audio_runtime");

    /** Restore saved audio volume and mute state. */
    music_player_service_restore_volume();
    memory_monitor_report("after_volume_restore");

    /** Restore desktop sleep timeout and standby preferences. */
    ret = sleep_timeout_restore();
    standby_prefs_restore();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sleep timeout preference unavailable: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Auto sleep timeout %" PRIu32 " s", sleep_timeout_get());

    ret = ethernet_service_init();
    if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Ethernet runtime disabled or not configured");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet runtime unavailable: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Ethernet runtime startup scheduled asynchronously");
    }

    ret = serial_terminal_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Serial terminal unavailable: %s", esp_err_to_name(ret));
    }

    ret = display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    /** Start the factory boot video after display initialization. */
    const esp_err_t boot_video_ret = desktop_boot_video_start();
    if (boot_video_ret != ESP_OK) {
        ESP_LOGW(TAG, "Boot video not started: %s", esp_err_to_name(boot_video_ret));
        /** Restore panel brightness when no boot video is available. */
        ret = display_backlight_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Backlight level not applied: %s", esp_err_to_name(ret));
        }
    }

    uint32_t lock_wait_ms = 0U;
    if (ui_acquire_display_lock(&lock_wait_ms)) {
        const int64_t build_start_us = esp_timer_get_time();
        desktop_boot_video_yield();
        desktop_font_init();
        ret = assets_storage_init_lvgl_decoder();
        if (ret != ESP_OK) {
#if CONFIG_FACTORY_DESKTOP_ICONS_EMBEDDED
            ESP_LOGW(TAG, "PNG decoder unavailable; SPIFFS images may be missing: %s",
                     esp_err_to_name(ret));
#else
            ESP_LOGW(TAG, "PNG icon decoder unavailable; using fallback app icons: %s",
                     esp_err_to_name(ret));
#endif
        }
        /** Install aligned image-buffer allocation after decoder initialization. */
        display_image_buffers_init();
        desktop_language_init(desktop_font_has_chinese());
        desktop_boot_video_yield();
        desktop_ui_create();
        ESP_LOGI(TAG, "UI ready: lock_wait=%" PRIu32 " ms build=%" PRId64 " ms",
                 lock_wait_ms, (esp_timer_get_time() - build_start_us) / 1000);
        lvgl_display_unlock();
    } else {
        ESP_LOGE(TAG, "Timed out waiting for the LVGL display lock after %" PRIu32
                      " ms; UI not created", lock_wait_ms);
    }

    /** Finish boot playback and reveal the constructed desktop. */
    desktop_boot_video_finish(DESKTOP_BOOT_VIDEO_TIMEOUT_MS);

    /** Initialize Wi-Fi after desktop construction and before Bluetooth restoration. */
    memory_monitor_report("before_wifi");
    ret = wifi_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi service unavailable: %s", esp_err_to_name(ret));
    }
    /** Connect Wi-Fi link notifications to the desktop indicator. */
    desktop_ui_register_wifi_indicator();

    /** Restore the saved Bluetooth power state. */
    music_player_service_restore_bt_power();
    memory_monitor_report("after_bt_power_restore");

    /** Select the weather language before the first fetch. */
    weather_service_set_language(desktop_language_current() == DESKTOP_LANGUAGE_CHINESE);
    ret = weather_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Weather service unavailable: %s", esp_err_to_name(ret));
    }
    desktop_ui_register_weather_widget();
    ret = settings_server_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Settings page unavailable: %s", esp_err_to_name(ret));
    }
    memory_monitor_report("after_wifi");
}
