/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

/** Install the esp_lv_decoder 0.5.0 file-lifetime workaround under the LVGL lock. */
bool assets_decoder_compat_install(lv_image_decoder_t *decoder);
