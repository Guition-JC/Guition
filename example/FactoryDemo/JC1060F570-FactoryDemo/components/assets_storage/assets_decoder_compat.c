/** SPDX-License-Identifier: GPL-2.0-only */
#include "assets_decoder_compat.h"

#include "image/lv_image_decoder_private.h"

/* This workaround is scoped to the LVGL version used by this project. Review
 * the upstream decoder's file ownership before enabling it on a new version. */
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6
static lv_image_decoder_open_f_t s_original_open;

static lv_result_t decoder_open_with_file(lv_image_decoder_t *decoder,
                                           lv_image_decoder_dsc_t *dsc)
{
    if (dsc->src_type != LV_IMAGE_SRC_FILE) {
        return s_original_open(decoder, dsc);
    }

    /* LVGL 9.6 closes the info callback's file before open_cb, and a header
     * cache hit never opens it. esp_lv_decoder 0.5.0 nevertheless uses this
     * field for PJPG/SP magic detection. Its actual loaders and split-image
     * sessions own separate files, so this handle is needed only in open_cb. */
    if (lv_fs_open(&dsc->file, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        dsc->error_msg = "Image file open failed";
        return LV_RESULT_INVALID;
    }

    const lv_result_t result = s_original_open(decoder, dsc);
    lv_fs_close(&dsc->file);
    return result;
}
#endif

bool assets_decoder_compat_install(lv_image_decoder_t *decoder)
{
    if (decoder == NULL || decoder->open_cb == NULL) {
        return false;
    }
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6
    if (decoder->open_cb == decoder_open_with_file) {
        return true;
    }
    /* Several instances may share the same upstream callback. Do not wrap
     * an unrelated decoder with the saved callback. */
    if (s_original_open != NULL && s_original_open != decoder->open_cb) {
        return false;
    }
    s_original_open = decoder->open_cb;
    lv_image_decoder_set_open_cb(decoder, decoder_open_with_file);
    return true;
#else
    return false;
#endif
}
