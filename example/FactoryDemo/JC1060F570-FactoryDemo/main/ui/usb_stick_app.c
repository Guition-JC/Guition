#include "usb_stick_app.h"

#include <stdio.h>
#include <stdlib.h>

#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"

#define C_CANVAS lv_color_hex(0xDCE8F5)
#define C_SURFACE lv_color_hex(0xF7FAFD)
#define C_MUTED lv_color_hex(0xE2ECF6)
#define C_INK lv_color_hex(0x122033)
#define C_INK_MUTED lv_color_hex(0x53667B)
#define C_SYSTEM lv_color_hex(0x101A2B)
#define C_SYSTEM_TEXT lv_color_hex(0xF8FBFF)
#define C_ACCENT lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_SUCCESS lv_color_hex(0x22A96B)
#define C_WARNING_BG lv_color_hex(0xFFF3CD)
#define C_WARNING_INK lv_color_hex(0x7A5200)
#define C_DANGER lv_color_hex(0xE05151)
#define C_DIVIDER lv_color_hex(0xD9E0E7)

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    struct usb_stick_app *app;
    usb_stick_ui_intent_kind_t kind;
    usb_stick_ui_role_t role;
    bool enabled;
} intent_binding_t;

struct usb_stick_app {
    lv_obj_t *root;
    lv_obj_t *role_buttons[2];
    lv_obj_t *role_labels[2];
    lv_obj_t *action_buttons[4];
    lv_obj_t *close_button;
    lv_obj_t *summary_label;
    lv_obj_t *status_label;
    lv_obj_t *error_label;
    lv_obj_t *guidance_label;
    lv_obj_t *blocking_panel;
    lv_obj_t *blocking_label;
    intent_binding_t bindings[USB_STICK_UI_INTENT_COUNT];
    usb_stick_ui_intent_cb_t intent_cb;
    void *user_data;
    usb_stick_ui_role_t role;
    uint32_t generation;
    bool has_generation;
};

static bool valid_role(usb_stick_ui_role_t role)
{
    return role >= USB_STICK_UI_ROLE_NONE && role <= USB_STICK_UI_ROLE_HOST;
}

static bool valid_state(usb_stick_ui_state_t state)
{
    return state >= USB_STICK_UI_STATE_IDLE && state <= USB_STICK_UI_STATE_FAILED;
}

static const char *text_or(const char *text, const char *fallback)
{
    return text != NULL && text[0] != '\0' ? text : fallback;
}

static const char *role_text(usb_stick_ui_role_t role)
{
    static const char *const values[] = {"NONE", "DEVICE", "HOST"};
    return valid_role(role) ? values[role] : "INVALID";
}

static const char *state_text(usb_stick_ui_state_t state)
{
    static const char *const values[] = {"IDLE", "STARTING", "ACTIVE", "STOPPING", "FAILED"};
    return valid_state(state) ? values[state] : "INVALID";
}

static void style_plain(lv_obj_t *obj, lv_color_t color, int32_t radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static void emit_intent(lv_event_t *event)
{
    intent_binding_t *binding = lv_event_get_user_data(event);
    if (binding == NULL || binding->app == NULL || !binding->enabled ||
        binding->kind >= USB_STICK_UI_INTENT_COUNT || binding->app->intent_cb == NULL) {
        return;
    }
    const usb_stick_ui_intent_t intent = {
        .kind = binding->kind,
        .role = binding->role == USB_STICK_UI_ROLE_NONE ? binding->app->role : binding->role,
        .generation = binding->app->generation,
    };
    binding->app->intent_cb(&intent, binding->app->user_data);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             intent_binding_t *binding, lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    style_plain(button, C_MUTED, 12);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, emit_intent, LV_EVENT_CLICKED, binding);
    lv_obj_t *label = make_label(button, text, C_INK);
    lv_obj_center(label);
    if (label_out != NULL) {
        *label_out = label;
    }
    return button;
}

static void set_enabled(lv_obj_t *button, intent_binding_t *binding, bool enabled)
{
    binding->enabled = enabled;
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, 0);
    }
}

static void detach_intent_callbacks(usb_stick_app_t *app)
{
    app->intent_cb = NULL;
    app->user_data = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(app->bindings); i++) {
        app->bindings[i].enabled = false;
        app->bindings[i].app = NULL;
    }
}

static void root_deleted(lv_event_t *event)
{
    usb_stick_app_t *app = lv_event_get_user_data(event);
    if (app != NULL) {
        detach_intent_callbacks(app);
        app->root = NULL;
    }
}

static void init_binding(usb_stick_app_t *app, usb_stick_ui_intent_kind_t kind,
                         usb_stick_ui_role_t role)
{
    app->bindings[kind] = (intent_binding_t){
        .app = app, .kind = kind, .role = role, .enabled = false,
    };
}

static lv_obj_t *usb_flex_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = make_label(parent, text, color);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static bool build_page(usb_stick_app_t *app, lv_obj_t *parent)
{
    app->root = lv_obj_create(parent);
    if (app->root == NULL) {
        return false;
    }
    lv_obj_add_event_cb(app->root, root_deleted, LV_EVENT_DELETE, app);
    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = C_CANVAS,
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(app->root, &colors, "", "", "", &layout)) {
        return false;
    }

    desktop_fullscreen_layout_remove_page_header(&layout);
    const lv_font_t *font = desktop_font_get_primary();
    if (font != NULL) {
        lv_obj_set_style_text_font(app->root, font, 0);
    }

    app->close_button = make_button(app->root, "CLOSE",
        &app->bindings[USB_STICK_UI_INTENT_CLOSE], NULL);
    lv_obj_set_size(app->close_button, 120, 44);
    lv_obj_align(app->close_button, LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_add_flag(app->close_button, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *left = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 43, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    lv_obj_t *role_panel = desktop_fullscreen_layout_create_panel(
        left, C_SURFACE, 100, 16, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_height(role_panel, 154);
    lv_obj_set_style_border_width(role_panel, 1, 0);
    lv_obj_set_style_border_color(role_panel, C_DIVIDER, 0);
    lv_obj_set_style_pad_all(role_panel, 14, 0);
    lv_obj_set_style_pad_row(role_panel, 8, 0);
    usb_flex_label(role_panel, "ROLE", C_INK);
    usb_flex_label(role_panel, "Select DEVICE or HOST while idle", C_INK_MUTED);
    lv_obj_t *role_actions = desktop_fullscreen_layout_create_panel(
        role_panel, C_SURFACE, 100, 0, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(role_actions, 56);
    lv_obj_set_style_pad_column(role_actions, 10, 0);
    lv_obj_set_flex_align(role_actions, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    app->role_buttons[0] = make_button(role_actions, "DEVICE",
        &app->bindings[USB_STICK_UI_INTENT_SELECT_DEVICE], &app->role_labels[0]);
    app->role_buttons[1] = make_button(role_actions, "HOST",
        &app->bindings[USB_STICK_UI_INTENT_SELECT_HOST], &app->role_labels[1]);
    lv_obj_set_size(app->role_buttons[0], LV_PCT(48), 54);
    lv_obj_set_size(app->role_buttons[1], LV_PCT(48), 54);

    lv_obj_t *status_panel = desktop_fullscreen_layout_create_panel(
        left, C_SURFACE, 100, 16, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_height(status_panel, 0);
    lv_obj_set_flex_grow(status_panel, 1);
    lv_obj_set_style_border_width(status_panel, 1, 0);
    lv_obj_set_style_border_color(status_panel, C_DIVIDER, 0);
    lv_obj_set_style_pad_all(status_panel, 14, 0);
    lv_obj_set_style_pad_row(status_panel, 7, 0);
    app->summary_label = usb_flex_label(
        status_panel, "role: NONE   state: IDLE   media: NOT MOUNTED   operation: IDLE", C_INK);
    usb_flex_label(status_panel, "status", C_INK_MUTED);
    app->status_label = usb_flex_label(status_panel, "Ready", C_SUCCESS);
    usb_flex_label(status_panel, "error", C_INK_MUTED);
    app->error_label = usb_flex_label(status_panel, "None", C_INK_MUTED);
    app->guidance_label = usb_flex_label(
        status_panel, "STOP before ejecting the USB Stick.", C_INK_MUTED);
    app->blocking_panel = lv_obj_create(status_panel);
    style_plain(app->blocking_panel, C_WARNING_BG, 10);
    lv_obj_set_size(app->blocking_panel, LV_PCT(100), 52);
    app->blocking_label = usb_flex_label(
        app->blocking_panel, "CLOSE blocked: controller is still working.", C_WARNING_INK);
    lv_obj_center(app->blocking_label);
    lv_obj_add_flag(app->blocking_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *actions = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, 56, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(actions, 14, 0);
    lv_obj_set_style_pad_row(actions, 12, 0);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    usb_flex_label(actions, "OPERATIONS", C_INK_MUTED);
    static const char *const names[] = {"START", "READ", "WRITE", "STOP"};
    static const usb_stick_ui_intent_kind_t kinds[] = {
        USB_STICK_UI_INTENT_START, USB_STICK_UI_INTENT_READ,
        USB_STICK_UI_INTENT_WRITE, USB_STICK_UI_INTENT_STOP,
    };
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        app->action_buttons[i] = make_button(actions, names[i], &app->bindings[kinds[i]], NULL);
        lv_obj_set_size(app->action_buttons[i], LV_PCT(100), 60);
        if (i == 3) {
            lv_obj_set_style_bg_color(app->action_buttons[i], C_DANGER, 0);
            lv_obj_set_style_text_color(app->action_buttons[i], C_SYSTEM_TEXT, 0);
        }
    }
    return true;
}

void usb_stick_app_refresh_fonts(usb_stick_app_t *app)
{
    if (app == NULL || app->root == NULL) {
        return;
    }
    const lv_font_t *font = desktop_font_get_primary();
    if (font != NULL) {
        lv_obj_set_style_text_font(app->root, font, 0);
    }
}

/**
 * @brief Allocate the page context and build USB role and operation controls.
 * @param parent Parent object.
 * @param intent_cb Callback for user actions.
 * @param user_data Context passed to the callback.
 * @return Created page context, or NULL on failure.
 */
usb_stick_app_t *usb_stick_app_create(lv_obj_t *parent,
                                      usb_stick_ui_intent_cb_t intent_cb,
                                      void *user_data)
{
    if (parent == NULL || intent_cb == NULL) {
        return NULL;
    }
    usb_stick_app_t *app = calloc(1, sizeof(*app));
    if (app == NULL) {
        return NULL;
    }
    app->intent_cb = intent_cb;
    app->user_data = user_data;
    init_binding(app, USB_STICK_UI_INTENT_SELECT_DEVICE, USB_STICK_UI_ROLE_DEVICE);
    init_binding(app, USB_STICK_UI_INTENT_SELECT_HOST, USB_STICK_UI_ROLE_HOST);
    init_binding(app, USB_STICK_UI_INTENT_START, USB_STICK_UI_ROLE_NONE);
    init_binding(app, USB_STICK_UI_INTENT_READ, USB_STICK_UI_ROLE_HOST);
    init_binding(app, USB_STICK_UI_INTENT_WRITE, USB_STICK_UI_ROLE_HOST);
    init_binding(app, USB_STICK_UI_INTENT_STOP, USB_STICK_UI_ROLE_NONE);
    init_binding(app, USB_STICK_UI_INTENT_CLOSE, USB_STICK_UI_ROLE_NONE);
    if (!build_page(app, parent)) {
        free(app);
        return NULL;
    }
    return app;
}

/**
 * @brief Apply a current-generation snapshot to page labels and enabled actions.
 * @param app Page context.
 * @param model State snapshot to render.
 * @return true if the snapshot is accepted; false for invalid or older input.
 */
bool usb_stick_app_render(usb_stick_app_t *app, const usb_stick_ui_view_model_t *model)
{
    if (app == NULL || app->root == NULL || model == NULL || !valid_role(model->role) ||
        !valid_state(model->state) || (app->has_generation && model->generation < app->generation)) {
        return false;
    }
    app->generation = model->generation;
    app->role = model->role;
    app->has_generation = true;
    char summary[144];
    (void)snprintf(summary, sizeof(summary), "role: %s   state: %s   media: %s   operation: %s",
                   role_text(model->role), state_text(model->state),
                   model->media_mounted ? "MOUNTED" : "NOT MOUNTED",
                   model->operation_busy ? "BUSY" : "IDLE");
    lv_label_set_text(app->summary_label, summary);
    char status[192];
    if (model->storage_info != NULL && model->storage_info[0] != '\0') {
        (void)snprintf(status, sizeof(status), "%s | %s",
                       text_or(model->status, "No status"), model->storage_info);
    } else {
        (void)snprintf(status, sizeof(status), "%s", text_or(model->status, "No status"));
    }
    lv_label_set_text(app->status_label, status);
    lv_obj_set_style_text_color(app->status_label,
        model->state == USB_STICK_UI_STATE_FAILED ? C_DANGER : C_SUCCESS, 0);
    lv_label_set_text(app->error_label, text_or(model->error, "None"));
    lv_obj_set_style_text_color(app->error_label,
        model->error != NULL && model->error[0] != '\0' ? C_DANGER : C_INK_MUTED, 0);
    const bool blocked = model->blocking_reason != NULL && model->blocking_reason[0] != '\0';
    if (blocked) {
        char reason[192];
        (void)snprintf(reason, sizeof(reason), "CLOSE blocked: %s", model->blocking_reason);
        lv_label_set_text(app->blocking_label, reason);
        lv_obj_remove_flag(app->blocking_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(app->blocking_panel, LV_OBJ_FLAG_HIDDEN);
    }
    const char *guidance = "Choose DEVICE or HOST, then START. STOP before ejecting the USB Stick.";
    if (model->operation_busy) {
        guidance = "STOP is unavailable while BUSY; wait for the operation before ejecting.";
    } else if (model->state == USB_STICK_UI_STATE_ACTIVE && model->role == USB_STICK_UI_ROLE_HOST &&
               model->media_mounted) {
        guidance = "Use READ or WRITE, then press STOP before ejecting the USB Stick.";
    } else if (model->state == USB_STICK_UI_STATE_STARTING || model->state == USB_STICK_UI_STATE_STOPPING) {
        guidance = "Wait for the session transition; keep the USB Stick connected.";
    } else if (model->state == USB_STICK_UI_STATE_FAILED) {
        guidance = "Resolve the error, then press STOP before ejecting the USB Stick.";
    }
    lv_label_set_text(app->guidance_label, guidance);
    const bool idle = model->state == USB_STICK_UI_STATE_IDLE && !model->operation_busy;
    set_enabled(app->role_buttons[0], &app->bindings[USB_STICK_UI_INTENT_SELECT_DEVICE],
                idle && model->permissions.select_device);
    set_enabled(app->role_buttons[1], &app->bindings[USB_STICK_UI_INTENT_SELECT_HOST],
                idle && model->permissions.select_host);
    lv_obj_set_style_bg_color(app->role_buttons[0], model->role == USB_STICK_UI_ROLE_DEVICE ? C_ACCENT : C_MUTED, 0);
    lv_obj_set_style_bg_color(app->role_buttons[1], model->role == USB_STICK_UI_ROLE_HOST ? C_ACCENT : C_MUTED, 0);
    lv_obj_set_style_text_color(app->role_labels[0], model->role == USB_STICK_UI_ROLE_DEVICE ? C_SURFACE : C_INK, 0);
    lv_obj_set_style_text_color(app->role_labels[1], model->role == USB_STICK_UI_ROLE_HOST ? C_SURFACE : C_INK, 0);
    const bool host_io_idle = model->state == USB_STICK_UI_STATE_ACTIVE &&
                              model->role == USB_STICK_UI_ROLE_HOST && model->media_mounted && !model->operation_busy;
    const bool can_stop = model->permissions.stop &&
                          (model->state == USB_STICK_UI_STATE_STARTING ||
                           model->state == USB_STICK_UI_STATE_ACTIVE || model->state == USB_STICK_UI_STATE_FAILED);
    set_enabled(app->action_buttons[0], &app->bindings[USB_STICK_UI_INTENT_START],
                idle && model->role != USB_STICK_UI_ROLE_NONE && model->permissions.start);
    set_enabled(app->action_buttons[1], &app->bindings[USB_STICK_UI_INTENT_READ], host_io_idle && model->permissions.read);
    set_enabled(app->action_buttons[2], &app->bindings[USB_STICK_UI_INTENT_WRITE], host_io_idle && model->permissions.write);
    set_enabled(app->action_buttons[3], &app->bindings[USB_STICK_UI_INTENT_STOP], can_stop);
    set_enabled(app->close_button, &app->bindings[USB_STICK_UI_INTENT_CLOSE], model->permissions.close || blocked);
    return true;
}

/**
 * @brief Delete page widgets and release their owning context.
 * @param app Page context to destroy.
 */
void usb_stick_app_destroy(usb_stick_app_t *app)
{
    if (app == NULL) {
        return;
    }
    detach_intent_callbacks(app);
    if (app->root != NULL) {
        lv_obj_delete(app->root);
        app->root = NULL;
    }
    free(app);
}
