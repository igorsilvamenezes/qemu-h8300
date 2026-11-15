/*
 * H8/3003 on-chip supporting modules.
 *
 * Implements the interrupt controller (section 5), refresh controller
 * counter (section 7), DMAC (section 8), ITU (section 10), TPC
 * (section 11), A/D converter (section 14) and the bus controller
 * registers (section 6) of the H8/3003 Hardware Manual
 * (REJ09B0101 / ADE-602-055).  Watchdog, SCI and I/O ports are board
 * devices that overlay this register window.
 *
 * Timing follows QEMU_CLOCK_VIRTUAL; the ITU counts lazily between
 * compare-match/overflow events, which are delivered from timers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/h8300/h83003_onchip.h"

enum {
    UNIMP_SYNC = 1 << 0,
    UNIMP_PWM = 1 << 1,
    UNIMP_BUFFER = 1 << 2,
    UNIMP_CCLR_SYNC = 1 << 3,
    UNIMP_DMA_SCI = 1 << 4,
    UNIMP_DMA_DREQ = 1 << 5,
    UNIMP_TPC_NOV = 1 << 6,
    UNIMP_CMI = 1 << 7,
    UNIMP_DMA_INVALID = 1 << 8,
};

static void onchip_update(H83003OnChipState *s);

static void log_unimp_once(H83003OnChipState *s, uint32_t bit, const char *what)
{
    if (!(s->unimp_logged & bit)) {
        s->unimp_logged |= bit;
        qemu_log_mask(LOG_UNIMP, "h83003: %s not implemented\n", what);
    }
}

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* ------------------------------------------------------------------ */
/* Interrupt controller                                               */
/* ------------------------------------------------------------------ */

/* Bit of (IPRA << 8 | IPRB) that sets the priority of a vector. */
static int intc_ipr_bit(int vec)
{
    static const int8_t map[H83003_NR_VECTORS] = {
        [12] = 15, [13] = 14, [14] = 13, [15] = 13,
        [16] = 12, [17] = 12, [18] = 12, [19] = 12,
        [20] = 11, [21] = 11,
        [24] = 10, [25] = 10, [26] = 10,
        [28] = 9, [29] = 9, [30] = 9,
        [32] = 8, [33] = 8, [34] = 8,
        [36] = 7, [37] = 7, [38] = 7,
        [40] = 6, [41] = 6, [42] = 6,
        [44] = 5, [45] = 5, [46] = 5, [47] = 5,
        [48] = 4, [49] = 4, [50] = 4, [51] = 4,
        [52] = 3, [53] = 3, [54] = 3, [55] = 3,
        [56] = 2, [57] = 2, [58] = 2, [59] = 2,
        [60] = 1,
    };

    return map[vec] ? map[vec] : -1;
}

static uint64_t intc_pending(H83003OnChipState *s)
{
    uint64_t p = s->in_level;
    int i;

    for (i = 0; i < 5; i++) {
        p |= (uint64_t)(s->itu[i].tsr & s->itu[i].tier & 7) << (24 + 4 * i);
    }
    for (i = 0; i < 4; i++) {
        uint8_t a = s->dma[2 * i].dtcr, b = s->dma[2 * i + 1].dtcr;
        bool full = (a & 0x06) == 0x06;

        /* DEND is requested while DTE = 0 and DTIE = 1. */
        if (!(a & 0x80) && (a & 0x08)) {
            p |= 1ULL << (44 + 2 * i);
        }
        if (!full && !(b & 0x80) && (b & 0x08)) {
            p |= 1ULL << (45 + 2 * i);
        }
    }
    if ((s->adcsr & 0xc0) == 0xc0) {
        p |= 1ULL << 60;
    }
    if ((s->rtmcsr & 0xc0) == 0xc0) {
        p |= 1ULL << 21;
    }
    p |= (uint64_t)(s->isr & s->ier) << 12;
    return p;
}

static void intc_update(H83003OnChipState *s)
{
    uint64_t p = intc_pending(s) & ~((1ULL << 12) - 1);
    uint16_t ipr = s->ipra << 8 | s->iprb;
    int best = -1, best_pri = -1, vec;

    while (p) {
        int bit, pri;

        vec = ctz64(p);
        p &= p - 1;
        bit = intc_ipr_bit(vec);
        if (bit < 0) {
            continue;
        }
        pri = (ipr >> bit) & 1;
        if (pri > best_pri) {
            best = vec;
            best_pri = pri;
        }
    }
    if (best != s->cur_vec) {
        s->cur_vec = best;
        qemu_set_irq(s->cpu_irq, best < 0 ? 0 : (best_pri << 8) | best);
    }
}

/* Level inputs from board devices (WDT, SCI). */
static void onchip_irq_in(void *opaque, int n, int level)
{
    H83003OnChipState *s = opaque;

    s->in_level = deposit64(s->in_level, n, 1, level != 0);
    intc_update(s);
}

/* IRQ0-7 pins, 1 = asserted (the pins are active low on the chip). */
static void onchip_irq_pin(void *opaque, int n, int level)
{
    H83003OnChipState *s = opaque;
    bool old = (s->irq_pin >> n) & 1;

    s->irq_pin = deposit32(s->irq_pin, n, 1, level != 0);
    if ((s->iscr >> n) & 1) {
        if (level && !old) {
            s->isr |= 1 << n;
        }
    } else if (level) {
        s->isr |= 1 << n;
    }
    intc_update(s);
}

/* The CPU accepted the vector presented in cur_vec. */
static void onchip_ack(void *opaque, int n, int level)
{
    H83003OnChipState *s = opaque;
    int vec = s->cur_vec;

    if (!level) {
        return;
    }
    if (vec >= 12 && vec < 20 && ((s->iscr >> (vec - 12)) & 1)) {
        s->isr &= ~(1 << (vec - 12));
    }
    /* The CPU dropped its request; present any still-pending source. */
    s->cur_vec = -1;
    intc_update(s);
}

/* ------------------------------------------------------------------ */
/* TPC                                                                */
/* ------------------------------------------------------------------ */

static unsigned tpc_group_src(H83003OnChipState *s, int g)
{
    return (s->tpcr >> (2 * g)) & 3;
}

/* Compare match A of ITU channel k transfers NDR to the port pins. */
static void tpc_compare(H83003OnChipState *s, int k)
{
    int g, bit;

    for (g = 0; g < 4; g++) {
        uint8_t nder = g < 2 ? s->ndera : s->nderb;
        uint8_t ndr = g < 2 ? s->ndra : s->ndrb;
        int base = g < 2 ? 0 : 8;

        if (tpc_group_src(s, g) != k) {
            continue;
        }
        if ((s->tpmr >> g) & 1) {
            log_unimp_once(s, UNIMP_TPC_NOV, "TPC non-overlapping output");
        }
        for (bit = (g & 1) * 4; bit < (g & 1) * 4 + 4; bit++) {
            if ((nder >> bit) & 1) {
                qemu_set_irq(s->tpc_out[base + bit], (ndr >> bit) & 1);
            }
        }
    }
}

static uint8_t tpc_read(H83003OnChipState *s, unsigned off)
{
    switch (off) {
    case 0xa0:
        return s->tpmr | 0xf0;
    case 0xa1:
        return s->tpcr;
    case 0xa2:
        return s->nderb;
    case 0xa3:
        return s->ndera;
    case 0xa4:
    case 0xa6:
        return s->ndrb;
    default:
        return s->ndra;
    }
}

static void tpc_write(H83003OnChipState *s, unsigned off, uint8_t val)
{
    bool same_a = tpc_group_src(s, 0) == tpc_group_src(s, 1);
    bool same_b = tpc_group_src(s, 2) == tpc_group_src(s, 3);

    switch (off) {
    case 0xa0:
        s->tpmr = val | 0xf0;
        break;
    case 0xa1:
        s->tpcr = val;
        break;
    case 0xa2:
        s->nderb = val;
        break;
    case 0xa3:
        s->ndera = val;
        break;
    case 0xa4: /* NDRB, or group 3 when groups 2 and 3 differ */
        s->ndrb = same_b ? val : (s->ndrb & 0x0f) | (val & 0xf0);
        break;
    case 0xa5: /* NDRA, or group 1 when groups 0 and 1 differ */
        s->ndra = same_a ? val : (s->ndra & 0x0f) | (val & 0xf0);
        break;
    case 0xa6: /* group 2 */
        if (!same_b) {
            s->ndrb = (s->ndrb & 0xf0) | (val & 0x0f);
        }
        break;
    case 0xa7: /* group 0 */
        if (!same_a) {
            s->ndra = (s->ndra & 0xf0) | (val & 0x0f);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ITU                                                                */
/* ------------------------------------------------------------------ */

static const uint8_t itu_base[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };

enum { EV_A = 1, EV_B = 2, EV_WRAP = 4 };

static bool itu_decode(unsigned addr, int *ch, unsigned *off)
{
    int i;

    for (i = 0; i < 5; i++) {
        unsigned len = i < 3 ? 10 : 14;

        if (addr >= itu_base[i] && addr < itu_base[i] + len) {
            *ch = i;
            *off = addr - itu_base[i];
            return true;
        }
    }
    return false;
}

static uint32_t itu_div(H83003ITUChannel *c)
{
    unsigned tpsc = c->tcr & 7;

    /* TPSC 1xx selects the TCLKA-D pins, which are not connected. */
    return tpsc < 4 ? 1u << tpsc : 0;
}

static bool itu_running(H83003OnChipState *s, H83003ITUChannel *c)
{
    return ((s->tstr >> c->index) & 1) && itu_div(c);
}

static int64_t itu_ticks_ns(H83003OnChipState *s, H83003ITUChannel *c,
                            uint64_t ticks)
{
    return muldiv64(ticks * itu_div(c), NANOSECONDS_PER_SECOND, s->freq);
}

static uint64_t itu_ns_ticks(H83003OnChipState *s, H83003ITUChannel *c,
                             int64_t ns)
{
    return muldiv64(ns, s->freq, NANOSECONDS_PER_SECOND) / itu_div(c);
}

/* GRA (b = false) or GRB (b = true) works as an output compare register. */
static bool itu_compare(H83003ITUChannel *c, bool b)
{
    return !(c->tior & (b ? 0x40 : 0x04));
}

/* Distance in counts from tcnt to the next compare match or wrap. */
static uint32_t itu_next_event(H83003OnChipState *s, H83003ITUChannel *c,
                               unsigned *ev, bool *ovf)
{
    unsigned cclr = (c->tcr >> 5) & 3;
    uint32_t cnt = c->tcnt, top = 0xffff, end, d;
    bool clear = false;

    if (cclr == 1 && itu_compare(c, false)) {
        top = c->gra;
        clear = true;
    } else if (cclr == 2 && itu_compare(c, true)) {
        top = c->grb;
        clear = true;
    } else if (cclr == 3) {
        log_unimp_once(s, UNIMP_CCLR_SYNC, "ITU synchronous clearing");
    }
    if (cnt <= top) {
        end = top;
    } else {
        end = 0xffff;
        clear = false;
    }
    *ovf = !clear;
    d = end - cnt + 1;
    *ev = EV_WRAP;
    if (itu_compare(c, false) && c->gra > cnt && c->gra <= end) {
        d = c->gra - cnt;
        *ev = EV_A;
    }
    if (itu_compare(c, true) && c->grb > cnt && c->grb <= end) {
        if (c->grb - cnt < d) {
            d = c->grb - cnt;
            *ev = EV_B;
        } else if (c->grb - cnt == d) {
            *ev |= EV_B;
        }
    }
    return d;
}

static void itu_apply(H83003OnChipState *s, H83003ITUChannel *c, uint32_t d,
                      unsigned ev, bool ovf)
{
    if (ev & EV_WRAP) {
        c->tcnt = 0;
        if (ovf) {
            c->tsr |= 0x04;
        }
        if (itu_compare(c, false) && c->gra == 0) {
            ev |= EV_A;
        }
        if (itu_compare(c, true) && c->grb == 0) {
            ev |= EV_B;
        }
    } else {
        c->tcnt += d;
    }
    if (ev & EV_A) {
        c->tsr |= 0x01;
        if (c->index < 4) {
            tpc_compare(s, c->index);
        }
    }
    if (ev & EV_B) {
        c->tsr |= 0x02;
    }
}

static void itu_schedule(H83003OnChipState *s, H83003ITUChannel *c)
{
    unsigned ev;
    bool ovf;
    uint32_t d;

    if (c->in_sync) {
        return;
    }
    if (!itu_running(s, c)) {
        timer_del(&c->timer);
        return;
    }
    d = itu_next_event(s, c, &ev, &ovf);
    timer_mod(&c->timer, c->anchor_ns + itu_ticks_ns(s, c, d) + 1);
}

/* Process every event up to now; tcnt stays the count at anchor_ns. */
static void itu_sync(H83003OnChipState *s, H83003ITUChannel *c, int64_t now)
{
    if (c->in_sync) {
        return;
    }
    c->in_sync = true;
    while (itu_running(s, c)) {
        unsigned ev;
        bool ovf;
        uint32_t d = itu_next_event(s, c, &ev, &ovf);
        int64_t t = c->anchor_ns + itu_ticks_ns(s, c, d) + 1;

        if (t > now) {
            break;
        }
        itu_apply(s, c, d, ev, ovf);
        c->anchor_ns = t;
        /* Activate the DMAC at the event, before the next one. */
        onchip_update(s);
    }
    c->in_sync = false;
    itu_schedule(s, c);
}

static uint16_t itu_count(H83003OnChipState *s, H83003ITUChannel *c,
                          int64_t now)
{
    if (c->in_sync || !itu_running(s, c)) {
        return c->tcnt;
    }
    return c->tcnt + itu_ns_ticks(s, c, now - c->anchor_ns);
}

/* Move the anchor to now before a register write changes the timing. */
static void itu_rebase(H83003OnChipState *s, H83003ITUChannel *c, int64_t now)
{
    if (c->in_sync) {
        return;
    }
    c->tcnt = itu_count(s, c, now);
    c->anchor_ns = now;
}

static void itu_timer_cb(void *opaque)
{
    H83003ITUChannel *c = opaque;

    itu_sync(c->s, c, now_ns());
}

static uint16_t itu_read16(H83003OnChipState *s, int ch, unsigned off)
{
    H83003ITUChannel *c = &s->itu[ch];
    int64_t now = now_ns();

    switch (off & ~1) {
    case 4:
        itu_sync(s, c, now);
        return itu_count(s, c, now);
    case 6:
        return c->gra;
    case 8:
        return c->grb;
    case 10:
        return c->bra;
    default:
        return c->brb;
    }
}

static uint8_t itu_read8(H83003OnChipState *s, int ch, unsigned off)
{
    H83003ITUChannel *c = &s->itu[ch];

    switch (off) {
    case 0:
        return c->tcr;
    case 1:
        return c->tior;
    case 2:
        return c->tier;
    case 3:
        itu_sync(s, c, now_ns());
        c->read_tsr = c->tsr;
        return c->tsr | 0xf8;
    default:
        return off & 1 ? itu_read16(s, ch, off) : itu_read16(s, ch, off) >> 8;
    }
}

static void itu_write16(H83003OnChipState *s, int ch, unsigned off,
                        uint16_t val)
{
    H83003ITUChannel *c = &s->itu[ch];
    int64_t now = now_ns();

    itu_sync(s, c, now);
    itu_rebase(s, c, now);
    switch (off & ~1) {
    case 4:
        c->tcnt = val;
        break;
    case 6:
        c->gra = val;
        break;
    case 8:
        c->grb = val;
        break;
    case 10:
        c->bra = val;
        break;
    default:
        c->brb = val;
        break;
    }
    itu_schedule(s, c);
}

static void itu_write8(H83003OnChipState *s, int ch, unsigned off, uint8_t val)
{
    H83003ITUChannel *c = &s->itu[ch];
    int64_t now = now_ns();
    uint16_t v;

    switch (off) {
    case 0:
    case 1:
        itu_sync(s, c, now);
        itu_rebase(s, c, now);
        if (off == 0) {
            c->tcr = val | 0x80;
        } else {
            c->tior = val | 0x88;
        }
        itu_schedule(s, c);
        break;
    case 2:
        c->tier = val | 0xf8;
        onchip_update(s);
        break;
    case 3:
        /* A flag is cleared by writing 0 after reading it as 1. */
        itu_sync(s, c, now);
        c->tsr &= ~(c->read_tsr & ~val & 0x07);
        c->read_tsr &= val;
        onchip_update(s);
        break;
    default:
        v = itu_read16(s, ch, off);
        v = off & 1 ? (v & 0xff00) | val : (v & 0x00ff) | (val << 8);
        itu_write16(s, ch, off, v);
        break;
    }
}

static void itu_write_tstr(H83003OnChipState *s, uint8_t val)
{
    int64_t now = now_ns();
    int i;

    for (i = 0; i < 5; i++) {
        itu_sync(s, &s->itu[i], now);
        itu_rebase(s, &s->itu[i], now);
    }
    s->tstr = val | 0xe0;
    for (i = 0; i < 5; i++) {
        itu_schedule(s, &s->itu[i]);
    }
    onchip_update(s);
}

/* ------------------------------------------------------------------ */
/* DMAC                                                               */
/* ------------------------------------------------------------------ */

static void dma_move(uint32_t src, uint32_t dst, bool word)
{
    uint8_t buf[2];
    int len = word ? 2 : 1;

    src &= word ? 0xfffffe : 0xffffff;
    dst &= word ? 0xfffffe : 0xffffff;
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

/* IMIAn interrupt request (flag and enable) from ITU channel n. */
static bool dma_imia_request(H83003OnChipState *s, int n)
{
    return s->itu[n].tsr & s->itu[n].tier & 0x01;
}

/* One transfer of a short address mode channel (I/O, idle, repeat). */
static void dma_short_transfer(H83003DMAChannel *c, bool rx)
{
    uint8_t dt = c->dtcr;
    bool word = dt & 0x40;
    int32_t step = (word ? 2 : 1) * ((dt & 0x20) ? -1 : 1);
    uint32_t io = 0xffff00 | c->ioar;

    if (rx) {
        dma_move(io, c->mar, word);
    } else {
        dma_move(c->mar, io, word);
    }
    if (!(dt & 0x10)) {                 /* I/O mode */
        c->mar = (c->mar + step) & 0xffffff;
        if (--c->etcr == 0) {
            c->dtcr &= ~0x80;
        }
    } else if (dt & 0x08) {             /* idle mode */
        if (--c->etcr == 0) {
            c->dtcr &= ~0x80;
        }
    } else {                            /* repeat mode */
        uint8_t h = c->etcr >> 8, l = c->etcr;

        c->mar = (c->mar + step) & 0xffffff;
        if (--h == 0) {
            h = l;
            c->mar = (c->mar - step * (l ? l : 256)) & 0xffffff;
        }
        c->etcr = h << 8 | l;
    }
}

/* Full address mode channel n (A = control/source, B = destination). */
static void dma_full_unit(H83003DMAChannel *a, H83003DMAChannel *b,
                          int32_t sstep, int32_t dstep, bool word)
{
    dma_move(a->mar, b->mar, word);
    a->mar = (a->mar + sstep) & 0xffffff;
    b->mar = (b->mar + dstep) & 0xffffff;
}

/* Returns the ITU channel whose IMIA request this channel consumed. */
static int dma_full_service(H83003OnChipState *s, int n)
{
    H83003DMAChannel *a = &s->dma[2 * n], *b = &s->dma[2 * n + 1];
    bool word = a->dtcr & 0x40;
    int32_t unit = word ? 2 : 1;
    int32_t sstep = (a->dtcr & 0x10) ? ((a->dtcr & 0x20) ? -unit : unit) : 0;
    int32_t dstep = (b->dtcr & 0x10) ? ((b->dtcr & 0x20) ? -unit : unit) : 0;
    unsigned dts = b->dtcr & 7;

    if (!(a->dtcr & 0x80) || !(b->dtcr & 0x80)) {
        return -1;
    }
    if (!(a->dtcr & 0x01)) {
        /* Normal mode: auto-request completes at once. */
        if (dts == 0 || dts == 2) {
            do {
                dma_full_unit(a, b, sstep, dstep, word);
            } while (--a->etcr != 0);
            a->dtcr &= ~0x80;
        } else if (dts >= 6) {
            log_unimp_once(s, UNIMP_DMA_DREQ, "DMAC DREQ activation");
        } else {
            log_unimp_once(s, UNIMP_DMA_INVALID, "DMAC invalid DTS setting");
        }
        return -1;
    }
    /* Block transfer mode */
    if (dts < 4) {
        uint8_t h = a->etcr >> 8, l = a->etcr;
        unsigned count = h ? h : 256, i;

        if (!dma_imia_request(s, dts)) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            dma_full_unit(a, b, sstep, dstep, word);
        }
        /* The block area address returns to its initial value. */
        if (b->dtcr & 0x08) {
            a->mar = (a->mar - sstep * (int32_t)count) & 0xffffff;
        } else {
            b->mar = (b->mar - dstep * (int32_t)count) & 0xffffff;
        }
        a->etcr = l << 8 | l;
        if (--b->etcr == 0) {
            a->dtcr &= ~0x80;
        }
        return dts;
    }
    if (dts == 6) {
        log_unimp_once(s, UNIMP_DMA_DREQ, "DMAC DREQ activation");
    } else {
        log_unimp_once(s, UNIMP_DMA_INVALID, "DMAC invalid DTS setting");
    }
    return -1;
}

static void dmac_service(H83003OnChipState *s)
{
    unsigned consumed = 0;
    int n, k, i;

    for (n = 0; n < 4; n++) {
        if ((s->dma[2 * n].dtcr & 0x06) == 0x06) {
            k = dma_full_service(s, n);
            if (k >= 0) {
                consumed |= 1 << k;
            }
            continue;
        }
        for (i = 2 * n; i < 2 * n + 2; i++) {
            H83003DMAChannel *c = &s->dma[i];
            unsigned dts = c->dtcr & 7;

            if (!(c->dtcr & 0x80)) {
                continue;
            }
            if (dts < 4) {
                if (dma_imia_request(s, dts)) {
                    dma_short_transfer(c, false);
                    consumed |= 1 << dts;
                }
            } else if (dts < 6) {
                log_unimp_once(s, UNIMP_DMA_SCI, "DMAC activation by SCI");
            } else if (i & 1) {
                log_unimp_once(s, UNIMP_DMA_DREQ, "DMAC DREQ activation");
            }
        }
    }
    /* An interrupt that activated the DMAC never reaches the CPU. */
    for (k = 0; k < 4; k++) {
        if (consumed & (1 << k)) {
            s->itu[k].tsr &= ~0x01;
        }
    }
}

static void onchip_update(H83003OnChipState *s)
{
    if (s->updating) {
        s->rescan = true;
        return;
    }
    s->updating = true;
    do {
        s->rescan = false;
        dmac_service(s);
    } while (s->rescan);
    s->updating = false;
    intc_update(s);
}

static uint8_t dma_read8(H83003OnChipState *s, unsigned addr)
{
    H83003DMAChannel *c = &s->dma[(addr - 0x20) >> 3];

    switch (addr & 7) {
    case 0:
        return 0xff;
    case 1:
        return c->mar >> 16;
    case 2:
        return c->mar >> 8;
    case 3:
        return c->mar;
    case 4:
        return c->etcr >> 8;
    case 5:
        return c->etcr;
    case 6:
        return c->ioar;
    default:
        return c->dtcr;
    }
}

static void dma_write8(H83003OnChipState *s, unsigned addr, uint8_t val)
{
    H83003DMAChannel *c = &s->dma[(addr - 0x20) >> 3];

    switch (addr & 7) {
    case 0:
        break;
    case 1:
        c->mar = deposit32(c->mar, 16, 8, val);
        break;
    case 2:
        c->mar = deposit32(c->mar, 8, 8, val);
        break;
    case 3:
        c->mar = deposit32(c->mar, 0, 8, val);
        break;
    case 4:
        c->etcr = deposit32(c->etcr, 8, 8, val);
        break;
    case 5:
        c->etcr = deposit32(c->etcr, 0, 8, val);
        break;
    case 6:
        c->ioar = val;
        break;
    default:
        c->dtcr = val;
        onchip_update(s);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Refresh controller: RTCNT/RTCOR compare match, without the CMI.    */
/* ------------------------------------------------------------------ */

static uint32_t rt_div(H83003OnChipState *s)
{
    static const uint16_t div[8] = { 0, 2, 8, 32, 128, 512, 2048, 4096 };

    return div[(s->rtmcsr >> 3) & 7];
}

static void rt_sync(H83003OnChipState *s, int64_t now)
{
    uint64_t ticks, v;

    if (!rt_div(s)) {
        s->rt_anchor_ns = now;
        return;
    }
    ticks = muldiv64(now - s->rt_anchor_ns, s->freq, NANOSECONDS_PER_SECOND)
            / rt_div(s);
    if (!ticks) {
        return;
    }
    s->rt_anchor_ns += muldiv64(ticks * rt_div(s), NANOSECONDS_PER_SECOND,
                                s->freq);
    v = s->rtcnt + ticks;
    if (s->rtcnt <= s->rtcor && v > s->rtcor) {
        s->rtmcsr |= 0x80;
        v = (v - s->rtcor - 1) % (s->rtcor + 1u);
    }
    s->rtcnt = v;
}

/* ------------------------------------------------------------------ */
/* A/D converter                                                      */
/* ------------------------------------------------------------------ */

static int64_t ad_conv_ns(H83003OnChipState *s)
{
    return muldiv64(s->adcsr & 0x08 ? 134 : 266, NANOSECONDS_PER_SECOND,
                    s->freq);
}

static void ad_timer_cb(void *opaque)
{
    H83003OnChipState *s = opaque;
    int ch = s->ad_ch;

    if (!(s->adcsr & 0x20)) {
        return;
    }
    s->addr[ch & 3] = s->an[ch] & 0x3ff;
    if (!(s->adcsr & 0x10)) {
        s->adcsr = (s->adcsr | 0x80) & ~0x20;
    } else {
        int last = (s->adcsr & 0x04) + (s->adcsr & 0x03);

        if (ch >= last) {
            s->adcsr |= 0x80;
            ch = s->adcsr & 0x04;
        } else {
            ch++;
        }
        s->ad_ch = ch;
        timer_mod(&s->ad_timer, now_ns() + ad_conv_ns(s));
    }
    onchip_update(s);
}

static void ad_write_adcsr(H83003OnChipState *s, uint8_t val)
{
    bool was = s->adcsr & 0x20;
    uint8_t adf = s->adcsr & 0x80 & (val | ~s->read_adcsr);

    s->adcsr = adf | (val & 0x7f);
    s->read_adcsr &= val;
    if (!(val & 0x20)) {
        timer_del(&s->ad_timer);
    } else if (!was) {
        s->ad_ch = (val & 0x10) ? (val & 0x04) : (val & 0x07);
        timer_mod(&s->ad_timer, now_ns() + ad_conv_ns(s));
    }
    onchip_update(s);
}

/* ------------------------------------------------------------------ */
/* Register window                                                    */
/* ------------------------------------------------------------------ */

static uint8_t onchip_read8(H83003OnChipState *s, unsigned addr)
{
    int ch;
    unsigned off;

    if (addr >= 0x20 && addr < 0x60) {
        return dma_read8(s, addr);
    }
    if (itu_decode(addr, &ch, &off)) {
        return itu_read8(s, ch, off);
    }
    switch (addr) {
    case 0x60:
        return s->tstr;
    case 0x61:
        return s->tsnc;
    case 0x62:
        return s->tmdr;
    case 0x63:
        return s->tfcr;
    case 0x90:
        return s->toer;
    case 0x91:
        return s->tocr;
    case 0xa0 ... 0xa7:
        return tpc_read(s, addr);
    case 0xac:
        return s->rfshcr;
    case 0xad:
        rt_sync(s, now_ns());
        return s->rtmcsr;
    case 0xae:
        rt_sync(s, now_ns());
        return s->rtcnt;
    case 0xaf:
        return s->rtcor;
    case 0xe0 ... 0xe7:
        return addr & 1 ? (s->addr[(addr - 0xe0) >> 1] & 3) << 6
                        : s->addr[(addr - 0xe0) >> 1] >> 2;
    case 0xe8:
        s->read_adcsr = s->adcsr;
        return s->adcsr;
    case 0xe9:
        return s->adcr;
    case 0xec:
        return s->abwcr;
    case 0xed:
        return s->astcr;
    case 0xee:
        return s->wcr;
    case 0xef:
        return s->wcer;
    case 0xf3:
        return s->brcr;
    case 0xf4:
        return s->iscr;
    case 0xf5:
        return s->ier;
    case 0xf6:
        s->read_isr = s->isr;
        return s->isr;
    case 0xf8:
        return s->ipra;
    case 0xf9:
        return s->iprb;
    default:
        qemu_log_mask(LOG_UNIMP, "h83003: read of unimplemented register "
                      "0x%06x\n", H83003_ONCHIP_BASE + addr);
        return 0xff;
    }
}

static void onchip_write8(H83003OnChipState *s, unsigned addr, uint8_t val)
{
    int ch;
    unsigned off;
    int64_t now;

    if (addr >= 0x20 && addr < 0x60) {
        dma_write8(s, addr, val);
        return;
    }
    if (itu_decode(addr, &ch, &off)) {
        itu_write8(s, ch, off, val);
        return;
    }
    switch (addr) {
    case 0x60:
        itu_write_tstr(s, val);
        break;
    case 0x61:
        s->tsnc = val | 0xe0;
        if (val & 0x1f) {
            log_unimp_once(s, UNIMP_SYNC, "ITU synchronous operation");
        }
        break;
    case 0x62:
        s->tmdr = val | 0x80;
        if (val & 0x5f) {
            log_unimp_once(s, UNIMP_PWM, "ITU PWM/phase counting modes");
        }
        break;
    case 0x63:
        s->tfcr = val | 0xc0;
        if (val & 0x3f) {
            log_unimp_once(s, UNIMP_BUFFER, "ITU buffer/complementary modes");
        }
        break;
    case 0x90:
        s->toer = val | 0xc0;
        break;
    case 0x91:
        s->tocr = val | 0xe0;
        break;
    case 0xa0 ... 0xa7:
        tpc_write(s, addr, val);
        break;
    case 0xac:
        s->rfshcr = val | 0x02;
        break;
    case 0xad:
        now = now_ns();
        rt_sync(s, now);
        s->rtmcsr = (s->rtmcsr & val & 0x80) | (val & 0x78) | 0x07;
        s->rt_anchor_ns = now;
        if (val & 0x40) {
            log_unimp_once(s, UNIMP_CMI, "refresh controller CMI interrupt");
        }
        onchip_update(s);
        break;
    case 0xae:
        rt_sync(s, now_ns());
        s->rtcnt = val;
        break;
    case 0xaf:
        rt_sync(s, now_ns());
        s->rtcor = val;
        break;
    case 0xe0 ... 0xe7:
        break;
    case 0xe8:
        ad_write_adcsr(s, val);
        break;
    case 0xe9:
        s->adcr = val | 0x7f;
        break;
    case 0xec:
        s->abwcr = val;
        break;
    case 0xed:
        s->astcr = val;
        break;
    case 0xee:
        s->wcr = val | 0xf0;
        break;
    case 0xef:
        s->wcer = val;
        break;
    case 0xf3:
        s->brcr = val | 0xfe;
        break;
    case 0xf4:
        s->iscr = val;
        break;
    case 0xf5:
        s->ier = val;
        intc_update(s);
        break;
    case 0xf6:
        /* Level-sensitive pins keep their flag while asserted. */
        s->isr &= ~(s->read_isr & ~val);
        s->read_isr &= val;
        s->isr |= s->irq_pin & ~s->iscr;
        intc_update(s);
        break;
    case 0xf8:
        s->ipra = val;
        intc_update(s);
        break;
    case 0xf9:
        s->iprb = val & 0xfe;
        intc_update(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "h83003: write of unimplemented register "
                      "0x%06x = 0x%02x\n", H83003_ONCHIP_BASE + addr, val);
        break;
    }
}

static bool onchip_is_itu16(unsigned addr, int *ch, unsigned *off)
{
    return itu_decode(addr, ch, off) && *off >= 4 && !(*off & 1);
}

static uint64_t onchip_read(void *opaque, hwaddr addr, unsigned size)
{
    H83003OnChipState *s = opaque;
    int ch;
    unsigned off;

    if (size == 2) {
        if (onchip_is_itu16(addr, &ch, &off)) {
            return itu_read16(s, ch, off);
        }
        return onchip_read8(s, addr) << 8 | onchip_read8(s, addr + 1);
    }
    return onchip_read8(s, addr);
}

static void onchip_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    H83003OnChipState *s = opaque;
    int ch;
    unsigned off;

    if (size == 2) {
        if (onchip_is_itu16(addr, &ch, &off)) {
            itu_write16(s, ch, off, val);
            onchip_update(s);
            return;
        }
        onchip_write8(s, addr, val >> 8);
        onchip_write8(s, addr + 1, val);
        return;
    }
    onchip_write8(s, addr, val);
}

static const MemoryRegionOps onchip_ops = {
    .read = onchip_read,
    .write = onchip_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

static void onchip_reset(DeviceState *dev)
{
    H83003OnChipState *s = H83003_ONCHIP(dev);
    int64_t now = now_ns();
    int i;

    s->in_level = 0;
    s->irq_pin = 0;
    s->iscr = s->ier = s->isr = s->read_isr = 0;
    s->ipra = s->iprb = 0;

    s->tstr = 0xe0;
    s->tsnc = 0xe0;
    s->tmdr = 0x80;
    s->tfcr = 0xc0;
    s->toer = 0xff;
    s->tocr = 0xff;
    for (i = 0; i < 5; i++) {
        H83003ITUChannel *c = &s->itu[i];

        timer_del(&c->timer);
        c->tcr = 0x80;
        c->tior = 0x88;
        c->tier = 0xf8;
        c->tsr = c->read_tsr = 0;
        c->tcnt = 0;
        c->gra = c->grb = c->bra = c->brb = 0xffff;
        c->anchor_ns = now;
        c->in_sync = false;
    }
    for (i = 0; i < 8; i++) {
        s->dma[i].dtcr = 0;
    }
    s->updating = s->rescan = false;

    s->tpmr = 0xf0;
    s->tpcr = 0xff;
    s->nderb = s->ndera = 0;
    s->ndrb = s->ndra = 0;

    s->rfshcr = 0x02;
    s->rtmcsr = 0x07;
    s->rtcnt = 0;
    s->rtcor = 0xff;
    s->rt_anchor_ns = now;

    timer_del(&s->ad_timer);
    s->adcsr = s->read_adcsr = 0;
    s->adcr = 0x7f;
    memset(s->addr, 0, sizeof(s->addr));

    s->abwcr = 0xff;
    s->astcr = 0xff;
    s->wcr = 0xf3;
    s->wcer = 0xff;
    s->brcr = 0xfe;

    s->cur_vec = -1;
    qemu_set_irq(s->cpu_irq, 0);
}

static void onchip_realize(DeviceState *dev, Error **errp)
{
    H83003OnChipState *s = H83003_ONCHIP(dev);
    int i;

    if (!s->freq) {
        error_setg(errp, "h83003-onchip: clock-frequency must be set");
        return;
    }
    for (i = 0; i < 5; i++) {
        s->itu[i].s = s;
        s->itu[i].index = i;
        timer_init_ns(&s->itu[i].timer, QEMU_CLOCK_VIRTUAL, itu_timer_cb,
                      &s->itu[i]);
    }
    timer_init_ns(&s->ad_timer, QEMU_CLOCK_VIRTUAL, ad_timer_cb, s);
}

/*
 * Analog input pin AN<n> driven by another device: the level is the
 * conversion result for that voltage, 0 to 0x3ff (a fraction of AVref).
 */
static void onchip_analog_in(void *opaque, int n, int level)
{
    H83003OnChipState *s = opaque;

    s->an[n] = MIN(MAX(level, 0), 0x3ff);
}

static void onchip_init(Object *obj)
{
    H83003OnChipState *s = H83003_ONCHIP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &onchip_ops, s, "h83003-onchip",
                          H83003_ONCHIP_SIZE);
    /* DMAC transfers write the ITU and TPC registers of this device. */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->cpu_irq);
    qdev_init_gpio_in(dev, onchip_irq_in, H83003_NR_VECTORS);
    qdev_init_gpio_in_named(dev, onchip_irq_pin, "irq-pin", 8);
    qdev_init_gpio_in_named(dev, onchip_ack, "ack", 1);
    qdev_init_gpio_in_named(dev, onchip_analog_in, "an", H83003_AN_LINES);
    qdev_init_gpio_out_named(dev, s->tpc_out, "tpc", H83003_TPC_LINES);
}

static Property onchip_properties[] = {
    DEFINE_PROP_UINT64("clock-frequency", H83003OnChipState, freq, 16000000),
    DEFINE_PROP_UINT16("an0", H83003OnChipState, an[0], 0x200),
    DEFINE_PROP_UINT16("an1", H83003OnChipState, an[1], 0x200),
    DEFINE_PROP_UINT16("an2", H83003OnChipState, an[2], 0x200),
    DEFINE_PROP_UINT16("an3", H83003OnChipState, an[3], 0x200),
    DEFINE_PROP_UINT16("an4", H83003OnChipState, an[4], 0x200),
    DEFINE_PROP_UINT16("an5", H83003OnChipState, an[5], 0x200),
    DEFINE_PROP_UINT16("an6", H83003OnChipState, an[6], 0x200),
    DEFINE_PROP_UINT16("an7", H83003OnChipState, an[7], 0x200),
    DEFINE_PROP_END_OF_LIST(),
};

static void onchip_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = onchip_realize;
    dc->reset = onchip_reset;
    dc->user_creatable = false;
    device_class_set_props(dc, onchip_properties);
}

static const TypeInfo onchip_info = {
    .name = TYPE_H83003_ONCHIP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(H83003OnChipState),
    .instance_init = onchip_init,
    .class_init = onchip_class_init,
};

static void onchip_register_types(void)
{
    type_register_static(&onchip_info);
}

type_init(onchip_register_types)
