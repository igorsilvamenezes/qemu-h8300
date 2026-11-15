/*
 * HP/Avago HDSP-211x eight character 5x7 intelligent alphanumeric display
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_HDSP211X_H
#define HW_DISPLAY_HDSP211X_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_HDSP211X "hdsp-211x"
OBJECT_DECLARE_SIMPLE_TYPE(HDSP211XState, HDSP211X)

#define HDSP211X_DIGITS 8
#define HDSP211X_UDCS 16
#define HDSP211X_ROWS 7
/* A0-A4 plus the FL input wired to A5. */
#define HDSP211X_MMIO_SIZE 0x40

struct HDSP211XState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharBackend chr;
    QEMUTimer *log_timer;

    uint8_t ch[HDSP211X_DIGITS];
    uint8_t flash[HDSP211X_DIGITS];
    uint8_t udc[HDSP211X_UDCS][HDSP211X_ROWS];
    uint8_t udc_addr;
    uint8_t ctrl;

    /* Last state written to the log, to report only visible changes. */
    char logged[HDSP211X_DIGITS * 2 + 8];
};

#endif
