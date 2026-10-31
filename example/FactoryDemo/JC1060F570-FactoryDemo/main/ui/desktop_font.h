#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

/** @brief Maximum buffer size for a stored SD font file name. */
#define DESKTOP_FONT_NAME_MAX 64U

/** @brief Maximum number of SD fonts listed in settings. */
#define DESKTOP_FONT_SD_LIST_MAX 8U

/**
 * @brief Reload font roles from NVS: selected SD font, bundled font, then built-in faces.
 */
void desktop_font_init(void);

/**
 * @brief Switch to bundled faces before SD detach; retain SD faces until widgets are rebound.
 */
void desktop_font_prepare_for_sd_unmount(void);

/**
 * @brief Release SD-backed faces after widgets stop using them.
 * @return ESP_OK after resources are released.
 */
esp_err_t desktop_font_release_sd_resources(void);

/**
 * @brief Return the active primary text font with the configured fallback.
 * @return Active primary text font.
 */
const lv_font_t *desktop_font_get_primary(void);
/**
 * @brief Return the active title font with the configured fallback.
 * @return Active title font.
 */
const lv_font_t *desktop_font_get_title(void);
/**
 * @brief Return the 32 px music title font.
 * @return Font for music titles.
 */
const lv_font_t *desktop_font_get_lyric(void);
/**
 * @brief Return the 24 px font shared by artist labels, desktop captions and the clock.
 * @return Font for artist labels, captions and the clock.
 */
const lv_font_t *desktop_font_get_artist(void);
/**
 * @brief Return the flip-clock font, with a built-in 48 px fallback.
 * @return Font for the flip clock.
 */
const lv_font_t *desktop_font_get_clock_face(void);

/**
 * @brief Report whether a bundled or SD font with Chinese glyphs is active.
 * @return true when the active font supports Chinese glyphs.
 */
bool desktop_font_has_chinese(void);
/**
 * @brief Report whether the active face is file-backed rather than a built-in bitmap.
 * @return true when the active font is file-backed.
 */
bool desktop_font_using_sd_font(void);

/**
 * @brief Persist and apply an SD font name; NULL or empty selects the bundled font. Keep unavailable names for
 * retry and use bundled faces in the meantime.
 */
esp_err_t desktop_font_select_sd(const char *file_name);

/**
 * @brief List sorted TTF file names from the SD card.
 * @param[out] names Array receiving file names.
 * @param max Number of entries available in names.
 * @return Number of names written, or zero if none are available.
 */
size_t desktop_font_scan_sd(char (*names)[DESKTOP_FONT_NAME_MAX], size_t max);

/**
 * @brief Return the stored SD font name, or NULL for the bundled font.
 * @return Stored SD font name, or NULL when the bundled font is selected.
 */
const char *desktop_font_selected_sd(void);

/**
 * @brief Release retired faces after the desktop rebuild has rebound every widget.
 */
void desktop_font_release_retired(void);

/**
 * @brief Report whether the boot-time missing-glyph notice is enabled.
 * @return true when the boot-time notice is enabled.
 */
bool desktop_font_boot_notice_wanted(void);

/**
 * @brief Persist dismissal of the boot-time font notice.
 * @return ESP_OK on success, otherwise a storage error.
 */
esp_err_t desktop_font_disable_boot_notice(void);
