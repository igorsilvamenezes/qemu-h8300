/*
 * Turnstile with a release solenoid and two rotation switches.
 *
 * The controller energizes the solenoid (GPIO input 0) to let the arm
 * turn.  A passenger pushing the arm closes switch 1 at the start of the
 * rotation and switch 2 near its end; the switch outputs are active high.
 * Once started, a rotation always completes, whatever the solenoid does.
 *
 * With "auto" (the default) a passenger pushes the arm "pass-delay" ms
 * after each release.  "qom-set <path> push on" pushes it by hand; a
 * locked arm does not turn.  "passages" counts the complete rotations and
 * the turnstile_* trace events report the solenoid, switches and passages.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/misc/turnstile.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Switch timing as a percentage of the rotation time. */
#define SW1_OPENS_PCT 40
#define SW2_CLOSES_PCT 60

static void turnstile_set_switch(TurnstileState *s, int n, bool closed)
{
    trace_turnstile_switch(s->description, n + 1, closed);
    qemu_set_irq(s->sw[n], closed);
}

static void turnstile_schedule(TurnstileState *s, uint32_t ms)
{
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
}

static uint32_t rotation_part(TurnstileState *s, unsigned pct)
{
    return s->rotation_ms * pct / 100;
}

static void turnstile_start_rotation(TurnstileState *s)
{
    s->phase = TURNSTILE_SW1_ON;
    turnstile_set_switch(s, TURNSTILE_SW1, true);
    turnstile_schedule(s, rotation_part(s, SW1_OPENS_PCT));
}

static void turnstile_timer(void *opaque)
{
    TurnstileState *s = opaque;

    switch (s->phase) {
    case TURNSTILE_WAITING:
        trace_turnstile_push(s->description, "automatic");
        turnstile_start_rotation(s);
        break;
    case TURNSTILE_SW1_ON:
        s->phase = TURNSTILE_BETWEEN;
        turnstile_set_switch(s, TURNSTILE_SW1, false);
        turnstile_schedule(s, rotation_part(s, SW2_CLOSES_PCT - SW1_OPENS_PCT));
        break;
    case TURNSTILE_BETWEEN:
        s->phase = TURNSTILE_SW2_ON;
        turnstile_set_switch(s, TURNSTILE_SW2, true);
        turnstile_schedule(s, rotation_part(s, 100 - SW2_CLOSES_PCT));
        break;
    case TURNSTILE_SW2_ON:
        s->phase = TURNSTILE_IDLE;
        turnstile_set_switch(s, TURNSTILE_SW2, false);
        s->passages++;
        trace_turnstile_passage(s->description, s->passages);
        break;
    default:
        break;
    }
}

static void turnstile_set_release(void *opaque, int line, int level)
{
    TurnstileState *s = TURNSTILE(opaque);
    bool released = (level != 0) == s->release_active_high;

    assert(line == 0);
    if (released == s->released) {
        return;
    }
    s->released = released;
    trace_turnstile_solenoid(s->description,
                             released ? "released" : "locked");
    if (released && s->phase == TURNSTILE_IDLE && s->auto_pass) {
        s->phase = TURNSTILE_WAITING;
        turnstile_schedule(s, s->pass_delay_ms);
    } else if (!released && s->phase == TURNSTILE_WAITING) {
        /* Locked again before the passenger arrived. */
        s->phase = TURNSTILE_IDLE;
        timer_del(s->timer);
    }
}

static void turnstile_set_push(Object *obj, bool value, Error **errp)
{
    TurnstileState *s = TURNSTILE(obj);

    if (!value) {
        return;
    }
    if (s->phase >= TURNSTILE_SW1_ON) {
        trace_turnstile_push(s->description, "ignored, already turning");
    } else if (!s->released) {
        trace_turnstile_push(s->description, "refused, arm locked");
    } else {
        trace_turnstile_push(s->description, "manual");
        timer_del(s->timer);
        turnstile_start_rotation(s);
    }
}

static bool turnstile_get_push(Object *obj, Error **errp)
{
    return TURNSTILE(obj)->phase >= TURNSTILE_SW1_ON;
}

static bool turnstile_get_released(Object *obj, Error **errp)
{
    return TURNSTILE(obj)->released;
}

static void turnstile_reset(DeviceState *dev)
{
    TurnstileState *s = TURNSTILE(dev);

    timer_del(s->timer);
    s->released = false;
    s->phase = TURNSTILE_IDLE;
    s->passages = 0;
}

static void turnstile_realize(DeviceState *dev, Error **errp)
{
    TurnstileState *s = TURNSTILE(dev);

    if (s->rotation_ms == 0) {
        error_setg(errp, "rotation-time must be at least 1 ms");
        return;
    }
    if (s->description == NULL) {
        s->description = g_strdup("n/a");
    }
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, turnstile_timer, s);
    qdev_init_gpio_in(dev, turnstile_set_release, 1);
    qdev_init_gpio_out_named(dev, s->sw, "switch", TURNSTILE_SWITCHES);
}

static void turnstile_init(Object *obj)
{
    TurnstileState *s = TURNSTILE(obj);

    object_property_add_bool(obj, "push", turnstile_get_push,
                             turnstile_set_push);
    object_property_set_description(obj, "push",
        "Set to push the arm; reads true while the arm turns");
    object_property_add_bool(obj, "released", turnstile_get_released, NULL);
    object_property_set_description(obj, "released",
        "Whether the solenoid lets the arm turn");
    object_property_add_uint32_ptr(obj, "passages", &s->passages,
                                   OBJ_PROP_FLAG_READ);
    object_property_set_description(obj, "passages",
        "Complete rotations since reset");
}

static const VMStateDescription vmstate_turnstile = {
    .name = TYPE_TURNSTILE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, TurnstileState),
        VMSTATE_BOOL(released, TurnstileState),
        VMSTATE_UINT32(phase, TurnstileState),
        VMSTATE_UINT32(passages, TurnstileState),
        VMSTATE_END_OF_LIST()
    }
};

static Property turnstile_properties[] = {
    DEFINE_PROP_STRING("description", TurnstileState, description),
    DEFINE_PROP_BOOL("release-active-high", TurnstileState,
                     release_active_high, true),
    DEFINE_PROP_BOOL("auto", TurnstileState, auto_pass, true),
    DEFINE_PROP_UINT32("pass-delay", TurnstileState, pass_delay_ms, 1000),
    DEFINE_PROP_UINT32("rotation-time", TurnstileState, rotation_ms, 600),
    DEFINE_PROP_END_OF_LIST(),
};

static void turnstile_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Turnstile with release solenoid and rotation switches";
    dc->vmsd = &vmstate_turnstile;
    dc->reset = turnstile_reset;
    dc->realize = turnstile_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, turnstile_properties);
}

static const TypeInfo turnstile_info = {
    .name = TYPE_TURNSTILE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TurnstileState),
    .instance_init = turnstile_init,
    .class_init = turnstile_class_init,
};

static void turnstile_register_types(void)
{
    type_register_static(&turnstile_info);
}

type_init(turnstile_register_types)

TurnstileState *turnstile_create_simple(Object *parent, const char *name,
                                        const char *description)
{
    DeviceState *dev = qdev_new(TYPE_TURNSTILE);

    qdev_prop_set_string(dev, "description", description);
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    return TURNSTILE(dev);
}
