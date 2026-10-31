/** SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "photo_catalog.h"

static void test_name_rules(void)
{
    assert(photo_catalog_name_is_safe("2.jpg"));
    assert(photo_catalog_name_is_safe("IMG_0001.JPEG"));
    assert(!photo_catalog_name_is_safe(NULL));
    assert(!photo_catalog_name_is_safe(""));
    assert(!photo_catalog_name_is_safe(".hidden.jpg"));
    assert(!photo_catalog_name_is_safe("dir/pic.jpg"));
    assert(!photo_catalog_name_is_safe("dir\\pic.jpg"));
    assert(!photo_catalog_name_is_safe("C:pic.jpg"));

    assert(photo_catalog_is_supported_name("a.jpg"));
    assert(photo_catalog_is_supported_name("a.JPEG"));
    assert(photo_catalog_is_supported_name("a.png"));
    assert(photo_catalog_is_supported_name("a.sjpg"));
    assert(photo_catalog_is_supported_name("a.spng"));
    assert(!photo_catalog_is_supported_name("a.bmp"));
    assert(!photo_catalog_is_supported_name("a.gif"));
    assert(!photo_catalog_is_supported_name("a"));
    assert(!photo_catalog_is_supported_name("a.jpg.txt"));

    assert(photo_catalog_is_sliced_name("2.sjpg"));
    assert(photo_catalog_is_sliced_name("2.spng"));
    assert(!photo_catalog_is_sliced_name("2.jpg"));
    assert(!photo_catalog_is_sliced_name("2.png"));

    assert(strcmp(photo_catalog_extension("a.jpg"), "jpg") == 0);
    assert(strcmp(photo_catalog_extension("a"), "") == 0);
    assert(strcmp(photo_catalog_extension(".jpg"), "") == 0);

    assert(photo_catalog_bytes_per_pixel("a.jpg") == PHOTO_CATALOG_JPEG_BPP);
    assert(photo_catalog_bytes_per_pixel("a.sjpg") == PHOTO_CATALOG_JPEG_BPP);
    assert(photo_catalog_bytes_per_pixel("a.png") == PHOTO_CATALOG_PNG_BPP);
    assert(photo_catalog_bytes_per_pixel("a.spng") == PHOTO_CATALOG_PNG_BPP);
}

static void test_paths(void)
{
    char posix[PHOTO_CATALOG_PATH_MAX];
    char lv[PHOTO_CATALOG_PATH_MAX];

    assert(photo_catalog_build_paths("/sdcard/image", "A:/sdcard/image", "2.jpg",
                                     posix, sizeof(posix), lv, sizeof(lv)));
    assert(strcmp(posix, "/sdcard/image/2.jpg") == 0);
    assert(strcmp(lv, "A:/sdcard/image/2.jpg") == 0);

    assert(!photo_catalog_build_paths("/sdcard/image", "A:/sdcard/image", "../2.jpg",
                                      posix, sizeof(posix), lv, sizeof(lv)));
    assert(!photo_catalog_build_paths("/sdcard/image", "A:/sdcard/image", "2.jpg",
                                      posix, 4U, lv, sizeof(lv)));
    assert(posix[0] == '\0');
}

static void test_merge_prefers_sliced_and_keeps_sources(void)
{
    const photo_dir_entry_t sd_entries[] = {
        {"a.jpg", 100U},
        {"a.sjpg", 90U},
        {"b.PNG", 200U},
        {"c.bmp", 10U},
        {"readme.txt", 5U},
    };
    const photo_dir_entry_t builtin_entries[] = {
        {"2.jpg", 740740U},
        {"2.sjpg", 100U},
        {"1.jpg", 605940U},
        {"3.jpg", 661574U},
    };
    const photo_catalog_source_t sources[] = {
        {"/sdcard/image", "A:/sdcard/image", PHOTO_SOURCE_SD, sd_entries,
         sizeof(sd_entries) / sizeof(sd_entries[0])},
        {"/assets/image", "A:/assets/image", PHOTO_SOURCE_BUILTIN, builtin_entries,
         sizeof(builtin_entries) / sizeof(builtin_entries[0])},
    };
    photo_catalog_t catalog;

    assert(photo_catalog_merge(sources, sizeof(sources) / sizeof(sources[0]), &catalog) ==
           PHOTO_CATALOG_OK);
    assert(catalog.count == 5U);

    assert(strcmp(catalog.entries[0].name, "a.sjpg") == 0);
    assert(catalog.entries[0].source == PHOTO_SOURCE_SD);
    assert(catalog.entries[0].sliced);
    assert(strcmp(catalog.entries[0].lv_path, "A:/sdcard/image/a.sjpg") == 0);

    assert(strcmp(catalog.entries[1].name, "b.PNG") == 0);
    assert(!catalog.entries[1].sliced);

    assert(catalog.entries[2].source == PHOTO_SOURCE_BUILTIN);
    assert(strcmp(catalog.entries[2].name, "2.sjpg") == 0);
    assert(strcmp(catalog.entries[3].name, "1.jpg") == 0);
    assert(strcmp(catalog.entries[4].name, "3.jpg") == 0);
}

static void test_merge_keeps_same_name_across_sources(void)
{
    const photo_dir_entry_t sd_entries[] = {{"2.jpg", 1U}};
    const photo_dir_entry_t builtin_entries[] = {{"2.jpg", 2U}};
    const photo_catalog_source_t sources[] = {
        {"/sdcard/image", "A:/sdcard/image", PHOTO_SOURCE_SD, sd_entries, 1U},
        {"/assets/image", "A:/assets/image", PHOTO_SOURCE_BUILTIN, builtin_entries, 1U},
    };
    photo_catalog_t catalog;

    assert(photo_catalog_merge(sources, 2U, &catalog) == PHOTO_CATALOG_OK);
    assert(catalog.count == 2U);
    assert(catalog.entries[0].source == PHOTO_SOURCE_SD);
    assert(catalog.entries[1].source == PHOTO_SOURCE_BUILTIN);
}

static void test_merge_empty_and_invalid(void)
{
    const photo_dir_entry_t empty_entries[] = {{"a.bmp", 1U}, {"notes.txt", 1U}};
    const photo_catalog_source_t empty_sources[] = {
        {"/sdcard/image", "A:/sdcard/image", PHOTO_SOURCE_SD, empty_entries, 2U},
    };
    const photo_catalog_source_t no_entries[] = {
        {"/sdcard/image", "A:/sdcard/image", PHOTO_SOURCE_SD, NULL, 0U},
    };
    const photo_catalog_source_t bad_source[] = {
        {NULL, "A:/sdcard/image", PHOTO_SOURCE_SD, NULL, 0U},
    };
    photo_catalog_t catalog;

    assert(photo_catalog_merge(empty_sources, 1U, &catalog) == PHOTO_CATALOG_EMPTY);
    assert(catalog.count == 0U);
    assert(photo_catalog_merge(no_entries, 1U, &catalog) == PHOTO_CATALOG_EMPTY);
    assert(photo_catalog_merge(bad_source, 1U, &catalog) == PHOTO_CATALOG_INVALID_ARGUMENT);
    assert(photo_catalog_merge(NULL, 1U, &catalog) == PHOTO_CATALOG_INVALID_ARGUMENT);
    assert(photo_catalog_merge(empty_sources, 0U, &catalog) == PHOTO_CATALOG_INVALID_ARGUMENT);
}

static void test_merge_truncates_at_limit(void)
{
    photo_dir_entry_t entries[PHOTO_CATALOG_MAX_ENTRIES + 8U];
    char names[PHOTO_CATALOG_MAX_ENTRIES + 8U][PHOTO_CATALOG_NAME_MAX];
    photo_catalog_t catalog;

    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        snprintf(names[i], sizeof(names[i]), "img_%03u.jpg", (unsigned)i);
        snprintf(entries[i].name, sizeof(entries[i].name), "%s", names[i]);
        entries[i].size_bytes = 1U;
    }

    const photo_catalog_source_t sources[] = {
        {"/sdcard/image", "A:/sdcard/image", PHOTO_SOURCE_SD, entries,
         sizeof(entries) / sizeof(entries[0])},
    };

    assert(photo_catalog_merge(sources, 1U, &catalog) == PHOTO_CATALOG_TOO_MANY_ENTRIES);
    assert(catalog.count == PHOTO_CATALOG_MAX_ENTRIES);
    assert(strcmp(catalog.entries[0].name, "img_000.jpg") == 0);
    assert(strcmp(catalog.entries[PHOTO_CATALOG_MAX_ENTRIES - 1U].name, "img_063.jpg") == 0);
}

static void test_index_wrapping(void)
{
    assert(photo_catalog_next_index(0U, 3U) == 1U);
    assert(photo_catalog_next_index(2U, 3U) == 0U);
    assert(photo_catalog_prev_index(0U, 3U) == 2U);
    assert(photo_catalog_prev_index(1U, 3U) == 0U);
    assert(photo_catalog_next_index(0U, 0U) == 0U);
    assert(photo_catalog_prev_index(0U, 0U) == 0U);
}

static void test_scale_rule(void)
{

    assert(photo_catalog_scale_for_screen(1024U, 600U, 1024U, 600U) == 256U);
    assert(photo_catalog_scale_for_screen(640U, 480U, 1024U, 600U) == 256U);
    assert(photo_catalog_scale_for_screen(800U, 480U, 1024U, 600U) == 256U);

    const uint16_t panel_fit = photo_catalog_scale_for_screen(1024U, 600U, 800U, 480U);
    assert(panel_fit == 200U);
    assert((1024U * panel_fit) / 256U == 800U);

    const uint16_t fit = photo_catalog_scale_for_screen(1600U, 1200U, 1024U, 600U);
    assert(fit == 128U);
    assert((1600U * fit) / 256U == 800U);
    assert((1200U * fit) / 256U == 600U);

    assert(photo_catalog_scale_for_screen(2048U, 600U, 1024U, 600U) == 128U);
    assert(photo_catalog_scale_for_screen(4000U, 3000U, 1024U, 600U) == 51U);

    assert(photo_catalog_scale_for_screen(0U, 0U, 1024U, 600U) == 256U);
    assert(photo_catalog_scale_for_screen(1024U, 600U, 0U, 0U) == 256U);
}

static void test_decode_budget(void)
{
    assert(photo_catalog_decode_bytes(1024U, 600U, PHOTO_CATALOG_JPEG_BPP) == 1228800U);
    assert(photo_catalog_decode_bytes(1024U, 600U, PHOTO_CATALOG_PNG_BPP) == 2457600U);

    assert(photo_catalog_within_decode_budget("a.jpg", 1024U, 600U));
    assert(photo_catalog_within_decode_budget("a.jpg", 1600U, 1200U));
    assert(photo_catalog_within_decode_budget("a.png", 1024U, 600U));
    assert(!photo_catalog_within_decode_budget("a.jpg", 4000U, 3000U));
    assert(!photo_catalog_within_decode_budget("a.png", 2048U, 1024U));
    assert(!photo_catalog_within_decode_budget("a.jpg", 0U, 0U));
}

static void test_source_names(void)
{
    assert(strcmp(photo_catalog_source_name(PHOTO_SOURCE_SD), "sd") == 0);
    assert(strcmp(photo_catalog_source_name(PHOTO_SOURCE_BUILTIN), "builtin") == 0);
    assert(strcmp(photo_catalog_error_name(PHOTO_CATALOG_EMPTY), "empty") == 0);
    assert(strcmp(photo_catalog_error_name(PHOTO_CATALOG_TOO_MANY_ENTRIES), "too-many-entries") == 0);
}

int main(void)
{
    test_name_rules();
    test_paths();
    test_merge_prefers_sliced_and_keeps_sources();
    test_merge_keeps_same_name_across_sources();
    test_merge_empty_and_invalid();
    test_merge_truncates_at_limit();
    test_index_wrapping();
    test_scale_rule();
    test_decode_budget();
    test_source_names();
    puts("photo_catalog_test: ok");
    return 0;
}
