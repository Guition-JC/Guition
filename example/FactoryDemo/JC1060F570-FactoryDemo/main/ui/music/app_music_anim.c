#include "app_music_anim.h"

#include <stddef.h>

void app_music_anim_reset(app_music_anim_t *animation)
{
    if (animation == NULL) {
        return;
    }

    animation->state = APP_MUSIC_ANIM_IDLE;
    animation->arm_angle = APP_MUSIC_ARM_IDLE_ANGLE;
}

/**
 * @brief 进入唱臂归位状态。
 * @param animation 要更新的动画状态。
 * @return 需要执行的动画动作。
 */
static app_music_anim_effect_t begin_return(app_music_anim_t *animation)
{
    if (animation->state == APP_MUSIC_ANIM_IDLE ||
        animation->state == APP_MUSIC_ANIM_RETURNING) {
        return APP_MUSIC_ANIM_EFFECT_NONE;
    }

    animation->state = APP_MUSIC_ANIM_RETURNING;
    return APP_MUSIC_ANIM_EFFECT_RETURN_ARM;
}

/**
 * @brief 将播放事件转换为唱臂和唱片的状态变化。
 * @param animation 要更新的动画状态。
 * @param event 播放事件。
 * @return 需要执行的动画动作。
 */
app_music_anim_effect_t app_music_anim_reduce(app_music_anim_t *animation,
                                                   app_music_anim_event_t event)
{
    if (animation == NULL) {
        return APP_MUSIC_ANIM_EFFECT_NONE;
    }

    switch (event) {
        case APP_MUSIC_ANIM_EVENT_PLAY:
            if (animation->state == APP_MUSIC_ANIM_ARMING ||
                animation->state == APP_MUSIC_ANIM_PLAYING) {
                return APP_MUSIC_ANIM_EFFECT_NONE;
            }
            animation->state = APP_MUSIC_ANIM_ARMING;
            return APP_MUSIC_ANIM_EFFECT_ENGAGE_ARM;
        case APP_MUSIC_ANIM_EVENT_PAUSE:
        case APP_MUSIC_ANIM_EVENT_END:
        case APP_MUSIC_ANIM_EVENT_SOURCE_SWITCH:
            return begin_return(animation);
        case APP_MUSIC_ANIM_EVENT_ARM_ENGAGED:
            if (animation->state != APP_MUSIC_ANIM_ARMING) {
                return APP_MUSIC_ANIM_EFFECT_NONE;
            }
            animation->arm_angle = APP_MUSIC_ARM_PLAY_ANGLE;
            animation->state = APP_MUSIC_ANIM_PLAYING;
            return APP_MUSIC_ANIM_EFFECT_START_DISC;
        case APP_MUSIC_ANIM_EVENT_ARM_RESTED:
            if (animation->state != APP_MUSIC_ANIM_RETURNING) {
                return APP_MUSIC_ANIM_EFFECT_NONE;
            }
            animation->arm_angle = APP_MUSIC_ARM_IDLE_ANGLE;
            animation->state = APP_MUSIC_ANIM_IDLE;
            return APP_MUSIC_ANIM_EFFECT_NONE;
        case APP_MUSIC_ANIM_EVENT_CLOSE:
            app_music_anim_reset(animation);
            return APP_MUSIC_ANIM_EFFECT_STOP_ALL;
    }

    return APP_MUSIC_ANIM_EFFECT_NONE;
}
