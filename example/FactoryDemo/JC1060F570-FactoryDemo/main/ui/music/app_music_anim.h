#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 旋转角度以 0.1 度为单位。 */
#define APP_MUSIC_ARM_IDLE_ANGLE 3400
#define APP_MUSIC_ARM_PLAY_ANGLE 3600
#define APP_MUSIC_DISC_START_ANGLE 0
#define APP_MUSIC_DISC_END_ANGLE 3600

typedef enum {
    APP_MUSIC_ANIM_IDLE = 0,
    APP_MUSIC_ANIM_ARMING,
    APP_MUSIC_ANIM_PLAYING,
    APP_MUSIC_ANIM_RETURNING,
} app_music_anim_state_t;

typedef enum {
    APP_MUSIC_ANIM_EVENT_PLAY = 0,
    APP_MUSIC_ANIM_EVENT_PAUSE,
    APP_MUSIC_ANIM_EVENT_END,
    APP_MUSIC_ANIM_EVENT_SOURCE_SWITCH,
    APP_MUSIC_ANIM_EVENT_ARM_ENGAGED,
    APP_MUSIC_ANIM_EVENT_ARM_RESTED,
    APP_MUSIC_ANIM_EVENT_CLOSE,
} app_music_anim_event_t;

typedef enum {
    APP_MUSIC_ANIM_EFFECT_NONE = 0,
    APP_MUSIC_ANIM_EFFECT_ENGAGE_ARM,
    APP_MUSIC_ANIM_EFFECT_START_DISC,
    APP_MUSIC_ANIM_EFFECT_RETURN_ARM,
    APP_MUSIC_ANIM_EFFECT_STOP_ALL,
} app_music_anim_effect_t;

typedef struct {
    app_music_anim_state_t state;
    int32_t arm_angle;
} app_music_anim_t;

/**
 * @brief 恢复空闲状态并重置唱臂角度。
 * @param animation 要重置的动画状态。
 */
void app_music_anim_reset(app_music_anim_t *animation);
/**
 * @brief 处理播放事件，返回需要执行的动画动作。
 * @param animation 要更新的动画状态。
 * @param event 播放事件。
 * @return 需要执行的动画动作。
 */
app_music_anim_effect_t app_music_anim_reduce(app_music_anim_t *animation,
                                                   app_music_anim_event_t event);

#ifdef __cplusplus
}
#endif
