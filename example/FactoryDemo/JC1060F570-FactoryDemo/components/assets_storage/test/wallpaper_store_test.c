/** SPDX-License-Identifier: GPL-2.0-only */

#define WALLPAPER_STORE_HOST_TEST 1

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wallpaper_store.h"

static void test_accepts_lvgl_and_posix_paths(void)
{
    assert(wallpaper_store_path_is_valid(WALLPAPER_DEFAULT_PATH));
    assert(wallpaper_store_path_is_valid("A:/sdcard/image/IMG_0001.JPG"));
    assert(wallpaper_store_path_is_valid("A:/sdcard/image/holiday.jpeg"));
    assert(wallpaper_store_path_is_valid("A:/sdcard/image/2.png"));
    assert(wallpaper_store_path_is_valid("A:/assets/image/2.sjpg"));
    assert(wallpaper_store_path_is_valid("A:/assets/image/2.spng"));
    assert(wallpaper_store_path_is_valid("/sdcard/image/2.jpg"));
    assert(wallpaper_store_path_is_valid("/assets/image/2.jpg"));
    assert(wallpaper_store_path_is_valid("A:/sdcard/image/相册.jpg"));
}

static void test_rejects_invalid_paths(void)
{
    assert(!wallpaper_store_path_is_valid(NULL));
    assert(!wallpaper_store_path_is_valid(""));
    assert(!wallpaper_store_path_is_valid("2.jpg"));
    assert(!wallpaper_store_path_is_valid("B:/sdcard/2.jpg"));
    assert(!wallpaper_store_path_is_valid("A:/"));
    assert(!wallpaper_store_path_is_valid("/"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/image"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/2.bmp"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/2.gif"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/2.jpg.txt"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/../2.jpg"));
    assert(!wallpaper_store_path_is_valid("A:/../2.jpg"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard//2.jpg"));
    assert(!wallpaper_store_path_is_valid("A:/sd\\card/2.jpg"));
    assert(!wallpaper_store_path_is_valid("A:/sdcard/2:1.jpg"));

    assert(wallpaper_store_path_is_valid("A:/sdcard/image/..jpg"));
}

static void test_rejects_overlong_paths(void)
{
    char path[WALLPAPER_PATH_MAX + 16U];
    memset(path, 'a', sizeof(path));
    path[0] = 'A';
    path[1] = ':';
    path[2] = '/';
    path[3] = 'i';
    path[WALLPAPER_PATH_MAX - 5U] = '.';
    path[WALLPAPER_PATH_MAX - 4U] = 'j';
    path[WALLPAPER_PATH_MAX - 3U] = 'p';
    path[WALLPAPER_PATH_MAX - 2U] = 'g';
    path[WALLPAPER_PATH_MAX - 1U] = '\0';
    assert(wallpaper_store_path_is_valid(path));

    memset(path, 'a', sizeof(path));
    path[0] = 'A';
    path[1] = ':';
    path[2] = '/';
    path[WALLPAPER_PATH_MAX - 4U] = '.';
    path[WALLPAPER_PATH_MAX - 3U] = 'j';
    path[WALLPAPER_PATH_MAX - 2U] = 'p';
    path[WALLPAPER_PATH_MAX - 1U] = 'g';
    path[WALLPAPER_PATH_MAX] = '\0';
    assert(!wallpaper_store_path_is_valid(path));
}

static void test_posix_conversion(void)
{
    char out[WALLPAPER_PATH_MAX];

    assert(wallpaper_store_posix_path(WALLPAPER_DEFAULT_PATH, out, sizeof(out)));
    assert(strcmp(out, "/assets/image/2.jpg") == 0);

    assert(wallpaper_store_posix_path("/sdcard/image/2.jpg", out, sizeof(out)));
    assert(strcmp(out, "/sdcard/image/2.jpg") == 0);

    assert(!wallpaper_store_posix_path("A:/sdcard/2.bmp", out, sizeof(out)));
    assert(!wallpaper_store_posix_path(WALLPAPER_DEFAULT_PATH, out, 4U));
    assert(out[0] == '\0');
}

int main(void)
{
    test_accepts_lvgl_and_posix_paths();
    test_rejects_invalid_paths();
    test_rejects_overlong_paths();
    test_posix_conversion();
    puts("wallpaper_store_test: ok");
    return 0;
}
