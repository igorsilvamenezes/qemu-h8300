/*
 * Backup battery with a low-voltage detector.
 *
 * Boards that keep their RAM and real-time clock on a lithium cell often
 * watch it with a comparator: its output tells the firmware whether the
 * cell is still above a threshold.  This device models the cell voltage
 * ("voltage", in mV) and the comparator ("threshold", in mV); GPIO output
 * "low" is high while the voltage is below the threshold.  Both can be
 * changed at run time with qom-set, the read-only "low" property shows
 * the detector output and the battery_sensor_* trace events report it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/irq.h"
#include "hw/misc/battery_sensor.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/reset.h"
#include "trace.h"

bool battery_sensor_is_low(BatterySensorState *s)
{
    return s->voltage_mv < s->threshold_mv;
}

static void battery_sensor_update(BatterySensorState *s)
{
    bool low = battery_sensor_is_low(s);

    trace_battery_sensor_level(s->description, s->voltage_mv,
                               s->threshold_mv, low ? "low" : "ok");
    qemu_set_irq(s->low_out, low);
}

static void battery_sensor_get_mv(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t *ptr = opaque;

    visit_type_uint32(v, name, ptr, errp);
}

static void battery_sensor_set_mv(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    BatterySensorState *s = BATTERY_SENSOR(obj);
    uint32_t *ptr = opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    *ptr = value;
    if (DEVICE(s)->realized) {
        battery_sensor_update(s);
    }
}

static bool battery_sensor_get_low(Object *obj, Error **errp)
{
    return battery_sensor_is_low(BATTERY_SENSOR(obj));
}

static void battery_sensor_reset(DeviceState *dev)
{
    battery_sensor_update(BATTERY_SENSOR(dev));
}

/*
 * The sensor usually hangs off a board without a bus, so system reset does
 * not reach it on its own; drive the output again at every reset.
 */
static void battery_sensor_system_reset(void *opaque)
{
    device_cold_reset(DEVICE(opaque));
}

static void battery_sensor_realize(DeviceState *dev, Error **errp)
{
    BatterySensorState *s = BATTERY_SENSOR(dev);

    if (s->description == NULL) {
        s->description = g_strdup("n/a");
    }
    qdev_init_gpio_out_named(dev, &s->low_out, "low", 1);
    qemu_register_reset(battery_sensor_system_reset, dev);
}

static void battery_sensor_unrealize(DeviceState *dev)
{
    qemu_unregister_reset(battery_sensor_system_reset, dev);
}

static void battery_sensor_init(Object *obj)
{
    BatterySensorState *s = BATTERY_SENSOR(obj);

    /* Defaults: a 3 V lithium cell and a 2.5 V detector. */
    s->voltage_mv = 3000;
    s->threshold_mv = 2500;

    /*
     * Plain QOM properties rather than qdev ones, so that they can still
     * be set after realize; -global battery-sensor.voltage=... works too.
     */
    object_property_add(obj, "voltage", "uint32", battery_sensor_get_mv,
                        battery_sensor_set_mv, NULL, &s->voltage_mv);
    object_property_set_description(obj, "voltage",
        "Battery voltage in mV (default 3000)");
    object_property_add(obj, "threshold", "uint32", battery_sensor_get_mv,
                        battery_sensor_set_mv, NULL, &s->threshold_mv);
    object_property_set_description(obj, "threshold",
        "Low-battery detector threshold in mV (default 2500)");
    object_property_add_bool(obj, "low", battery_sensor_get_low, NULL);
    object_property_set_description(obj, "low",
        "Whether the voltage is below the threshold (detector output)");
}

static const VMStateDescription vmstate_battery_sensor = {
    .name = TYPE_BATTERY_SENSOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(voltage_mv, BatterySensorState),
        VMSTATE_UINT32(threshold_mv, BatterySensorState),
        VMSTATE_END_OF_LIST()
    }
};

static Property battery_sensor_properties[] = {
    DEFINE_PROP_STRING("description", BatterySensorState, description),
    DEFINE_PROP_END_OF_LIST(),
};

static void battery_sensor_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Backup battery with low-voltage detector";
    dc->vmsd = &vmstate_battery_sensor;
    dc->reset = battery_sensor_reset;
    dc->realize = battery_sensor_realize;
    dc->unrealize = battery_sensor_unrealize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, battery_sensor_properties);
}

static const TypeInfo battery_sensor_info = {
    .name = TYPE_BATTERY_SENSOR,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BatterySensorState),
    .instance_init = battery_sensor_init,
    .class_init = battery_sensor_class_init,
};

static void battery_sensor_register_types(void)
{
    type_register_static(&battery_sensor_info);
}

type_init(battery_sensor_register_types)

BatterySensorState *battery_sensor_create_simple(Object *parent,
                                                 const char *name,
                                                 const char *description)
{
    DeviceState *dev = qdev_new(TYPE_BATTERY_SENSOR);

    qdev_prop_set_string(dev, "description", description);
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    return BATTERY_SENSOR(dev);
}
