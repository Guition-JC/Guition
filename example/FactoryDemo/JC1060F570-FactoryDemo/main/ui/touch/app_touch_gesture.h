#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Action recognized from a touch gesture. */
typedef enum {
    APP_TOUCH_GESTURE_NONE = 0,
    APP_TOUCH_GESTURE_EXIT,
} app_touch_gesture_action_t;

/** @brief Geometry and tracking state for the bottom-edge exit gesture. */
typedef struct {
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t focus_x;
    uint16_t focus_y;
    uint16_t focus_width;
    uint16_t focus_height;
    uint16_t start_x;
    uint16_t start_y;
    bool tracking;
} app_touch_gesture_t;

/**
 * @brief Set the exit zone to the full panel width and bottom 10 pixels.
 * @param[out] gesture State to initialize; NULL is ignored.
 * @param screen_width Panel width in pixels.
 * @param screen_height Panel height in pixels.
 */
void app_touch_gesture_init(app_touch_gesture_t *gesture,
                              uint16_t screen_width, uint16_t screen_height);
/**
 * @brief Begin tracking only when the initial point is inside the exit zone.
 * @param[in,out] gesture Gesture state.
 * @param x Initial horizontal coordinate in pixels.
 * @param y Initial vertical coordinate in pixels.
 * @return true if tracking started, otherwise false.
 */
bool app_touch_gesture_begin(app_touch_gesture_t *gesture, uint16_t x, uint16_t y);
/**
 * @brief Recognize an upward exit swipe from the tracked starting point.
 * @param gesture Gesture state.
 * @param x Current horizontal coordinate in pixels.
 * @param y Current vertical coordinate in pixels.
 * @return The recognized action, or APP_TOUCH_GESTURE_NONE.
 */
app_touch_gesture_action_t app_touch_gesture_update(app_touch_gesture_t *gesture,
                                                        uint16_t x, uint16_t y);
/**
 * @brief Stop tracking the current touch sequence.
 * @param[in,out] gesture Gesture state; NULL is ignored.
 */
void app_touch_gesture_end(app_touch_gesture_t *gesture);
