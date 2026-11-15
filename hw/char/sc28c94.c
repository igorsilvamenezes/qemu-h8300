/*
 * Philips SC28C94 quad UART (QUART), minimal model
 *
 * The SC28C94 holds two 68681-style DUART blocks and a set of global
 * interrupt registers.  With the chip on a byte-wide bus the map is:
 *
 *   0x00-0x0F  block A: channels a (0x00-0x03) and b (0x08-0x0B)
 *   0x10-0x1F  block B: channels c (0x10-0x13) and d (0x18-0x1B)
 *   0x20-0x2F  global registers
 *
 * Inside a block: MR (0), SR/CSR (1), CR (2), RHR/THR (3), IPCR/ACR (4),
 * ISR/IMR (5), CTU (6), CTL (7), the same four for channel b at 8-0xB,
 * IVR (0xC), IP/OPCR (0xD) and the counter and output port commands
 * (0xE, 0xF).  MR is accessed through a pointer that walks MR0, MR1, MR2;
 * CR command 0x1 moves it to MR1 and command 0xB to MR0.
 *
 * The global register map below covers what the E2300 operating system
 * uses: a write to 0x2A updates the Current Interrupt Register, which is
 * read at 0x28 and reports the interrupting channel in bits 1-0 and the
 * cause in bits 4-2 (2 = transmitter ready, 3 = receiver ready, 0 = none).
 * Other global registers only store the value written.
 *
 * This is a functional model, not a cycle accurate one: a character written
 * to THR is sent to the channel's character backend at once, so the
 * transmitter is always ready and empty while enabled.  Characters received
 * from the backend go into an eight byte FIFO.  Baud rate, parity, counter,
 * timer and the input and output ports are stored but have no effect.  The
 * interrupt output is asserted while any enabled block interrupt is pending;
 * the interrupt arbitration threshold logic is not modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/char/sc28c94.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "trace.h"

#define SR_RXRDY    0x01
#define SR_FFULL    0x02
#define SR_TXRDY    0x04
#define SR_TXEMT    0x08

#define ISR_TXRDY   0x01    /* channel a; channel b is shifted left by 4 */
#define ISR_RXRDY   0x02

#define CIR_UPDATE  0x0a    /* global register offsets */
#define CIR_READ    0x08
#define CIR_TYPE_TX 2
#define CIR_TYPE_RX 3

static uint8_t sc28c94_sr(SC28C94Channel *c)
{
    uint8_t sr = 0;

    if (c->fifo_count) {
        sr |= SR_RXRDY;
    }
    if (c->fifo_count == SC28C94_FIFO_SIZE) {
        sr |= SR_FFULL;
    }
    if (c->tx_enabled) {
        sr |= SR_TXRDY | SR_TXEMT;
    }
    return sr;
}

/* Interrupt status of a block before masking by IMR. */
static uint8_t sc28c94_isr(SC28C94State *s, int b)
{
    uint8_t isr = 0;
    int i;

    for (i = 0; i < 2; i++) {
        uint8_t sr = sc28c94_sr(&s->ch[b * 2 + i]);

        isr |= ((sr & SR_TXRDY ? ISR_TXRDY : 0) |
                (sr & SR_RXRDY ? ISR_RXRDY : 0)) << (i * 4);
    }
    return isr;
}

static void sc28c94_update_irq(SC28C94State *s)
{
    bool level = false;
    int b;

    for (b = 0; b < SC28C94_BLOCKS; b++) {
        level |= (sc28c94_isr(s, b) & s->block[b].imr) != 0;
    }
    qemu_set_irq(s->irq, level);
}

/* Latch the highest priority enabled interrupt: receivers first. */
static void sc28c94_update_cir(SC28C94State *s)
{
    static const uint8_t types[] = { CIR_TYPE_RX, CIR_TYPE_TX };
    int t, n;

    for (t = 0; t < ARRAY_SIZE(types); t++) {
        uint8_t bit = types[t] == CIR_TYPE_RX ? ISR_RXRDY : ISR_TXRDY;

        for (n = 0; n < SC28C94_CHANNELS; n++) {
            int b = n / 2;
            uint8_t mask = bit << ((n & 1) * 4);

            if (sc28c94_isr(s, b) & s->block[b].imr & mask) {
                s->cir = types[t] << 2 | n;
                return;
            }
        }
    }
    s->cir = 0;
}

static uint8_t sc28c94_pop(SC28C94Channel *c)
{
    uint8_t val;

    if (!c->fifo_count) {
        return 0;
    }
    val = c->fifo[c->fifo_head];
    c->fifo_head = (c->fifo_head + 1) % SC28C94_FIFO_SIZE;
    c->fifo_count--;
    qemu_chr_fe_accept_input(&c->chr);
    return val;
}

static void sc28c94_command(SC28C94State *s, int n, uint8_t val)
{
    SC28C94Channel *c = &s->ch[n];

    switch (val & 0x03) {
    case 1:
        c->rx_enabled = true;
        qemu_chr_fe_accept_input(&c->chr);
        break;
    case 2:
        c->rx_enabled = false;
        break;
    }
    switch (val & 0x0c) {
    case 4:
        c->tx_enabled = true;
        break;
    case 8:
        c->tx_enabled = false;
        break;
    }
    switch (val >> 4) {
    case 0x0:   /* no command */
    case 0x4:   /* reset error status */
    case 0x5:   /* reset break change interrupt */
    case 0x6:   /* start break */
    case 0x7:   /* stop break */
        break;
    case 0x1:
        c->mr_ptr = 1;
        break;
    case 0x2:   /* reset receiver */
        c->rx_enabled = false;
        c->fifo_head = c->fifo_count = 0;
        qemu_chr_fe_accept_input(&c->chr);
        break;
    case 0x3:   /* reset transmitter */
        c->tx_enabled = false;
        break;
    case 0xb:
        c->mr_ptr = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sc28c94: channel %d command 0x%x\n",
                      n, val >> 4);
        break;
    }
}

static uint64_t sc28c94_read(void *opaque, hwaddr addr, unsigned size)
{
    SC28C94State *s = opaque;
    int b = addr >> 4;
    int reg = addr & 0x0f;
    uint8_t val = 0xff;

    if (b >= SC28C94_BLOCKS) {
        val = reg == CIR_READ ? s->cir : s->global[reg];
    } else if ((reg & 0x07) < 4) {
        int n = b * 2 + (reg >> 3);
        SC28C94Channel *c = &s->ch[n];

        switch (reg & 0x03) {
        case 0:
            val = c->mr[c->mr_ptr];
            if (c->mr_ptr < 2) {
                c->mr_ptr++;
            }
            break;
        case 1:
            val = sc28c94_sr(c);
            break;
        case 3:
            val = sc28c94_pop(c);
            sc28c94_update_irq(s);
            break;
        }
    } else {
        switch (reg) {
        case 0x4:   /* IPCR: no input port changes */
            val = 0;
            break;
        case 0x5:
            val = sc28c94_isr(s, b);
            break;
        case 0x6:
            val = s->block[b].ctur;
            break;
        case 0x7:
            val = s->block[b].ctlr;
            break;
        case 0xc:
            val = s->block[b].ivr;
            break;
        case 0xd:   /* input port: undriven pins read high */
            val = 0xff;
            break;
        }
    }
    trace_sc28c94_read(addr, val);
    return val;
}

static void sc28c94_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    SC28C94State *s = opaque;
    int b = addr >> 4;
    int reg = addr & 0x0f;

    trace_sc28c94_write(addr, val);
    if (b >= SC28C94_BLOCKS) {
        s->global[reg] = val;
        if (reg == CIR_UPDATE) {
            sc28c94_update_cir(s);
        }
        return;
    }
    if ((reg & 0x07) < 4) {
        int n = b * 2 + (reg >> 3);
        SC28C94Channel *c = &s->ch[n];

        switch (reg & 0x03) {
        case 0:
            c->mr[c->mr_ptr] = val;
            if (c->mr_ptr < 2) {
                c->mr_ptr++;
            }
            break;
        case 1:
            c->csr = val;
            break;
        case 2:
            sc28c94_command(s, n, val);
            break;
        case 3:
            if (c->tx_enabled) {
                uint8_t ch = val;

                trace_sc28c94_tx(n, ch);
                /* Like other QEMU UARTs, wait for a slow backend. */
                qemu_chr_fe_write_all(&c->chr, &ch, 1);
            }
            break;
        }
    } else {
        switch (reg) {
        case 0x4:
            s->block[b].acr = val;
            break;
        case 0x5:
            s->block[b].imr = val;
            break;
        case 0x6:
            s->block[b].ctur = val;
            break;
        case 0x7:
            s->block[b].ctlr = val;
            break;
        case 0xc:
            s->block[b].ivr = val;
            break;
        case 0xd:
            s->block[b].opcr = val;
            break;
        default:    /* output port bit commands */
            break;
        }
    }
    sc28c94_update_irq(s);
}

static const MemoryRegionOps sc28c94_ops = {
    .read = sc28c94_read,
    .write = sc28c94_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static int sc28c94_can_receive(void *opaque)
{
    SC28C94Channel *c = opaque;

    return c->rx_enabled ? SC28C94_FIFO_SIZE - c->fifo_count : 0;
}

static void sc28c94_receive(void *opaque, const uint8_t *buf, int size)
{
    SC28C94Channel *c = opaque;
    int i;

    for (i = 0; i < size && c->fifo_count < SC28C94_FIFO_SIZE; i++) {
        c->fifo[(c->fifo_head + c->fifo_count) % SC28C94_FIFO_SIZE] = buf[i];
        c->fifo_count++;
    }
    sc28c94_update_irq(c->owner);
}

static void sc28c94_reset(DeviceState *dev)
{
    SC28C94State *s = SC28C94(dev);
    int i;

    for (i = 0; i < SC28C94_CHANNELS; i++) {
        SC28C94Channel *c = &s->ch[i];

        memset(c->mr, 0, sizeof(c->mr));
        c->mr_ptr = 1;
        c->csr = 0;
        c->rx_enabled = c->tx_enabled = false;
        c->fifo_head = c->fifo_count = 0;
    }
    for (i = 0; i < SC28C94_BLOCKS; i++) {
        memset(&s->block[i], 0, sizeof(s->block[i]));
        s->block[i].ivr = 0x0f;
    }
    memset(s->global, 0, sizeof(s->global));
    s->cir = 0;
    qemu_set_irq(s->irq, 0);
}

static void sc28c94_init(Object *obj)
{
    SC28C94State *s = SC28C94(obj);
    SysBusDevice *d = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &sc28c94_ops, s, "sc28c94",
                          SC28C94_MMIO_SIZE);
    sysbus_init_mmio(d, &s->mmio);
    sysbus_init_irq(d, &s->irq);
}

static void sc28c94_realize(DeviceState *dev, Error **errp)
{
    SC28C94State *s = SC28C94(dev);
    int i;

    for (i = 0; i < SC28C94_CHANNELS; i++) {
        s->ch[i].owner = s;
        qemu_chr_fe_set_handlers(&s->ch[i].chr, sc28c94_can_receive,
                                 sc28c94_receive, NULL, NULL, &s->ch[i],
                                 NULL, true);
    }
}

static const VMStateDescription vmstate_sc28c94_channel = {
    .name = "sc28c94-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mr, SC28C94Channel, 3),
        VMSTATE_UINT8(mr_ptr, SC28C94Channel),
        VMSTATE_UINT8(csr, SC28C94Channel),
        VMSTATE_BOOL(rx_enabled, SC28C94Channel),
        VMSTATE_BOOL(tx_enabled, SC28C94Channel),
        VMSTATE_UINT8_ARRAY(fifo, SC28C94Channel, SC28C94_FIFO_SIZE),
        VMSTATE_UINT8(fifo_head, SC28C94Channel),
        VMSTATE_UINT8(fifo_count, SC28C94Channel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sc28c94_block = {
    .name = "sc28c94-block",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(acr, SC28C94Block),
        VMSTATE_UINT8(imr, SC28C94Block),
        VMSTATE_UINT8(ctur, SC28C94Block),
        VMSTATE_UINT8(ctlr, SC28C94Block),
        VMSTATE_UINT8(ivr, SC28C94Block),
        VMSTATE_UINT8(opcr, SC28C94Block),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sc28c94 = {
    .name = TYPE_SC28C94,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, SC28C94State, SC28C94_CHANNELS, 1,
                             vmstate_sc28c94_channel, SC28C94Channel),
        VMSTATE_STRUCT_ARRAY(block, SC28C94State, SC28C94_BLOCKS, 1,
                             vmstate_sc28c94_block, SC28C94Block),
        VMSTATE_UINT8_ARRAY(global, SC28C94State, 16),
        VMSTATE_UINT8(cir, SC28C94State),
        VMSTATE_END_OF_LIST()
    }
};

static Property sc28c94_properties[] = {
    DEFINE_PROP_CHR("chardev0", SC28C94State, ch[0].chr),
    DEFINE_PROP_CHR("chardev1", SC28C94State, ch[1].chr),
    DEFINE_PROP_CHR("chardev2", SC28C94State, ch[2].chr),
    DEFINE_PROP_CHR("chardev3", SC28C94State, ch[3].chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void sc28c94_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sc28c94_realize;
    dc->reset = sc28c94_reset;
    dc->vmsd = &vmstate_sc28c94;
    device_class_set_props(dc, sc28c94_properties);
}

static const TypeInfo sc28c94_info = {
    .name = TYPE_SC28C94,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SC28C94State),
    .instance_init = sc28c94_init,
    .class_init = sc28c94_class_init,
};

static void sc28c94_register_types(void)
{
    type_register_static(&sc28c94_info);
}

type_init(sc28c94_register_types)
