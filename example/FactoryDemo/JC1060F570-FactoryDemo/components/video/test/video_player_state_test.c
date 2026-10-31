#include <assert.h>
#include <stdio.h>

#include "video_player_state.h"

static void test_display_dimensions(void)
{

    const uint16_t panel_width = 1024U;
    const uint16_t panel_height = 600U;
    uint16_t limit_width = panel_width;
    uint16_t limit_height = panel_height;
    video_player_align_resolution(&limit_width, &limit_height);
    assert(limit_width == 1024U);
    assert(limit_height == 608U);
    assert(video_player_dimensions_supported(1024U, 600U, limit_width, limit_height));
    assert(video_player_dimensions_supported(1024U, 608U, limit_width, limit_height));
    assert(video_player_dimensions_supported(800U, 480U, limit_width, limit_height));
    assert(video_player_dimensions_supported(640U, 360U, limit_width, limit_height));
    assert(!video_player_dimensions_supported(0U, 480U, limit_width, limit_height));
    assert(!video_player_dimensions_supported(1025U, 600U, limit_width, limit_height));
    assert(!video_player_dimensions_supported(1024U, 609U, limit_width, limit_height));

    assert(!video_player_dimensions_supported(800U, 480U, 640U, 360U));
    assert(!video_player_dimensions_supported(800U, 480U, 0U, 0U));
}

static void test_frame_alignment(void)
{

    assert(video_player_align_dimension(0U) == 0U);
    assert(video_player_align_dimension(1U) == 16U);
    assert(video_player_align_dimension(16U) == 16U);
    assert(video_player_align_dimension(17U) == 32U);
    assert(video_player_align_dimension(600U) == 608U);
    assert(video_player_align_dimension(608U) == 608U);
    assert(video_player_align_dimension(480U) == 480U);
    assert(video_player_align_dimension(800U) == 800U);

    uint16_t width = 1024U;
    uint16_t height = 600U;
    video_player_align_resolution(&width, &height);
    assert(width == 1024U);
    assert(height == 608U);

    video_player_align_resolution(NULL, NULL);
    video_player_align_resolution(NULL, &height);
    assert(height == 608U);
}

static void test_accepts_only_mp4(void)
{
    assert(video_player_path_is_mp4("/sdcard/video/demo.mp4"));
    assert(video_player_path_is_mp4("/sdcard/video/DEMO.MP4"));
    assert(!video_player_path_is_mp4("/sdcard/video/demo.m4v"));
    assert(!video_player_path_is_mp4("/sdcard/video/.mp4"));
    assert(!video_player_path_is_mp4(NULL));
}

static void test_lifecycle_transitions(void)
{
    video_player_state_t state;
    video_player_state_init(&state);
    assert(state.status == VIDEO_PLAYER_STATUS_IDLE);
    assert(!video_player_state_can_play(&state));

    video_player_state_set_media(&state, "demo.mp4", 120000U);
    assert(video_player_state_can_play(&state));
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_PLAY));
    assert(state.status == VIDEO_PLAYER_STATUS_PLAYING);

    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_PAUSE));
    assert(state.status == VIDEO_PLAYER_STATUS_PAUSED);
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_PLAY));
    assert(state.status == VIDEO_PLAYER_STATUS_PLAYING);

    video_player_state_set_position(&state, 130000U);
    assert(state.position_ms == 120000U);
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_EOS));
    assert(state.status == VIDEO_PLAYER_STATUS_ENDED);
    assert(state.position_ms == 120000U);
}

static void test_error_clears_playing(void)
{
    video_player_state_t state;
    video_player_state_init(&state);
    video_player_state_set_media(&state, "demo.mp4", 1000U);
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_PLAY));
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_ERROR));
    assert(state.status == VIDEO_PLAYER_STATUS_ERROR);
    assert(!video_player_state_can_pause(&state));
    assert(video_player_state_apply(&state, VIDEO_PLAYER_EVENT_STOP));
    assert(state.status == VIDEO_PLAYER_STATUS_READY);
}

int main(void)
{
    test_display_dimensions();
    test_frame_alignment();
    test_accepts_only_mp4();
    test_lifecycle_transitions();
    test_error_clears_playing();
    puts("video_player_state_test: PASS");
    return 0;
}
