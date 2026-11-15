/*
 * NTC thermistor in a voltage divider, read by an A/D converter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_NTC_THERMISTOR_H
#define HW_MISC_NTC_THERMISTOR_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_NTC_THERMISTOR "ntc-thermistor"
OBJECT_DECLARE_SIMPLE_TYPE(NTCThermistorState, NTC_THERMISTOR)

struct NTCThermistorState {
    DeviceState parent_obj;

    qemu_irq out;               /* A/D conversion result */
    int32_t temperature;        /* millidegrees Celsius */
    uint32_t code;              /* last value driven on "out" */

    /* Properties */
    char *description;
    uint32_t r25;               /* ohms at 25 degrees Celsius */
    uint32_t beta;              /* kelvin */
    uint32_t r_ref;             /* ohms, the other leg of the divider */
    bool low_side;              /* thermistor between the input and ground */
    uint8_t adc_bits;
};

/*
 * Create and realize a thermistor as a child @name of @parent, with the
 * GPIO output 0 connected to @adc_in before the first value is driven.
 */
NTCThermistorState *ntc_thermistor_create_simple(Object *parent,
                                                 const char *name,
                                                 const char *description,
                                                 uint32_t r25, uint32_t beta,
                                                 uint32_t r_ref,
                                                 qemu_irq adc_in);

#endif
