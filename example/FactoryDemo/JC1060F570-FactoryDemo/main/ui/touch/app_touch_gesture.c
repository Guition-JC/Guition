#include "app_touch_gesture.h"

#include <stdlib.h>

/** @brief Exit-zone width is a panel percentage; height and swipe distance are pixels. */
#define APP_TOUCH_FOCUS_WIDTH_PERCENT 100U
#define APP_TOUCH_FOCUS_HEIGHT_PX 10U
#define APP_TOUCH_UP_DISTANCE_PX 60U

void app_touch_gesture_init(app_touch_gesture_t *gesture,
                              uint16_t screen_width, uint16_t screen_height)
{
    if (gesture == NULL) {
        return;
    }

    const uint16_t focus_width =
        (uint16_t)(((uint32_t)screen_width * APP_TOUCH_FOCUS_WIDTH_PERCENT) / 100U);
    const uint16_t focus_height = APP_TOUCH_FOCUS_HEIGHT_PX;

    *gesture = (app_touch_gesture_t){
        .screen_width = screen_width,
        .screen_height = screen_height,
        .focus_x = (uint16_t)((screen_width - focus_width) / 2U),
        .focus_y = (uint16_t)(screen_height - focus_height),
        .focus_width = focus_width,
        .focus_height = focus_height,
    };
}

bool app_touch_gesture_begin(app_touch_gesture_t *gesture, uint16_t x, uint16_t y)
{
    if (gesture == NULL) {
        return false;
    }

    const uint32_t focus_right = (uint32_t)gesture->focus_x + gesture->focus_width;
    const uint32_t focus_bottom = (uint32_t)gesture->focus_y + gesture->focus_height;
    gesture->tracking = x >= gesture->focus_x && x < focus_right &&
                        y >= gesture->focus_y && y < focus_bottom;
    gesture->start_x = x;
    gesture->start_y = y;
    return gesture->tracking;
}

app_touch_gesture_action_t app_touch_gesture_update(app_touch_gesture_t *gesture,
                                                        uint16_t x, uint16_t y)
{
    if (gesture == NULL || !gesture->tracking) {
        return APP_TOUCH_GESTURE_NONE;
    }

    /** Require at least 60 pixels upward, exceeding the horizontal travel threshold. */
    const uint32_t up_distance = gesture->start_y > y ? (uint32_t)(gesture->start_y - y) : 0U;
    const uint32_t horizontal_distance = (uint32_t)abs((int)x - (int)gesture->start_x);
    if (up_distance >= APP_TOUCH_UP_DISTANCE_PX &&
        up_distance > horizontal_distance + horizontal_distance / 2U) {
        return APP_TOUCH_GESTURE_EXIT;
    }

    return APP_TOUCH_GESTURE_NONE;
}

void app_touch_gesture_end(app_touch_gesture_t *gesture)
{
    if (gesture == NULL) {
        return;
    }
    gesture->tracking = false;
}
