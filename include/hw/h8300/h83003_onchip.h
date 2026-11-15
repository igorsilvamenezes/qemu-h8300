/*
 * H8/3003 on-chip supporting modules: interrupt controller, ITU, DMAC,
 * TPC, A/D converter, refresh controller and bus controller registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_H8300_H83003_ONCHIP_H
#define HW_H8300_H83003_ONCHIP_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_H83003_ONCHIP "h83003-onchip"
OBJECT_DECLARE_SIMPLE_TYPE(H83003OnChipState, H83003_ONCHIP)

/* The device decodes 0xFFFF00-0xFFFFFF; board devices overlay it. */
#define H83003_ONCHIP_BASE 0xffff00
#define H83003_ONCHIP_SIZE 0x100

#define H83003_NR_VECTORS 64
#define H83003_TPC_LINES 16     /* PA0-PA7, PB0-PB7 */

/*
 * GPIO lines: unnamed inputs are the internal interrupt vectors, "irq-pin"
 * IRQ0-IRQ7, "ack" the CPU interrupt acknowledge and the "tpc" outputs
 * the TPC pins.  The 8 "an" inputs are the analog pins AN0-AN7: the level
 * is the conversion result (0-0x3ff, a fraction of AVref), which replaces
 * the value of the "anN" property.
 */
#define H83003_AN_LINES 8

/* Vector numbers used by board-level interrupt sources. */
#define H83003_VEC_WOVI 20
#define H83003_VEC_SCI0 52      /* ERI0, RXI0, TXI0, TEI0 */
#define H83003_VEC_SCI1 56      /* ERI1, RXI1, TXI1, TEI1 */

typedef struct H83003ITUChannel {
    struct H83003OnChipState *s;
    int index;
    uint8_t tcr, tior, tier, tsr, read_tsr;
    uint16_t tcnt;              /* count at anchor_ns while running */
    uint16_t gra, grb, bra, brb;
    int64_t anchor_ns;
    bool in_sync;
    QEMUTimer timer;
} H83003ITUChannel;

typedef struct H83003DMAChannel {
    uint32_t mar;               /* 24 bits; MARR always reads all 1s */
    uint16_t etcr;
    uint8_t ioar;
    uint8_t dtcr;
} H83003DMAChannel;

struct H83003OnChipState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq cpu_irq;
    qemu_irq tpc_out[H83003_TPC_LINES];
    uint64_t freq;

    /* Interrupt controller */
    uint64_t in_level;          /* level inputs from board devices */
    uint8_t irq_pin;            /* IRQ0-7 pin requests (1 = asserted) */
    uint8_t iscr, ier, isr, read_isr, ipra, iprb;
    int cur_vec;

    /* ITU */
    uint8_t tstr, tsnc, tmdr, tfcr, toer, tocr;
    H83003ITUChannel itu[5];

    /* DMAC: index 2n = channel nA, 2n + 1 = channel nB */
    H83003DMAChannel dma[8];
    bool updating, rescan;

    /* TPC */
    uint8_t tpmr, tpcr, nderb, ndera, ndrb, ndra;

    /* Refresh controller (RTCNT counted lazily) */
    uint8_t rfshcr, rtmcsr, rtcor, rtcnt;
    int64_t rt_anchor_ns;

    /* A/D converter */
    uint8_t adcsr, read_adcsr, adcr;
    uint16_t addr[4];
    uint16_t an[8];
    int ad_ch;
    QEMUTimer ad_timer;

    /* Bus controller */
    uint8_t abwcr, astcr, wcr, wcer, brcr;

    uint32_t unimp_logged;
};

#endif
