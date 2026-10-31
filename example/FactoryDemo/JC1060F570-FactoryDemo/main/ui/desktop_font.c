#include "desktop_font.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mmap_assets.h"
#include "include/lvgl/fs/lv_fs.h"
#include "mmap_generate_fonts.h"
#include "nvs.h"

/** @brief Read the bundled font from its memory-mapped flash partition. */
#define DESKTOP_FONT_BUNDLED_PARTITION "fonts"

/** @brief Expose SD fonts through a dedicated read-only LVGL drive. */
#define DESKTOP_FONT_SD_DIR       "/sdcard/fonts"
#define DESKTOP_FONT_SD_ROOT      "/sdcard"
#define DESKTOP_FONT_SD_LV_PREFIX "S:/fonts/"
#define DESKTOP_FONT_DRIVE_LETTER 'S'
#define DESKTOP_FONT_FS_PATH_MAX  256U
/** @brief Maximum number of candidates collected during one directory scan. */
#define DESKTOP_FONT_SD_SCAN_MAX  16U

#define DESKTOP_FONT_PRIMARY_SIZE    14U
#define DESKTOP_FONT_TITLE_SIZE      18U

#define DESKTOP_FONT_LYRIC_SIZE      32U

#define DESKTOP_FONT_ARTIST_SIZE     24U

#define DESKTOP_FONT_CLOCK_FACE_SIZE 100U

#define DESKTOP_FONT_NVS_NAMESPACE  "pados_font"
#define DESKTOP_FONT_NVS_KEY_FILE   "file"
#define DESKTOP_FONT_NVS_KEY_NOTICE "notice"
#define DESKTOP_FONT_NOTICE_HIDDEN  1U

static const char *TAG = "desktop_font";

typedef enum {
    DESKTOP_FONT_ROLE_PRIMARY = 0,
    DESKTOP_FONT_ROLE_TITLE,
    DESKTOP_FONT_ROLE_LYRIC,
    DESKTOP_FONT_ROLE_ARTIST,
    DESKTOP_FONT_ROLE_CLOCK_FACE,
    DESKTOP_FONT_ROLE_COUNT,
} desktop_font_role_t;

typedef struct {
    uint32_t size;
    const lv_font_t *builtin;
} desktop_font_role_desc_t;

/** @brief Map each font role to its size and built-in glyph fallback. */
static const desktop_font_role_desc_t s_roles[DESKTOP_FONT_ROLE_COUNT] = {
    { DESKTOP_FONT_PRIMARY_SIZE,    &lv_font_montserrat_14 },
    { DESKTOP_FONT_TITLE_SIZE,      &lv_font_montserrat_18 },
    { DESKTOP_FONT_LYRIC_SIZE,      &lv_font_montserrat_32 },
    { DESKTOP_FONT_ARTIST_SIZE,     &lv_font_montserrat_24 },
    { DESKTOP_FONT_CLOCK_FACE_SIZE, &lv_font_montserrat_48 },
};

/** @brief Keep bundled faces for the process lifetime and retired faces until widgets are rebound. */
typedef struct {
    lv_font_t *faces[DESKTOP_FONT_ROLE_COUNT];
    bool live;
} desktop_font_pool_t;

static desktop_font_pool_t s_bundled;
static desktop_font_pool_t s_sd;
static desktop_font_pool_t s_retired;
static bool s_use_sd;
static bool s_sd_drive_ready;
static lv_fs_drv_t s_sd_font_drv;
static char s_selected[DESKTOP_FONT_NAME_MAX];
/** @brief Keep the mapped font handle and path alive while its faces are in use. */
static mmap_assets_handle_t s_bundled_assets;
static lv_fs_path_ex_t s_bundled_path;
static bool s_bundled_path_ready;

/**
 * @brief Map the bundled font and create the LVGL memory-backed path.
 * @return true when the bundled font path is available.
 */
static bool bundled_path_ready(void)
{
    if(s_bundled_path_ready) {
        return true;
    }

    const mmap_assets_config_t config = {
        .partition_label = DESKTOP_FONT_BUNDLED_PARTITION,
        .max_files = MMAP_FONTS_FILES,
        .checksum = MMAP_FONTS_CHECKSUM,
        .flags = {
            .mmap_enable = true,
        },
    };
    if(mmap_assets_new(&config, &s_bundled_assets) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot map the '%s' font partition; using the built-in faces",
                 DESKTOP_FONT_BUNDLED_PARTITION);
        return false;
    }

    const void *memory = mmap_assets_get_mem(s_bundled_assets, MMAP_FONTS_DESKTOP_SANS_SC_LIGHT_TTF);
    const int size = mmap_assets_get_size(s_bundled_assets, MMAP_FONTS_DESKTOP_SANS_SC_LIGHT_TTF);
    if(memory == NULL || size <= 0) {
        ESP_LOGE(TAG, "The '%s' partition holds no usable font", DESKTOP_FONT_BUNDLED_PARTITION);
        return false;
    }

    lv_fs_make_path_from_buffer(&s_bundled_path, LV_FS_MEMFS_LETTER, memory, (uint32_t)size,
                                "ttf");
    s_bundled_path_ready = true;
    return true;
}

static bool sd_font_path_is_safe(const char *path)
{
    if(path == NULL || path[0] != '/' || path[1] == '\0') {
        return false;
    }

    for(const char *cursor = path; *cursor != '\0'; cursor++) {
        if(*cursor == '\\' || *cursor == ':' || (cursor[0] == '/' && cursor[1] == '/')) {
            return false;
        }
        if(cursor[0] == '/' && cursor[1] == '.' && cursor[2] == '.' &&
           (cursor[3] == '\0' || cursor[3] == '/')) {
            return false;
        }
    }

    return true;
}

static void *sd_font_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    LV_UNUSED(drv);
    if(mode != LV_FS_MODE_RD || !sd_font_path_is_safe(path)) {
        return NULL;
    }

    char full_path[DESKTOP_FONT_FS_PATH_MAX];
    int written = snprintf(full_path, sizeof(full_path), "%s%s", DESKTOP_FONT_SD_ROOT, path);
    if(written < 0 || (size_t)written >= sizeof(full_path)) {
        return NULL;
    }

    return fopen(full_path, "rb");
}

static lv_fs_res_t sd_font_close(lv_fs_drv_t *drv, void *file_p)
{
    LV_UNUSED(drv);
    if(file_p == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_font_read(lv_fs_drv_t *drv, void *file_p, void *buf,
                                uint32_t bytes_to_read, uint32_t *bytes_read)
{
    LV_UNUSED(drv);
    if(file_p == NULL || buf == NULL || bytes_read == NULL) {
        return LV_FS_RES_INV_PARAM;
    }

    *bytes_read = (uint32_t)fread(buf, 1, bytes_to_read, (FILE *)file_p);
    return ferror((FILE *)file_p) != 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

static lv_fs_res_t sd_font_write(lv_fs_drv_t *drv, void *file_p, const void *buf,
                                 uint32_t bytes_to_write, uint32_t *bytes_written)
{
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    LV_UNUSED(buf);
    LV_UNUSED(bytes_to_write);
    if(bytes_written != NULL) {
        *bytes_written = 0;
    }
    return LV_FS_RES_DENIED;
}

static lv_fs_res_t sd_font_seek(lv_fs_drv_t *drv, void *file_p, uint32_t position,
                                lv_fs_whence_t whence)
{
    LV_UNUSED(drv);
    if(file_p == NULL || (uint64_t)position > (uint64_t)LONG_MAX) {
        return LV_FS_RES_INV_PARAM;
    }

    int origin;
    switch(whence) {
        case LV_FS_SEEK_SET:
            origin = SEEK_SET;
            break;
        case LV_FS_SEEK_CUR:
            origin = SEEK_CUR;
            break;
        case LV_FS_SEEK_END:
            origin = SEEK_END;
            break;
        default:
            return LV_FS_RES_INV_PARAM;
    }

    return fseek((FILE *)file_p, (long)position, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_font_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *position)
{
    LV_UNUSED(drv);
    if(file_p == NULL || position == NULL) {
        return LV_FS_RES_INV_PARAM;
    }

    long current = ftell((FILE *)file_p);
    if(current < 0 || (uint64_t)current > UINT32_MAX) {
        return LV_FS_RES_UNKNOWN;
    }
    *position = (uint32_t)current;
    return LV_FS_RES_OK;
}

static bool sd_font_ready(lv_fs_drv_t *drv)
{
    LV_UNUSED(drv);
    return s_sd_drive_ready;
}

static bool register_sd_font_drive(void)
{
    lv_fs_drv_t *existing = lv_fs_get_drv(DESKTOP_FONT_DRIVE_LETTER);
    if(existing != NULL && existing != &s_sd_font_drv) {
        return false;
    }
    if(existing == &s_sd_font_drv) {
        s_sd_drive_ready = true;
        return true;
    }

    lv_fs_drv_init(&s_sd_font_drv);
    s_sd_font_drv.letter = DESKTOP_FONT_DRIVE_LETTER;
    s_sd_font_drv.ready_cb = sd_font_ready;
    s_sd_font_drv.open_cb = sd_font_open;
    s_sd_font_drv.close_cb = sd_font_close;
    s_sd_font_drv.read_cb = sd_font_read;
    s_sd_font_drv.write_cb = sd_font_write;
    s_sd_font_drv.seek_cb = sd_font_seek;
    s_sd_font_drv.tell_cb = sd_font_tell;
    lv_fs_drv_register(&s_sd_font_drv);

    s_sd_drive_ready = true;
    return lv_fs_get_drv(DESKTOP_FONT_DRIVE_LETTER) == &s_sd_font_drv;
}

static void disable_sd_font_drive(void)
{
    s_sd_drive_ready = false;
}

static void pool_destroy(desktop_font_pool_t *pool)
{
    for(size_t i = 0; i < DESKTOP_FONT_ROLE_COUNT; ++i) {
        if(pool->faces[i] != NULL) {
            lv_freetype_font_delete(pool->faces[i]);
            pool->faces[i] = NULL;
        }
    }
    pool->live = false;
}

/**
 * @brief Retain the previous font pool until the desktop has rebuilt its widgets.
 * @param pool Font pool to retain until widgets are rebound.
 */
static void pool_retire(desktop_font_pool_t *pool)
{
    pool_destroy(&s_retired);
    s_retired = *pool;
    memset(pool, 0, sizeof(*pool));
}

static bool pool_create(desktop_font_pool_t *pool, const char *lv_path)
{
    for(size_t i = 0; i < DESKTOP_FONT_ROLE_COUNT; ++i) {
        lv_font_t *face = lv_freetype_font_create(lv_path,
                                                  LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                  s_roles[i].size,
                                                  LV_FREETYPE_FONT_STYLE_NORMAL);
        if(face == NULL) {
            ESP_LOGE(TAG, "FreeType could not open %s at %u px", lv_path,
                     (unsigned)s_roles[i].size);
            pool_destroy(pool);
            return false;
        }
        /** Use the built-in face for glyphs absent from the selected font. */
        face->fallback = s_roles[i].builtin;
        pool->faces[i] = face;
    }
    pool->live = true;
    return true;
}

static const lv_font_t *active_face(desktop_font_role_t role)
{
    if(s_use_sd && s_sd.faces[role] != NULL) {
        return s_sd.faces[role];
    }
    if(s_bundled.faces[role] != NULL) {
        return s_bundled.faces[role];
    }
    return s_roles[role].builtin;
}

static esp_err_t preference_read(char *name, size_t size)
{
    if(name == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    name[0] = '\0';

    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(DESKTOP_FONT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if(opened != ESP_OK) {
        return opened;
    }
    size_t length = size;
    const esp_err_t ret = nvs_get_str(handle, DESKTOP_FONT_NVS_KEY_FILE, name, &length);
    nvs_close(handle);
    if(ret != ESP_OK) {
        name[0] = '\0';
    }
    return ret;
}

static esp_err_t preference_write(const char *name)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(DESKTOP_FONT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", DESKTOP_FONT_NVS_NAMESPACE,
                 esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_str(handle, DESKTOP_FONT_NVS_KEY_FILE, name != NULL ? name : "");
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "storing font '%s' failed: %s", name != NULL ? name : "",
                 esp_err_to_name(ret));
    }
    return ret;
}

static bool ends_with_ttf(const char *name)
{
    const size_t length = strlen(name);
    if(length < 4U) {
        return false;
    }
    const char *suffix = name + length - 4U;
    return suffix[0] == '.' && (suffix[1] == 't' || suffix[1] == 'T') &&
           (suffix[2] == 't' || suffix[2] == 'T') && (suffix[3] == 'f' || suffix[3] == 'F');
}

/**
 * @brief Accept only file names within the SD fonts directory.
 * @param name Font file name to validate.
 * @return true if the name is accepted.
 */
static bool sd_name_is_valid(const char *name)
{
    if(name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    const size_t length = strlen(name);
    if(length >= DESKTOP_FONT_NAME_MAX) {
        return false;
    }
    for(const char *cursor = name; *cursor != '\0'; ++cursor) {
        const unsigned char c = (unsigned char)*cursor;
        if(c < 0x20U || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return ends_with_ttf(name);
}

static bool sd_lv_path(const char *name, char *out, size_t size)
{
    if(!sd_name_is_valid(name)) {
        return false;
    }
    const int written = snprintf(out, size, "%s%s", DESKTOP_FONT_SD_LV_PREFIX, name);
    return written > 0 && (size_t)written < size;
}

/**
 * @brief Activate the selected SD font, falling back to bundled faces when loading fails.
 * @param name SD font file name.
 * @return true when the SD font is activated.
 */
static bool activate_sd(const char *name)
{
    char lv_path[DESKTOP_FONT_FS_PATH_MAX];
    if(!sd_lv_path(name, lv_path, sizeof(lv_path)) || !register_sd_font_drive()) {
        ESP_LOGW(TAG, "font '%s' is not usable; using the bundled face", name);
        pool_retire(&s_sd);
        s_use_sd = false;
        return false;
    }

    desktop_font_pool_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    if(!pool_create(&loaded, lv_path)) {
        pool_retire(&s_sd);
        s_use_sd = false;
        return false;
    }

    pool_retire(&s_sd);
    s_sd = loaded;
    s_use_sd = true;
    ESP_LOGI(TAG, "Loaded %s at %u/%u/%u/%u px", lv_path, DESKTOP_FONT_PRIMARY_SIZE,
             DESKTOP_FONT_TITLE_SIZE, DESKTOP_FONT_LYRIC_SIZE, DESKTOP_FONT_ARTIST_SIZE);
    return true;
}

void desktop_font_init(void)
{
    if(!s_bundled.live) {
        if(bundled_path_ready() && !pool_create(&s_bundled, (const char *)&s_bundled_path)) {
            ESP_LOGW(TAG, "bundled face unusable; keeping the built-in faces");
        }
    }

    char stored[DESKTOP_FONT_NAME_MAX];
    const bool has_preference = preference_read(stored, sizeof(stored)) == ESP_OK &&
                                stored[0] != '\0';
    snprintf(s_selected, sizeof(s_selected), "%s", has_preference ? stored : "");
    if(has_preference) {
        (void)activate_sd(stored);
    }
    else {
        pool_retire(&s_sd);
        s_use_sd = false;
        if(s_bundled.live) {
            ESP_LOGI(TAG, "Using the bundled font at %u/%u/%u/%u px", DESKTOP_FONT_PRIMARY_SIZE,
                     DESKTOP_FONT_TITLE_SIZE, DESKTOP_FONT_LYRIC_SIZE, DESKTOP_FONT_ARTIST_SIZE);
        }
        else {
            ESP_LOGW(TAG, "No font file is available; using the built-in faces");
        }
    }
}

void desktop_font_prepare_for_sd_unmount(void)
{
    /** Switch getters to bundled faces before releasing SD resources. */
    s_use_sd = false;
}

esp_err_t desktop_font_release_sd_resources(void)
{
    desktop_font_prepare_for_sd_unmount();
    /** Release SD faces after all widgets have been rebound. */
    pool_destroy(&s_sd);
    pool_destroy(&s_retired);
    disable_sd_font_drive();

    return ESP_OK;
}

esp_err_t desktop_font_select_sd(const char *file_name)
{
    const bool wants_sd = file_name != NULL && file_name[0] != '\0';
    if(wants_sd && !sd_name_is_valid(file_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t stored = preference_write(wants_sd ? file_name : "");
    if(stored != ESP_OK) {
        return stored;
    }
    snprintf(s_selected, sizeof(s_selected), "%s", wants_sd ? file_name : "");

    if(!wants_sd) {
        pool_retire(&s_sd);
        s_use_sd = false;
        return ESP_OK;
    }
    /** Keep the selected name in NVS so unavailable SD fonts can be retried at boot. */
    return activate_sd(file_name) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static int compare_names(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

size_t desktop_font_scan_sd(char (*names)[DESKTOP_FONT_NAME_MAX], size_t max)
{
    if(names == NULL || max == 0) {
        return 0;
    }

    DIR *directory = opendir(DESKTOP_FONT_SD_DIR);
    if(directory == NULL) {
        return 0;
    }

    char found[DESKTOP_FONT_SD_SCAN_MAX][DESKTOP_FONT_NAME_MAX];
    size_t count = 0;
    struct dirent *entry = NULL;
    while(count < DESKTOP_FONT_SD_SCAN_MAX && (entry = readdir(directory)) != NULL) {
        if(!sd_name_is_valid(entry->d_name)) {
            continue;
        }

        snprintf(found[count], DESKTOP_FONT_NAME_MAX, "%.*s", (int)(DESKTOP_FONT_NAME_MAX - 1U),
                 entry->d_name);
        ++count;
    }
    closedir(directory);

    qsort(found, count, DESKTOP_FONT_NAME_MAX, compare_names);
    const size_t kept = count < max ? count : max;
    memcpy(names, found, kept * DESKTOP_FONT_NAME_MAX);
    return kept;
}

const char *desktop_font_selected_sd(void)
{
    return s_selected[0] != '\0' ? s_selected : NULL;
}

void desktop_font_release_retired(void)
{
    pool_destroy(&s_retired);
}

bool desktop_font_boot_notice_wanted(void)
{
    nvs_handle_t handle = 0;
    if(nvs_open(DESKTOP_FONT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return true;
    }
    uint8_t hidden = 0U;
    const esp_err_t ret = nvs_get_u8(handle, DESKTOP_FONT_NVS_KEY_NOTICE, &hidden);
    nvs_close(handle);
    return ret != ESP_OK || hidden != DESKTOP_FONT_NOTICE_HIDDEN;
}

esp_err_t desktop_font_disable_boot_notice(void)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(DESKTOP_FONT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, DESKTOP_FONT_NVS_KEY_NOTICE, DESKTOP_FONT_NOTICE_HIDDEN);
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

const lv_font_t *desktop_font_get_primary(void)
{
    return active_face(DESKTOP_FONT_ROLE_PRIMARY);
}

const lv_font_t *desktop_font_get_title(void)
{
    return active_face(DESKTOP_FONT_ROLE_TITLE);
}

const lv_font_t *desktop_font_get_lyric(void)
{
    return active_face(DESKTOP_FONT_ROLE_LYRIC);
}

const lv_font_t *desktop_font_get_artist(void)
{
    return active_face(DESKTOP_FONT_ROLE_ARTIST);
}

const lv_font_t *desktop_font_get_clock_face(void)
{
    return active_face(DESKTOP_FONT_ROLE_CLOCK_FACE);
}

bool desktop_font_has_chinese(void)
{
    return s_use_sd || s_bundled.live;
}

bool desktop_font_using_sd_font(void)
{
    return s_use_sd || s_bundled.live;
}
