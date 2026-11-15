/*
 * Philips SC28C94 quad UART (QUART), minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_SC28C94_H
#define HW_CHAR_SC28C94_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_SC28C94 "sc28c94"
OBJECT_DECLARE_SIMPLE_TYPE(SC28C94State, SC28C94)

#define SC28C94_CHANNELS 4
#define SC28C94_BLOCKS 2
#define SC28C94_FIFO_SIZE 8
/* Two 16 register blocks (channels a/b and c/d) and the global registers. */
#define SC28C94_MMIO_SIZE 0x30

typedef struct SC28C94Channel {
    SC28C94State *owner;
    CharBackend chr;
    uint8_t mr[3];
    uint8_t mr_ptr;
    uint8_t csr;
    bool rx_enabled;
    bool tx_enabled;
    uint8_t fifo[SC28C94_FIFO_SIZE];
    uint8_t fifo_head;
    uint8_t fifo_count;
} SC28C94Channel;

typedef struct SC28C94Block {
    uint8_t acr;
    uint8_t imr;
    uint8_t ctur;
    uint8_t ctlr;
    uint8_t ivr;
    uint8_t opcr;
} SC28C94Block;

struct SC28C94State {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    SC28C94Channel ch[SC28C94_CHANNELS];
    SC28C94Block block[SC28C94_BLOCKS];
    uint8_t global[16];
    uint8_t cir;
};

#endif
