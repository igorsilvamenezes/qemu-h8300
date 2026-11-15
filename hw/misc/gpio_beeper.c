/*
 * GPIO driven beeper: a self-oscillating buzzer that sounds while its
 * input line is active.  There is no audio output; each beep is reported
 * through the gpio_beeper_on/gpio_beeper_off trace events and the "on"
 * property shows the current state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/misc/gpio_beeper.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

static void gpio_beeper_set(void *opaque, int line, int level)
{
    GpioBeeperState *s = GPIO_BEEPER(opaque);
    bool on = (level != 0) == s->gpio_active_high;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    assert(line == 0);
    if (on == s->on) {
        return;
    }
    s->on = on;
    if (on) {
        s->count++;
        s->on_since_ns = now;
        trace_gpio_beeper_on(s->description, s->count);
    } else {
        trace_gpio_beeper_off(s->description,
                              (now - s->on_since_ns) / SCALE_MS);
    }
}

static bool gpio_beeper_get_on(Object *obj, Error **errp)
{
    return GPIO_BEEPER(obj)->on;
}

static void gpio_beeper_reset(DeviceState *dev)
{
    GpioBeeperState *s = GPIO_BEEPER(dev);

    trace_gpio_beeper_reset(s->description, s->on, s->count);
    s->on = false;
    s->on_since_ns = 0;
    s->count = 0;
}

static void gpio_beeper_realize(DeviceState *dev, Error **errp)
{
    GpioBeeperState *s = GPIO_BEEPER(dev);

    if (s->description == NULL) {
        s->description = g_strdup("n/a");
    }
    qdev_init_gpio_in(dev, gpio_beeper_set, 1);
}

static void gpio_beeper_init(Object *obj)
{
    object_property_add_bool(obj, "on", gpio_beeper_get_on, NULL);
    object_property_set_description(obj, "on", "Whether the beeper sounds");
}

static const VMStateDescription vmstate_gpio_beeper = {
    .name = TYPE_GPIO_BEEPER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(on, GpioBeeperState),
        VMSTATE_INT64(on_since_ns, GpioBeeperState),
        VMSTATE_UINT32(count, GpioBeeperState),
        VMSTATE_END_OF_LIST()
    }
};

static Property gpio_beeper_properties[] = {
    DEFINE_PROP_STRING("description", GpioBeeperState, description),
    DEFINE_PROP_BOOL("gpio-active-high", GpioBeeperState, gpio_active_high,
                     true),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpio_beeper_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "GPIO driven beeper";
    dc->vmsd = &vmstate_gpio_beeper;
    dc->reset = gpio_beeper_reset;
    dc->realize = gpio_beeper_realize;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    device_class_set_props(dc, gpio_beeper_properties);
}

static const TypeInfo gpio_beeper_info = {
    .name = TYPE_GPIO_BEEPER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GpioBeeperState),
    .instance_init = gpio_beeper_init,
    .class_init = gpio_beeper_class_init,
};

static void gpio_beeper_register_types(void)
{
    type_register_static(&gpio_beeper_info);
}

type_init(gpio_beeper_register_types)

GpioBeeperState *gpio_beeper_create_simple(Object *parent, const char *name,
                                           const char *description)
{
    DeviceState *dev = qdev_new(TYPE_GPIO_BEEPER);

    qdev_prop_set_string(dev, "description", description);
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    return GPIO_BEEPER(dev);
}
