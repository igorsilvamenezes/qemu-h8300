/*
 * GPIO driven beeper: a self-oscillating buzzer that sounds while its
 * input line is active.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_GPIO_BEEPER_H
#define HW_MISC_GPIO_BEEPER_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_GPIO_BEEPER "gpio-beeper"
OBJECT_DECLARE_SIMPLE_TYPE(GpioBeeperState, GPIO_BEEPER)

struct GpioBeeperState {
    DeviceState parent_obj;

    bool on;
    int64_t on_since_ns;    /* QEMU_CLOCK_VIRTUAL time of the last turn-on */
    uint32_t count;         /* beeps since reset */

    /* Properties */
    char *description;
    bool gpio_active_high;
};

/*
 * Create and realize a beeper as a child @name of @parent.  Its GPIO input
 * line 0 sounds it.  @description identifies it in the trace output.
 */
GpioBeeperState *gpio_beeper_create_simple(Object *parent, const char *name,
                                           const char *description);

#endif
