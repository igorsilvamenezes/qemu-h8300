/*
 * Turnstile with a release solenoid and two rotation switches.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TURNSTILE_H
#define HW_MISC_TURNSTILE_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_TURNSTILE "turnstile"
OBJECT_DECLARE_SIMPLE_TYPE(TurnstileState, TURNSTILE)

/* GPIO output lines, named "switch": rotation switches 1 and 2. */
enum {
    TURNSTILE_SW1,
    TURNSTILE_SW2,
    TURNSTILE_SWITCHES
};

typedef enum TurnstilePhase {
    TURNSTILE_IDLE,         /* arm at rest */
    TURNSTILE_WAITING,      /* released, a passenger will push the arm */
    TURNSTILE_SW1_ON,       /* rotation started, switch 1 closed */
    TURNSTILE_BETWEEN,      /* switch 1 open again, switch 2 not yet */
    TURNSTILE_SW2_ON,       /* switch 2 closed, end of the rotation */
} TurnstilePhase;

struct TurnstileState {
    DeviceState parent_obj;

    QEMUTimer *timer;
    qemu_irq sw[TURNSTILE_SWITCHES];
    bool released;          /* solenoid lets the arm turn */
    uint32_t phase;         /* TurnstilePhase */
    uint32_t passages;      /* complete rotations since reset */

    /* Properties */
    char *description;
    bool release_active_high;
    bool auto_pass;
    uint32_t pass_delay_ms;
    uint32_t rotation_ms;
};

/*
 * Create and realize a turnstile as a child @name of @parent.  GPIO input
 * line 0 drives the release solenoid; the named "switch" outputs report
 * the rotation switches, active high.
 */
TurnstileState *turnstile_create_simple(Object *parent, const char *name,
                                        const char *description);

#endif
