#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the I2CTools screen below the supplied full-screen body.
 * @param parent Parent for the screen.
 * @param full_screen Full-screen host object.
 * @param close_button Shared close button.
 */
void i2ctools_view_build(lv_obj_t *parent, lv_obj_t *full_screen,
                         lv_obj_t *close_button);

/**
 * @brief Release the backend owned by the current page, if any.
 */
void i2ctools_view_cleanup(void);

#ifdef __cplusplus
}
#endif
