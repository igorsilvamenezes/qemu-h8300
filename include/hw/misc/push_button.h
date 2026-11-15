/*
 * Momentary push button.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PUSH_BUTTON_H
#define HW_MISC_PUSH_BUTTON_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_PUSH_BUTTON "push-button"
OBJECT_DECLARE_SIMPLE_TYPE(PushButtonState, PUSH_BUTTON)

struct PushButtonState {
    DeviceState parent_obj;

    QEMUTimer *timer;       /* releases a timed press */
    qemu_irq out;
    bool pressed;
    uint32_t presses;       /* presses since reset */

    /* Properties */
    char *description;
    uint32_t hold_ms;
};

/*
 * Create and realize a push button as a child @name of @parent.  GPIO
 * output line 0 is high while the button is pressed.
 */
PushButtonState *push_button_create_simple(Object *parent, const char *name,
                                           const char *description);

#endif
