/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_analog.h"
#include "app_clock_catalog.h"
#include "app_clock_face.h"
#include "app_clock_prefs.h"
#include "desktop_font.h"
#include "desktop_language.h"
#include "desktop_fullscreen_layout.h"
#include "image/lv_image_decoder_private.h"
#include "esp_log.h"
#include <stdio.h>
#include <inttypes.h>
#include "misc/cache/instance/lv_image_cache.h"
#include <stdlib.h>
#include <string.h>

#define TAG "ClockAnalog"
#define L(key) desktop_language_lookup(key)
#define EMPTY_INDEX SIZE_MAX

typedef struct {
    size_t index;
    unsigned next_part;
    bool ready;
    lv_image_decoder_dsc_t decoder[CLOCK_FACE_PARTS];
    char path[CLOCK_FACE_PARTS][CLOCK_FACE_PATH_MAX + 2];
    bool opened[CLOCK_FACE_PARTS];
    lv_image_dsc_t image[CLOCK_FACE_PARTS];
} face_slot_t;

typedef struct {
    clock_face_catalog_t catalog;
    face_slot_t slots[3];
    lv_obj_t *parent, *layer, *hands[3], *modal, *loading;
    lv_timer_t *loader;
    size_t current, wanted;
    int direction;
    bool sleep, covered, valid, persist;
    uint8_t hour, minute, second;
    char sd_root[64];
    void (*toast)(const char *);
} analog_state_t;

static analog_state_t *s;
static bool s_help_shown;

/**
 * @brief Release cached image references before closing the decoders that own their buffers.
 * @param slot Slot whose cached images and decoders are released.
 */
static void slot_release(face_slot_t *slot)
{
    for (size_t i = 0; i < CLOCK_FACE_PARTS; ++i) {
        if (slot->opened[i]) lv_image_cache_drop(&slot->image[i]);
        if (slot->opened[i]) lv_image_decoder_close(&slot->decoder[i]);
    }
    memset(slot, 0, sizeof(*slot));
    slot->index = EMPTY_INDEX;
}

static face_slot_t *slot_find(size_t index)
{
    for (size_t i = 0; i < 3; ++i) if (s->slots[i].index == index) return &s->slots[i];
    return NULL;
}

/**
 * @brief Keep current and requested buffers during a switch; preload neighbors after activation.
 * @param[out] indices Array receiving up to three face indices.
 * @return Number of indices written.
 */
static size_t desired(size_t indices[3])
{
    if (s->wanted != s->current) {
        indices[0] = s->wanted;
        indices[1] = s->current;
        return 2;
    }
    return clock_face_window(&s->catalog, s->current, s->sleep, indices);
}

static void reconcile(void)
{
    size_t indices[3];
    size_t count = desired(indices);
    for (size_t i = 0; i < 3; ++i) {
        bool keep = false;
        for (size_t j = 0; j < count; ++j) keep |= s->slots[i].index == indices[j];
        if (!keep) slot_release(&s->slots[i]);
    }
    for (size_t j = 0; j < count; ++j) {
        if (!indices[j] || slot_find(indices[j])) continue;
        for (size_t i = 0; i < 3; ++i) {
            if (s->slots[i].index == EMPTY_INDEX) {
                s->slots[i].index = indices[j];
                break;
            }
        }
    }
}

static void loading_hide(void)
{
    if (s->loading) { lv_obj_delete(s->loading); s->loading = NULL; }
}

static void help_close(lv_event_t *event)
{
    (void)event;
    if (s && s->modal) { lv_obj_delete(s->modal); s->modal = NULL; }
}

static void maybe_help(void)
{
    if (s->sleep || s->covered || s_help_shown || s->wanted != s->current) return;
    size_t last_builtin = 0;
    for (size_t i = 1; i < s->catalog.count; ++i) {
        if (s->catalog.faces[i].failed) continue;
        if (s->catalog.faces[i].sd) return;
        last_builtin = i;
    }
    if (s->current != last_builtin) return;
    s->modal = lv_obj_create(lv_obj_get_parent(s->parent));
    if (!s->modal) return;
    s_help_shown = true;
    desktop_fullscreen_layout_style_panel(s->modal, lv_color_hex(0x0E1420), 0);
    lv_obj_set_size(s->modal, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s->modal, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *panel = lv_obj_create(s->modal);
    desktop_fullscreen_layout_style_panel(panel, lv_color_hex(0x151C29), 14);
    lv_obj_set_size(panel, LV_PCT(90), LV_PCT(88));
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_t *text = lv_label_create(panel);
    char message[1200];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_CLOCK_ADD_BODY), s->sd_root + 7);
    lv_label_set_text(text, message);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_style_text_font(text, desktop_font_get_primary(), 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0xE6EEFA), 0);
    lv_obj_t *button = lv_button_create(s->modal);
    lv_obj_set_size(button, 140, 48);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x147EFB), 0);
    lv_obj_add_event_cb(button, help_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, desktop_font_get_primary(), 0);
    lv_label_set_text(label, L(DESKTOP_TEXT_CLOCK_STYLE_DONE));
    lv_obj_center(label);
}

static void set_angles(void)
{
    int32_t angles[] = {
        s->valid ? app_clock_face_hour_angle(s->hour, s->minute) : 0,
        s->valid ? app_clock_face_minute_angle(s->minute, s->second) : 0,
        s->valid ? app_clock_face_second_angle(s->second) : 0,
    };
    for (size_t i = 0; i < 3; ++i) if (s->hands[i]) lv_image_set_rotation(s->hands[i], angles[i]);
}

/**
 * @brief Create replacement widgets from decoded buffers before removing the visible face.
 * @param index Catalog index of the face to activate.
 * @return true if the requested face is active; false if it is not ready or widgets cannot be created.
 */
static bool activate(size_t index)
{
    face_slot_t *slot = index ? slot_find(index) : NULL;
    if (index && (!slot || !slot->ready)) return false;
    if (index == s->current) {
        s->wanted = index;
        s->persist = false;
        loading_hide();
        maybe_help();
        return true;
    }
    lv_obj_t *layer = NULL, *hands[3] = {NULL};
    if (index) {
        const clock_face_spec_t *face = &s->catalog.faces[index];
        layer = lv_obj_create(s->parent);
        if (!layer) return false;
        lv_obj_remove_style_all(layer);
        lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        for (size_t i = 0; i < CLOCK_FACE_PARTS; ++i) {
            if (!face->part[i].path[0]) continue;
            lv_obj_t *image = lv_image_create(layer);
            if (!image) { lv_obj_delete(layer); return false; }
            lv_image_set_src(image, &slot->image[i]);
            lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            const clock_face_part_t *part = &face->part[i];
            if (i == CLOCK_PART_DIAL) {
                lv_obj_set_pos(image, face->dial_x, face->dial_y);
            } else {
                lv_obj_set_pos(image, face->center_x - part->pivot_x, face->center_y - part->pivot_y);
                lv_image_set_pivot(image, part->pivot_x, part->pivot_y);
                lv_image_set_antialias(image, true);
                if (i <= CLOCK_PART_SECOND) hands[i - 1] = image;
            }
        }
    }
    if (s->layer) lv_obj_delete(s->layer);
    s->layer = layer;
    memcpy(s->hands, hands, sizeof(hands));
    s->current = index;
    s->wanted = index;
    set_angles();
    loading_hide();
    lv_obj_invalidate(s->parent);
    if (s->persist) {
        if (app_clock_prefs_save_face(s->catalog.faces[index].id) != ESP_OK && s->toast)
            s->toast(L(DESKTOP_TEXT_CLOCK_FACE_SAVE_ERROR));
        s->persist = false;
    }
    ESP_LOGI(TAG, "show %s (decoded resident=%d)", s->catalog.faces[index].id, slot != NULL);
    maybe_help();
    return true;
}

static void failed(size_t index)
{
    ESP_LOGW(TAG, "unusable face %s; skipped until next discovery", s->catalog.faces[index].id);
    s->catalog.faces[index].failed = true;
    face_slot_t *slot = slot_find(index);
    if (slot) slot_release(slot);
    if (!s->sleep && s->toast) s->toast(L(DESKTOP_TEXT_CLOCK_FACE_LOAD_ERROR));
    if (s->wanted == index) {
        s->wanted = s->sleep || !s->persist ? 0 :
                    clock_face_neighbor(&s->catalog, index, s->direction);
    }
}

/**
 * @brief Decode at most one image per loader tick into one of three owned face slots.
 * @param slot Slot to advance by one image.
 * @return false if decoding fails or dimensions do not match; true otherwise.
 */
static bool load_one(face_slot_t *slot)
{
    const clock_face_spec_t *face = &s->catalog.faces[slot->index];
    while (slot->next_part < CLOCK_FACE_PARTS) {
        unsigned i = slot->next_part++;
        const clock_face_part_t *part = &face->part[i];
        if (!part->path[0]) continue;
        snprintf(slot->path[i], sizeof(slot->path[i]), "A:%s", part->path);
        lv_image_decoder_args_t args = {.no_cache = true, .stride_align = true};
        uint32_t started = lv_tick_get();
        if (lv_image_decoder_open(&slot->decoder[i], slot->path[i], &args) != LV_RESULT_OK) return false;
        slot->opened[i] = true;
        const lv_draw_buf_t *decoded = slot->decoder[i].decoded;
        if (!decoded || (int32_t)decoded->header.w != part->width ||
            (int32_t)decoded->header.h != part->height) return false;
        lv_draw_buf_to_image(decoded, &slot->image[i]);

        /** Image descriptors borrow decoder storage and must not carry draw-buffer ownership. */
        slot->image[i].header.flags &= ~LV_IMAGE_FLAGS_ALLOCATED;
        ESP_LOGI(TAG, "decode %s part=%u bytes=%" PRIu32 " ms=%" PRIu32,
                 face->id, i, decoded->data_size, lv_tick_elaps(started));
        break;
    }
    if (slot->next_part == CLOCK_FACE_PARTS) slot->ready = true;
    return true;
}

static void loader_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s || s->covered) return;
    if (s->wanted == s->current && s->loading) {
        loading_hide();
        s->persist = false;
    }
    reconcile();
    face_slot_t *target = slot_find(s->wanted);
    if (s->wanted != s->current && (!s->wanted || (target && target->ready))) {
        if (!activate(s->wanted)) failed(s->wanted);
        reconcile();
    }
    size_t indices[3];
    size_t count = desired(indices);
    for (size_t j = 0; j < count; ++j) {
        face_slot_t *slot = slot_find(indices[j]);
        if (slot && !slot->ready) {
            size_t index = slot->index;
            if (!load_one(slot)) failed(index);
            return;
        }
    }
    maybe_help();
    lv_timer_pause(s->loader);
}

void app_clock_analog_start(lv_obj_t *parent, int32_t width, int32_t height,
                             bool sleep, void (*toast)(const char *))
{
    app_clock_analog_destroy();
    s = calloc(1, sizeof(*s));
    if (!s) { if (toast) toast(L(DESKTOP_TEXT_CLOCK_FACE_LOAD_ERROR)); return; }
    s->parent = parent; s->sleep = sleep; s->toast = toast; s->direction = 1;
    for (size_t i = 0; i < 3; ++i) s->slots[i].index = EMPTY_INDEX;
    snprintf(s->sd_root, sizeof(s->sd_root), "/sdcard/clock/%ldx%ld", (long)width, (long)height);
    clock_face_catalog_scan(&s->catalog, "/assets/clock/a01", s->sd_root, width, height);
    char id[CLOCK_FACE_ID_MAX];
    if (app_clock_prefs_load_face(id, sizeof(id)) == ESP_OK) {
        for (size_t i = 0; i < s->catalog.count; ++i)
            if (!strcmp(id, s->catalog.faces[i].id)) s->wanted = i;
    }
    if (sleep) {
        reconcile();
        face_slot_t *slot = slot_find(s->wanted);
        while (slot && !slot->ready) {
            if (!load_one(slot)) { failed(s->wanted); break; }
        }
        (void)activate(s->wanted);
        return;
    }
    s->loader = lv_timer_create(loader_cb, 30, NULL);
    if (!s->loader) { app_clock_analog_destroy(); if (toast) toast(L(DESKTOP_TEXT_CLOCK_FACE_LOAD_ERROR)); }
}

void app_clock_analog_step(int direction)
{
    if (!s || s->sleep || s->covered || s->modal) return;
    s->direction = direction < 0 ? -1 : 1;
    s->wanted = clock_face_neighbor(&s->catalog, s->wanted, s->direction);
    s->persist = true;
    face_slot_t *slot = slot_find(s->wanted);
    if (!s->wanted || (slot && slot->ready)) {
        if (!activate(s->wanted)) failed(s->wanted);
    } else if (!s->loading) {
        s->loading = lv_label_create(lv_obj_get_parent(s->parent));
        lv_obj_set_style_text_font(s->loading, desktop_font_get_primary(), 0);
        lv_obj_set_style_text_color(s->loading, lv_color_hex(0xE6EEFA), 0);
        lv_label_set_text(s->loading, L(DESKTOP_TEXT_CLOCK_FACE_LOADING));
        lv_obj_align(s->loading, LV_ALIGN_TOP_MID, 0, 12);
    }
    lv_timer_resume(s->loader);
}

void app_clock_analog_time(bool valid, uint8_t hour, uint8_t minute, uint8_t second)
{
    if (!s) return;
    s->valid = valid; s->hour = hour; s->minute = minute; s->second = second;
    set_angles();
}

bool app_clock_analog_is_image(void) { return s && s->current != 0; }
bool app_clock_analog_is_sd(void) { return s && s->catalog.faces[s->current].sd; }
bool app_clock_analog_has_modal(void) { return s && s->modal; }
void app_clock_analog_new_session(void) { s_help_shown = false; }

void app_clock_analog_covered(bool covered)
{
    if (!s) return;
    s->covered = covered;
    if (s->loader) {
        if (covered) lv_timer_pause(s->loader);
        else lv_timer_resume(s->loader);
    }
}

/**
 * @brief Remove widgets before releasing their image buffers and decoder state.
 */
void app_clock_analog_destroy(void)
{
    if (!s) return;
    if (s->loader) lv_timer_delete(s->loader);
    if (s->modal) lv_obj_delete(s->modal);
    loading_hide();
    if (s->layer) lv_obj_delete(s->layer);
    for (size_t i = 0; i < 3; ++i) slot_release(&s->slots[i]);
    free(s);
    s = NULL;
}
