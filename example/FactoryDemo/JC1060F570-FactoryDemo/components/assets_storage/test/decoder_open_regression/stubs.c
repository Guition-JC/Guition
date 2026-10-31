#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ESP_LOGW(...) ((void)0)
#define PJPG_MAGIC "_PJPG__"
#define PJPG_MAGIC_LEN 7
#define PJPG_HEADER_SIZE 22
#define LV_UNUSED(x) ((void)(x))
#define lv_strcmp strcmp
#define LV_RESULT_OK 0
#define LV_RESULT_INVALID 1
#define LV_FS_RES_OK 0
#define LV_FS_RES_UNKNOWN 1
#define LV_FS_RES_NOT_EX 2
#define LV_FS_MODE_RD 1
#define LV_FS_SEEK_SET 0
#define LV_FS_SEEK_CUR 1
#define LV_FS_SEEK_END 2
#define LV_IMAGE_SRC_FILE 1
#define LV_IMAGE_SRC_VARIABLE 2
#define image_cache_draw_buf_handlers NULL
#define LV_LOG_WARN(...) ((void)0)
#define LV_LOG_ERROR(...) ((void)0)
typedef int lv_result_t;
typedef int lv_fs_res_t;
typedef struct { void *drv; void *file_d; size_t offset; } lv_fs_file_t;
typedef struct { int dummy; } lv_image_decoder_t;
typedef struct { uint32_t data_size; } lv_draw_buf_t;
typedef struct { uint32_t data_size; const uint8_t *data; } lv_image_dsc_t;
typedef struct { int src_type; const void *src; struct { uint32_t size; } slot; } lv_image_cache_data_t;
typedef struct { int dummy; } lv_cache_entry_t;
typedef struct {
    int src_type;
    const void *src;
    lv_fs_file_t file;
    struct { bool no_cache; } args;
    lv_draw_buf_t *decoded;
    lv_cache_entry_t *cache_entry;
} lv_image_decoder_dsc_t;
typedef enum { SP_IMAGE_TYPE_PNG, SP_IMAGE_TYPE_QOI, SP_IMAGE_TYPE_JPG } sp_image_type_t;
enum { DECODE_NONE, DECODE_JPEG, DECODE_PNG, DECODE_PJPG, DECODE_SP, DECODE_QOI };
static const uint8_t *file_data;
static size_t file_length;
static bool fail_open, fail_read, fail_seek, fail_decode;
static int opens, closes, active_files, bad_accesses, decode_calls, decoder_kind, probe_left_open;
static lv_draw_buf_t draw_buf = { 128 };
static const char *lv_fs_get_ext(const void *path) { const char *p = strrchr(path, '.'); return p ? p + 1 : ""; }
static lv_fs_res_t lv_fs_open(lv_fs_file_t *f, const void *path, int mode)
{
    (void)path; (void)mode;

    if (fail_open || active_files) return LV_FS_RES_NOT_EX;
    *f = (lv_fs_file_t){ .drv = (void*)1, .file_d = (void*)1 };
    opens++; active_files++;
    return LV_FS_RES_OK;
}
static lv_fs_res_t lv_fs_close(lv_fs_file_t *f)
{
    if (!f->drv || !f->file_d) { bad_accesses++; return LV_FS_RES_UNKNOWN; }
    f->drv = NULL; f->file_d = NULL; closes++; active_files--;
    return LV_FS_RES_OK;
}
static lv_fs_res_t lv_fs_seek(lv_fs_file_t *f, uint32_t pos, int mode)
{
    if (!f->drv || !f->file_d) { bad_accesses++; return LV_FS_RES_UNKNOWN; }
    if (fail_seek) return LV_FS_RES_UNKNOWN;
    f->offset = mode == LV_FS_SEEK_END ? file_length + pos : mode == LV_FS_SEEK_CUR ? f->offset + pos : pos;
    return LV_FS_RES_OK;
}
static lv_fs_res_t lv_fs_read(lv_fs_file_t *f, void *out, uint32_t count, uint32_t *read)
{
    *read = 0;
    if (!f->drv || !f->file_d) { bad_accesses++; return LV_FS_RES_UNKNOWN; }
    if (fail_read) return LV_FS_RES_UNKNOWN;
    size_t remaining = f->offset < file_length ? file_length - f->offset : 0;
    *read = remaining < count ? remaining : count;
    memcpy(out, file_data + f->offset, *read); f->offset += *read;
    return LV_FS_RES_OK;
}
static bool sp_is_sp_tag(const uint8_t *data, sp_image_type_t *type)
{
    (void)type;
    return memcmp(data, "_SJPG__", 7) == 0 || memcmp(data, "_SPNG__", 7) == 0 || memcmp(data, "_SQOI__", 7) == 0;
}
static lv_draw_buf_t *mock_decode(lv_image_decoder_dsc_t *dsc, int kind)
{
    decode_calls++; decoder_kind = kind;
    if (active_files) probe_left_open++;
    if (dsc->src_type == LV_IMAGE_SRC_FILE) {
        lv_fs_file_t own_file;
        if (lv_fs_open(&own_file, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) return NULL;
        lv_fs_close(&own_file);
    }
    return fail_decode ? NULL : &draw_buf;
}
static lv_draw_buf_t *jpeg_decode_jpg(lv_image_decoder_dsc_t *dsc) { return mock_decode(dsc, DECODE_JPEG); }
static lv_draw_buf_t *png_decode_rgba(lv_image_decoder_dsc_t *dsc) { return mock_decode(dsc, DECODE_PNG); }
static lv_draw_buf_t *pjpg_decode_jpg(lv_image_decoder_dsc_t *dsc) { return mock_decode(dsc, DECODE_PJPG); }
static lv_draw_buf_t *qoi_decode_qoi(lv_image_decoder_dsc_t *dsc) { return mock_decode(dsc, DECODE_QOI); }
static lv_result_t sp_open_session(lv_image_decoder_dsc_t *dsc) { return mock_decode(dsc, DECODE_SP) ? LV_RESULT_OK : LV_RESULT_INVALID; }
static lv_draw_buf_t *lv_image_decoder_post_process(lv_image_decoder_dsc_t *dsc, lv_draw_buf_t *buf) { (void)dsc; return buf; }
static void lv_draw_buf_destroy_user(void *handlers, lv_draw_buf_t *buf) { (void)handlers; (void)buf; }
static bool lv_image_cache_is_enabled(void) { return false; }
static lv_cache_entry_t *lv_image_decoder_add_to_cache(lv_image_decoder_t *decoder, lv_image_cache_data_t *key, lv_draw_buf_t *buf, void *user) { (void)decoder; (void)key; (void)buf; (void)user; return NULL; }
