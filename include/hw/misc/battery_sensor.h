/*
 * Backup battery with a low-voltage detector.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BATTERY_SENSOR_H
#define HW_MISC_BATTERY_SENSOR_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_BATTERY_SENSOR "battery-sensor"
OBJECT_DECLARE_SIMPLE_TYPE(BatterySensorState, BATTERY_SENSOR)

struct BatterySensorState {
    DeviceState parent_obj;

    qemu_irq low_out;

    /* Properties */
    char *description;
    uint32_t voltage_mv;
    uint32_t threshold_mv;
};

/*
 * Create and realize a battery sensor as a child @name of @parent.  GPIO
 * output line 0, named "low", is high while the battery voltage is below
 * the detector threshold.
 */
BatterySensorState *battery_sensor_create_simple(Object *parent,
                                                 const char *name,
                                                 const char *description);

/* Whether the detector currently reports a low battery. */
bool battery_sensor_is_low(BatterySensorState *s);

#endif
