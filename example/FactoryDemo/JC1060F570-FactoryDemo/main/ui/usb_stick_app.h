#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_STICK_UI_ROLE_NONE = 0,
    USB_STICK_UI_ROLE_DEVICE,
    USB_STICK_UI_ROLE_HOST,
} usb_stick_ui_role_t;

typedef enum {
    USB_STICK_UI_STATE_IDLE = 0,
    USB_STICK_UI_STATE_STARTING,
    USB_STICK_UI_STATE_ACTIVE,
    USB_STICK_UI_STATE_STOPPING,
    USB_STICK_UI_STATE_FAILED,
} usb_stick_ui_state_t;

typedef enum {
    USB_STICK_UI_INTENT_SELECT_DEVICE = 0,
    USB_STICK_UI_INTENT_SELECT_HOST,
    USB_STICK_UI_INTENT_START,
    USB_STICK_UI_INTENT_READ,
    USB_STICK_UI_INTENT_WRITE,
    USB_STICK_UI_INTENT_STOP,
    USB_STICK_UI_INTENT_CLOSE,
    USB_STICK_UI_INTENT_COUNT,
} usb_stick_ui_intent_kind_t;

typedef struct {
    usb_stick_ui_intent_kind_t kind;
    usb_stick_ui_role_t role;
    uint32_t generation;
} usb_stick_ui_intent_t;

typedef void (*usb_stick_ui_intent_cb_t)(const usb_stick_ui_intent_t *intent,
                                         void *user_data);

typedef struct {
    bool select_device;
    bool select_host;
    bool start;
    bool read;
    bool write;
    bool stop;
    bool close;
} usb_stick_ui_permissions_t;

typedef struct {
    uint32_t generation;
    usb_stick_ui_role_t role;
    usb_stick_ui_state_t state;
    const char *status;
    const char *storage_info;
    const char *error;
    const char *blocking_reason;
    bool media_mounted;
    bool operation_busy;
    usb_stick_ui_permissions_t permissions;
} usb_stick_ui_view_model_t;

typedef struct usb_stick_app usb_stick_app_t;

/**
 * @brief Create one full-screen page; the callback must not block the LVGL event loop.
 * @param parent Parent object.
 * @param intent_cb Callback for user actions.
 * @param user_data Context passed to the callback.
 * @return Created page context, or NULL on failure.
 */
usb_stick_app_t *usb_stick_app_create(lv_obj_t *parent,
                                      usb_stick_ui_intent_cb_t intent_cb,
                                      void *user_data);

/**
 * @brief Render an immutable state snapshot; older generations are rejected.
 * @param app Page context.
 * @param model State snapshot to render.
 * @return true if the snapshot is accepted; false for invalid or older input.
 */
bool usb_stick_app_render(usb_stick_app_t *app,
                          const usb_stick_ui_view_model_t *model);

/**
 * @brief Rebind page-owned objects to the currently selected global font roles.
 * @param app Page context.
 */
void usb_stick_app_refresh_fonts(usb_stick_app_t *app);

/**
 * @brief Delete the page-owned LVGL tree and release the page context.
 * @param app Page context to destroy.
 */
void usb_stick_app_destroy(usb_stick_app_t *app);

#ifdef __cplusplus
}
#endif
