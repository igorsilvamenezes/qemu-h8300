/*
 * Bank of DIP switches
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_DIP_SWITCH_H
#define HW_MISC_DIP_SWITCH_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_DIP_SWITCH "dip-switch"
OBJECT_DECLARE_SIMPLE_TYPE(DIPSwitchState, DIP_SWITCH)

#define DIP_SWITCH_MAX 32

struct DIPSwitchState {
    DeviceState parent_obj;

    /*
     * Bit n set means switch n + 1 is ON.  Output GPIO "switch" n is high
     * while switch n + 1 is ON; the board decides what that means
     * electrically (most banks close a pin to ground).
     */
    uint32_t positions;
    qemu_irq out[DIP_SWITCH_MAX];
    char *pending;      /* "positions" set before realize, parsed there */

    /* Properties */
    uint32_t num_switches;
    char *label;
};

/*
 * Create a bank named @name under @parent.  @positions uses the syntax of
 * the "positions" property; NULL leaves every switch OFF.
 */
DIPSwitchState *dip_switch_create_simple(Object *parent, const char *name,
                                         const char *label,
                                         uint32_t num_switches,
                                         const char *positions);

/* Current positions, bit n set when switch n + 1 is ON. */
uint32_t dip_switch_get(DIPSwitchState *s);

/*
 * Parse @str as switch positions for a bank of @num switches, see the
 * "positions" property.
 */
bool dip_switch_parse(const char *str, uint32_t num, uint32_t *positions,
                      Error **errp);

/* Format @positions as "on,off,..." (switch 1 first); free with g_free. */
char *dip_switch_format(uint32_t positions, uint32_t num);

#endif
