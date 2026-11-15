/*
 * Momentary push button.
 *
 * GPIO output line 0 is high while the button is pressed; the board decides
 * what that does to its pin.  The button is operated through QOM:
 *
 *   qom-set <path> press on     press it for "hold-time" ms, then release
 *   qom-set <path> pressed on   hold it down until "pressed off"
 *
 * "presses" counts the presses since reset and push_button_state traces
 * every change.  A device reset releases the button; a button without a
 * bus is only reset if its board asks for it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/misc/push_button.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

static void push_button_set(PushButtonState *s, bool pressed, const char *how)
{
    if (pressed == s->pressed) {
        return;
    }
    s->pressed = pressed;
    if (pressed) {
        s->presses++;
    }
    trace_push_button_state(s->description, pressed ? "pressed" : "released",
                            how, s->presses);
    qemu_set_irq(s->out, pressed);
}

static void push_button_timer(void *opaque)
{
    push_button_set(opaque, false, "end of timed press");
}

static void push_button_set_press(Object *obj, bool value, Error **errp)
{
    PushButtonState *s = PUSH_BUTTON(obj);

    if (!value) {
        return;
    }
    push_button_set(s, true, "timed press");
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->hold_ms);
}

static void push_button_set_pressed(Object *obj, bool value, Error **errp)
{
    PushButtonState *s = PUSH_BUTTON(obj);

    timer_del(s->timer);
    push_button_set(s, value, "held");
}

static bool push_button_get_pressed(Object *obj, Error **errp)
{
    return PUSH_BUTTON(obj)->pressed;
}

static void push_button_reset(DeviceState *dev)
{
    PushButtonState *s = PUSH_BUTTON(dev);

    timer_del(s->timer);
    s->pressed = false;
    s->presses = 0;
    /* The board resets its view of the pin; keep the line in step. */
    qemu_set_irq(s->out, 0);
}

static void push_button_realize(DeviceState *dev, Error **errp)
{
    PushButtonState *s = PUSH_BUTTON(dev);

    if (s->hold_ms == 0) {
        error_setg(errp, "hold-time must be at least 1 ms");
        return;
    }
    if (s->description == NULL) {
        s->description = g_strdup("n/a");
    }
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, push_button_timer, s);
    qdev_init_gpio_out(dev, &s->out, 1);
}

static void push_button_init(Object *obj)
{
    PushButtonState *s = PUSH_BUTTON(obj);

    object_property_add_bool(obj, "press", push_button_get_pressed,
                             push_button_set_press);
    object_property_set_description(obj, "press",
        "Set to press the button for hold-time ms; reads true while pressed");
    object_property_add_bool(obj, "pressed", push_button_get_pressed,
                             push_button_set_pressed);
    object_property_set_description(obj, "pressed",
        "Hold the button down (on) or release it (off)");
    object_property_add_uint32_ptr(obj, "presses", &s->presses,
                                   OBJ_PROP_FLAG_READ);
    object_property_set_description(obj, "presses",
        "Presses since reset");
}

static const VMStateDescription vmstate_push_button = {
    .name = TYPE_PUSH_BUTTON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, PushButtonState),
        VMSTATE_BOOL(pressed, PushButtonState),
        VMSTATE_UINT32(presses, PushButtonState),
        VMSTATE_END_OF_LIST()
    }
};

static Property push_button_properties[] = {
    DEFINE_PROP_STRING("description", PushButtonState, description),
    DEFINE_PROP_UINT32("hold-time", PushButtonState, hold_ms, 3000),
    DEFINE_PROP_END_OF_LIST(),
};

static void push_button_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Momentary push button";
    dc->vmsd = &vmstate_push_button;
    dc->reset = push_button_reset;
    dc->realize = push_button_realize;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    device_class_set_props(dc, push_button_properties);
}

static const TypeInfo push_button_info = {
    .name = TYPE_PUSH_BUTTON,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(PushButtonState),
    .instance_init = push_button_init,
    .class_init = push_button_class_init,
};

static void push_button_register_types(void)
{
    type_register_static(&push_button_info);
}

type_init(push_button_register_types)

PushButtonState *push_button_create_simple(Object *parent, const char *name,
                                           const char *description)
{
    DeviceState *dev = qdev_new(TYPE_PUSH_BUTTON);

    qdev_prop_set_string(dev, "description", description);
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    return PUSH_BUTTON(dev);
}
