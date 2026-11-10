#include "desktop.h"
#include <uapi/pointer.h>

void pointer_preferences_changed(void) {
    memset(desk.pointer_motion, 0, sizeof(desk.pointer_motion));
    if (!desk.buttons)
        desk.primary_right_active = desk.primary_right;
    trace("POINTER_PREFERENCES", NULL, desk.pointer_acceleration ? "adaptive" : "linear");
}
void pointer_reset_defaults(void) {
    desk.pointer_speed = desk.touchpad_speed = 100;
    desk.pointer_acceleration = desk.primary_right = desk.natural_scroll = desk.touchpad_natural =
        false;
    desk.wheel_steps = 1;
    pointer_preferences_changed();
}
void pointer_input_reset(void) {
    memset(desk.pointer_motion, 0, sizeof(desk.pointer_motion));
    memset(desk.pointer_buttons, 0, sizeof(desk.pointer_buttons));
    desk.primary_right_active = desk.primary_right;
}
void pointer_inventory_refresh(void) {
    struct os_pointer_inventory inventory;
    int error = call2(SYS_pointer_info, &inventory, sizeof(inventory));
    if (!error && inventory.version == 1 && inventory.size == sizeof(inventory))
        desk.pointer_inventory = inventory;
    else
        memset(&desk.pointer_inventory, 0, sizeof(desk.pointer_inventory));
    desk.pointer_info_at = now_ms();
}
void pointer_transform(struct os_input_event_v2 *input) {
    struct os_input_event *e = &input->event;
    unsigned source = input->source_id < 256 ? input->source_id : 0;
    if (e->code & 4)
        pointer_input_reset(); /* Lost source samples: release stale buttons. */
    if (e->code & 3)
        memset(&desk.pointer_motion[source], 0, sizeof(PointerMotion));
    desk.pointer_buttons[source] = (e->code & 2) ? 0 : e->buttons;
    unsigned physical = 0;
    for (unsigned i = 0; i < 256; i++)
        physical |= desk.pointer_buttons[i];
    if (!physical)
        desk.primary_right_active = desk.primary_right;
    e->buttons = desk.primary_right_active
                     ? (physical & ~3u) | ((physical & 1u) << 1) | ((physical & 2u) >> 1)
                     : physical;
    bool touchpad = (input->capabilities & OS_POINTER_TOUCHPAD) != 0;
    pointer_motion(&desk.pointer_motion[source],
                   touchpad ? desk.touchpad_speed : desk.pointer_speed, desk.pointer_acceleration,
                   e->dx, e->dy, input->timestamp_us, &e->dx, &e->dy);
    int64_t wheel = (int64_t)e->wheel * desk.wheel_steps;
    if (touchpad ? desk.touchpad_natural : desk.natural_scroll)
        wheel = -wheel;
    e->wheel = clampi(wheel, -32, 32);
}
