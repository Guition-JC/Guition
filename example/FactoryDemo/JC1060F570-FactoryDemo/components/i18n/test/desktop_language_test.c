#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "desktop_language.h"

static void test_catalog_keys_round_trip(void)
{
    size_t count = 0;
    const desktop_language_text_t *catalog = desktop_language_catalog(&count);
    assert(count == DESKTOP_TEXT_KEY_COUNT);
    for (size_t i = 0; i < count; ++i) {
        assert(catalog[i].key != NULL && catalog[i].key[0] != '\0');
        assert(catalog[i].english != NULL);
        assert(catalog[i].chinese != NULL);
        assert(desktop_language_key_from_name(catalog[i].key) == (desktop_text_key_t)i);
    }
}

static void test_launch_text_and_font_fallback(void)
{
    desktop_language_init(true);
    assert(desktop_language_select(DESKTOP_LANGUAGE_CHINESE));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_APP_LAUNCH_STARTING), "正在启动…") == 0);
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_APP_LAUNCH_HINT),
                  "正在准备应用，首次打开可能需要一些时间。") == 0);

    assert(desktop_language_select(DESKTOP_LANGUAGE_ENGLISH));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_APP_LAUNCH_STARTING), "Starting...") == 0);
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_APP_LAUNCH_HINT),
                  "Preparing the app. First launch may take a few moments.") == 0);

    desktop_language_init(false);
    assert(!desktop_language_select(DESKTOP_LANGUAGE_CHINESE));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_APP_LAUNCH_STARTING), "Starting...") == 0);
}

static bool contains_cjk(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        if (*cursor >= 0xE4U && *cursor <= 0xE9U) {
            return true;
        }
        ++cursor;
    }
    return false;
}

static void test_catalog_is_bilingual_and_english_is_cjk_free(void)
{
    size_t count = 0;
    const desktop_language_text_t *catalog = desktop_language_catalog(&count);
    for (size_t i = 0; i < count; ++i) {
        assert(catalog[i].english != NULL && catalog[i].english[0] != '\0');
        assert(catalog[i].chinese != NULL && catalog[i].chinese[0] != '\0');
        assert(!contains_cjk(catalog[i].english));
    }
}

static void test_wifi_page_copy_tracks_the_runtime_language(void)
{
    desktop_language_init(true);
    assert(desktop_language_select(DESKTOP_LANGUAGE_CHINESE));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_WIFI_ACTION_SCAN), "刷新扫描") == 0);
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_WIFI_QR_ACCESS),
                  "%s\n密码: %s\n设备验证码: %s") == 0);
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_AICHAT_BINDING_REQUIRED),
                  "请先绑定小智设备") == 0);

    assert(desktop_language_select(DESKTOP_LANGUAGE_ENGLISH));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_WIFI_ACTION_SCAN), "Refresh scan") == 0);
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_WIFI_ACTION_QR), "QR provisioning") == 0);
    assert(!contains_cjk(desktop_language_lookup(DESKTOP_TEXT_WIFI_FIELD_SIGNAL_VALUE)));
    assert(!contains_cjk(desktop_language_lookup(DESKTOP_TEXT_WIFI_ERROR_POLL_UNAVAILABLE)));
    assert(!contains_cjk(desktop_language_lookup(DESKTOP_TEXT_AICHAT_BINDING_TRANSPORT)));

    desktop_language_init(false);
    assert(!desktop_language_select(DESKTOP_LANGUAGE_CHINESE));
    assert(strcmp(desktop_language_lookup(DESKTOP_TEXT_WIFI_ACTION_QR), "QR provisioning") == 0);
    assert(!contains_cjk(desktop_language_lookup(DESKTOP_TEXT_WIFI_STATE_SCANNING)));
}

int main(void)
{
    test_catalog_keys_round_trip();
    test_launch_text_and_font_fallback();
    test_catalog_is_bilingual_and_english_is_cjk_free();
    test_wifi_page_copy_tracks_the_runtime_language();
    puts("desktop_language_test: ok");
    return 0;
}
