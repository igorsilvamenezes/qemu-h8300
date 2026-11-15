/*
 * HP/Avago HDSP-211x eight character 5x7 intelligent alphanumeric display
 *
 * The display is a byte-wide bus peripheral.  Its register is selected by
 * the FL input and the address lines A4-A0.  This model expects FL on A5,
 * as on the E2300 board, which gives the following map:
 *
 *   0x00-0x07  flash RAM, bit 0 of each digit (FL low)
 *   0x20       UDC address register (selects one of 16 user characters)
 *   0x28-0x2E  UDC RAM, rows 0-6 of the selected user character (5 bits)
 *   0x30       control word: brightness (2-0), flash enable (3),
 *              blink enable (4), self test start (5), self test result (6),
 *              clear flash and character RAM (7)
 *   0x38-0x3F  character RAM: ASCII code, or 0x80 | n for user character n
 *
 * All registers can be read back; firmware uses this to save the character
 * under the cursor.  The self test completes immediately and passes.
 *
 * Every visible change is reported through the hdsp211x_display trace event
 * and, if the "chardev" property is set, as one text line per change, so a
 * test log shows what a person would read on the display.  Writes are
 * coalesced for one millisecond of virtual time so that a message written
 * character by character produces a single line.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/display/hdsp211x.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "trace.h"

#define CTRL_BRIGHTNESS   0x07
#define CTRL_FLASH        0x08
#define CTRL_BLINK        0x10
#define CTRL_SELF_TEST    0x20
#define CTRL_TEST_RESULT  0x40
#define CTRL_CLEAR        0x80

#define LOG_DELAY_NS      (1 * SCALE_MS)

/* Relative brightness for control word bits 2-0, from the data sheet. */
static const uint8_t brightness_pct[8] = { 100, 80, 53, 40, 27, 20, 13, 0 };

static void hdsp211x_clear(HDSP211XState *s)
{
    memset(s->ch, ' ', sizeof(s->ch));
    memset(s->flash, 0, sizeof(s->flash));
}

/* Printable view of the display: text, flashing digits and brightness. */
static void hdsp211x_render(HDSP211XState *s, char *text, char *flash)
{
    int i;

    for (i = 0; i < HDSP211X_DIGITS; i++) {
        uint8_t c = s->ch[i];

        if (c & 0x80) {
            text[i] = '#';                  /* user defined character */
        } else if (c < 0x20 || c == 0x7f) {
            text[i] = '.';                  /* special glyph of the font */
        } else {
            text[i] = c;
        }
        flash[i] = (s->ctrl & CTRL_FLASH) && (s->flash[i] & 1) ? '*' : '-';
    }
    text[HDSP211X_DIGITS] = '\0';
    flash[HDSP211X_DIGITS] = '\0';
}

static void hdsp211x_log(void *opaque)
{
    HDSP211XState *s = opaque;
    char text[HDSP211X_DIGITS + 1], flash[HDSP211X_DIGITS + 1];
    char state[sizeof(s->logged)];
    unsigned pct;

    hdsp211x_render(s, text, flash);
    pct = brightness_pct[s->ctrl & CTRL_BRIGHTNESS];
    snprintf(state, sizeof(state), "%s%s%c%c", text, flash,
             '0' + (s->ctrl & CTRL_BRIGHTNESS),
             s->ctrl & CTRL_BLINK ? 'b' : '-');
    if (!strcmp(state, s->logged)) {
        return;
    }
    pstrcpy(s->logged, sizeof(s->logged), state);

    trace_hdsp211x_display(text, flash, pct);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g_autofree char *line = g_strdup_printf(
            "[%5" PRId64 ".%06" PRId64 "] |%s| flash %s bright %3u%%%s\n",
            ns / (int64_t)NANOSECONDS_PER_SECOND,
            ns % (int64_t)NANOSECONDS_PER_SECOND / 1000, text, flash, pct,
            s->ctrl & CTRL_BLINK ? " blink" : "");

        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)line, strlen(line));
    }
}

static void hdsp211x_changed(HDSP211XState *s)
{
    timer_mod(s->log_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LOG_DELAY_NS);
}

static uint64_t hdsp211x_read(void *opaque, hwaddr addr, unsigned size)
{
    HDSP211XState *s = opaque;
    unsigned digit = addr & 7;
    uint8_t val;

    if (!(addr & 0x20)) {
        val = s->flash[digit] & 1;
    } else {
        switch (addr & 0x18) {
        case 0x00:
            val = s->udc_addr;
            break;
        case 0x08:
            val = digit < HDSP211X_ROWS ? s->udc[s->udc_addr][digit] : 0;
            break;
        case 0x10:
            val = s->ctrl;
            break;
        default:
            val = s->ch[digit];
            break;
        }
    }
    trace_hdsp211x_read(addr, val);
    return val;
}

static void hdsp211x_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    HDSP211XState *s = opaque;
    unsigned digit = addr & 7;

    trace_hdsp211x_write(addr, val);
    if (!(addr & 0x20)) {
        s->flash[digit] = val & 1;
        hdsp211x_changed(s);
        return;
    }

    switch (addr & 0x18) {
    case 0x00:
        s->udc_addr = val & (HDSP211X_UDCS - 1);
        break;
    case 0x08:
        if (digit < HDSP211X_ROWS) {
            s->udc[s->udc_addr][digit] = val & 0x1f;
        }
        break;
    case 0x10:
        s->ctrl = val & (CTRL_BRIGHTNESS | CTRL_FLASH | CTRL_BLINK);
        if (val & CTRL_CLEAR) {
            hdsp211x_clear(s);
        }
        if (val & CTRL_SELF_TEST) {
            /* The test takes about 4.5 s on the part; report a pass now. */
            s->ctrl |= CTRL_TEST_RESULT;
        }
        hdsp211x_changed(s);
        break;
    default:
        s->ch[digit] = val;
        hdsp211x_changed(s);
        break;
    }
}

static const MemoryRegionOps hdsp211x_ops = {
    .read = hdsp211x_read,
    .write = hdsp211x_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void hdsp211x_reset(DeviceState *dev)
{
    HDSP211XState *s = HDSP211X(dev);

    hdsp211x_clear(s);
    memset(s->udc, 0, sizeof(s->udc));
    s->udc_addr = 0;
    s->ctrl = 0;
    s->logged[0] = '\0';
    timer_del(s->log_timer);
}

static void hdsp211x_realize(DeviceState *dev, Error **errp)
{
    HDSP211XState *s = HDSP211X(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &hdsp211x_ops, s,
                          "hdsp-211x", HDSP211X_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    s->log_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hdsp211x_log, s);
}

static void hdsp211x_unrealize(DeviceState *dev)
{
    HDSP211XState *s = HDSP211X(dev);

    timer_free(s->log_timer);
}

static const VMStateDescription vmstate_hdsp211x = {
    .name = "hdsp-211x",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ch, HDSP211XState, HDSP211X_DIGITS),
        VMSTATE_UINT8_ARRAY(flash, HDSP211XState, HDSP211X_DIGITS),
        VMSTATE_UINT8_2DARRAY(udc, HDSP211XState, HDSP211X_UDCS,
                              HDSP211X_ROWS),
        VMSTATE_UINT8(udc_addr, HDSP211XState),
        VMSTATE_UINT8(ctrl, HDSP211XState),
        VMSTATE_END_OF_LIST()
    }
};

static Property hdsp211x_properties[] = {
    DEFINE_PROP_CHR("chardev", HDSP211XState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void hdsp211x_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = hdsp211x_realize;
    dc->unrealize = hdsp211x_unrealize;
    dc->reset = hdsp211x_reset;
    dc->vmsd = &vmstate_hdsp211x;
    dc->desc = "HDSP-211x 8 character intelligent alphanumeric display";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, hdsp211x_properties);
}

static const TypeInfo hdsp211x_info = {
    .name = TYPE_HDSP211X,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HDSP211XState),
    .class_init = hdsp211x_class_init,
};

static void hdsp211x_register_types(void)
{
    type_register_static(&hdsp211x_info);
}

type_init(hdsp211x_register_types)
