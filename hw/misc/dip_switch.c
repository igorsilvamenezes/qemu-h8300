/*
 * Bank of DIP switches
 *
 * A bank of "num-switches" two-position switches, numbered from 1 as
 * printed on the part.  The "positions" property sets or reads them at
 * any time, also while the guest runs:
 *
 *   -global dip-switch.positions=on:off:off:on
 *   (qemu) qom-set /machine/<name> positions 0x8
 *   (qemu) qom-get /machine/<name> positions
 *
 * Each switch drives output GPIO "switch" n (switch n + 1), high while
 * the switch is ON.  -trace dip_switch_set reports every change.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/dip_switch.h"
#include "migration/vmstate.h"
#include "trace.h"

static uint32_t dip_switch_mask(uint32_t num)
{
    return num >= 32 ? UINT32_MAX : (1u << num) - 1;
}

static bool dip_switch_parse_one(const char *item, bool *on)
{
    if (!g_ascii_strcasecmp(item, "on") || !strcmp(item, "1")) {
        *on = true;
    } else if (!g_ascii_strcasecmp(item, "off") || !strcmp(item, "0")) {
        *on = false;
    } else {
        return false;
    }
    return true;
}

/*
 * Two forms are accepted:
 *  - a list with one entry per switch, switch 1 first: "on,off,off,on"
 *    (entries on/off or 1/0, separated by ',', ':' or '/'; ':' avoids
 *    the comma escaping of -machine and -global);
 *  - a number whose bit n is switch n + 1, ON when set: "8" or "0x8".
 * Anything that is not a number is taken as a list.
 */
bool dip_switch_parse(const char *str, uint32_t num, uint32_t *positions,
                      Error **errp)
{
    g_autofree char *s = g_strstrip(g_strdup(str ? str : ""));
    unsigned int value;

    if (qemu_strtoui(s, NULL, 0, &value) == 0) {
        if (value & ~dip_switch_mask(num)) {
            error_setg(errp, "0x%x sets switches beyond the %u of the bank",
                       value, num);
            return false;
        }
        *positions = value;
        return true;
    } else {
        g_auto(GStrv) items = g_strsplit_set(s, ",:/", -1);
        uint32_t i, n = g_strv_length(items);
        bool on;

        if (n == 1 && !dip_switch_parse_one(s, &on)) {
            error_setg(errp, "'%s' is neither a list like on:off:... nor "
                       "a number", s);
            return false;
        }
        if (n != num) {
            error_setg(errp, "'%s' lists %u switches, the bank has %u",
                       s, n, num);
            return false;
        }
        *positions = 0;
        for (i = 0; i < n; i++) {
            if (!dip_switch_parse_one(g_strstrip(items[i]), &on)) {
                error_setg(errp, "switch %u: '%s' is not on, off, 1 or 0",
                           i + 1, items[i]);
                return false;
            }
            *positions |= (uint32_t)on << i;
        }
        return true;
    }
}

char *dip_switch_format(uint32_t positions, uint32_t num)
{
    GString *str = g_string_new(NULL);
    uint32_t i;

    for (i = 0; i < num; i++) {
        g_string_append_printf(str, "%s%s", i ? "," : "",
                               positions & BIT(i) ? "on" : "off");
    }
    return g_string_free(str, false);
}

uint32_t dip_switch_get(DIPSwitchState *s)
{
    return s->positions;
}

static void dip_switch_drive(DIPSwitchState *s)
{
    uint32_t i;

    for (i = 0; i < s->num_switches; i++) {
        qemu_set_irq(s->out[i], extract32(s->positions, i, 1));
    }
}

static void dip_switch_apply(DIPSwitchState *s, uint32_t positions)
{
    g_autofree char *text = dip_switch_format(positions, s->num_switches);

    s->positions = positions;
    trace_dip_switch_set(s->label ? s->label : "", text, positions);
    dip_switch_drive(s);
}

static char *dip_switch_get_positions(Object *obj, Error **errp)
{
    DIPSwitchState *s = DIP_SWITCH(obj);

    return dip_switch_format(s->positions, s->num_switches);
}

/*
 * Before realize the bank size may still change, so keep the text and
 * parse it in realize.
 */
static void dip_switch_set_positions(Object *obj, const char *str,
                                     Error **errp)
{
    DIPSwitchState *s = DIP_SWITCH(obj);
    uint32_t positions;

    if (!DEVICE(obj)->realized) {
        g_free(s->pending);
        s->pending = g_strdup(str);
        return;
    }
    if (dip_switch_parse(str, s->num_switches, &positions, errp)) {
        dip_switch_apply(s, positions);
    }
}

static void dip_switch_realize(DeviceState *dev, Error **errp)
{
    DIPSwitchState *s = DIP_SWITCH(dev);
    uint32_t positions = 0;

    if (s->num_switches < 1 || s->num_switches > DIP_SWITCH_MAX) {
        error_setg(errp, "num-switches must be between 1 and %d",
                   DIP_SWITCH_MAX);
        return;
    }
    if (s->pending &&
        !dip_switch_parse(s->pending, s->num_switches, &positions, errp)) {
        return;
    }
    g_clear_pointer(&s->pending, g_free);
    qdev_init_gpio_out_named(dev, s->out, "switch", s->num_switches);
    dip_switch_apply(s, positions);
}

/* Switches keep their positions over a reset; refresh the outputs. */
static void dip_switch_reset(DeviceState *dev)
{
    dip_switch_drive(DIP_SWITCH(dev));
}

static int dip_switch_post_load(void *opaque, int version_id)
{
    dip_switch_drive(opaque);
    return 0;
}

static const VMStateDescription vmstate_dip_switch = {
    .name = TYPE_DIP_SWITCH,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dip_switch_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(positions, DIPSwitchState),
        VMSTATE_END_OF_LIST()
    }
};

static Property dip_switch_properties[] = {
    DEFINE_PROP_UINT32("num-switches", DIPSwitchState, num_switches, 8),
    DEFINE_PROP_STRING("label", DIPSwitchState, label),
    DEFINE_PROP_END_OF_LIST(),
};

static void dip_switch_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Bank of DIP switches";
    dc->vmsd = &vmstate_dip_switch;
    dc->reset = dip_switch_reset;
    dc->realize = dip_switch_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, dip_switch_properties);
    object_class_property_add_str(klass, "positions",
                                  dip_switch_get_positions,
                                  dip_switch_set_positions);
    object_class_property_set_description(klass, "positions",
        "Switch positions, switch 1 first: on:off:... or a bit mask "
        "(bit 0 = switch 1)");
}

static void dip_switch_finalize(Object *obj)
{
    g_free(DIP_SWITCH(obj)->pending);
}

static const TypeInfo dip_switch_info = {
    .name = TYPE_DIP_SWITCH,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DIPSwitchState),
    .instance_finalize = dip_switch_finalize,
    .class_init = dip_switch_class_init,
};

static void dip_switch_register_types(void)
{
    type_register_static(&dip_switch_info);
}

type_init(dip_switch_register_types)

DIPSwitchState *dip_switch_create_simple(Object *parent, const char *name,
                                         const char *label,
                                         uint32_t num_switches,
                                         const char *positions)
{
    DeviceState *dev = qdev_new(TYPE_DIP_SWITCH);

    qdev_prop_set_uint32(dev, "num-switches", num_switches);
    qdev_prop_set_string(dev, "label", label);
    if (positions) {
        object_property_set_str(OBJECT(dev), "positions", positions,
                                &error_fatal);
    }
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    return DIP_SWITCH(dev);
}
