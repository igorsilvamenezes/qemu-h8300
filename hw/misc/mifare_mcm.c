/*
 * Mikron/Philips MIFARE Core Module (MCM) with a virtual MIFARE Classic 1K card
 *
 * The MCM (MCM200/MCM500/MCM520) is a byte-wide bus peripheral that drives
 * the 13.56 MHz field and handles the MIFARE bit coding, parity, CRC and the
 * three-pass authentication with keys held in its own key RAM.  The host
 * writes a command frame into DATA, the module sends it to the card and
 * places the answer in a FIFO, and STACON reports the result.
 *
 * Register map (offset from the module base, 0x810010 on the E2300):
 *
 *   0x0 DATA       write: frame byte to send / read: answer byte
 *   0x1 STACON     write: SOR(7) HF(6) NRF(1, field off) AC(0, anticollision)
 *                  read:  DV(7) TE(6, timeout) PE(5) CE(4) BE(3) AE(2)
 *   0x2 ENABLE     PEN PRE CEN CRE (parity and CRC enables)
 *   0x3 BCNTS      number of bits to send; the frame goes out once this
 *                  many bits have been written to DATA
 *   0x4 BCNTR      number of bits expected back
 *   0x5 BAUDRATE   0x6 TOC (timeout counter)   0x7 MODE
 *   0x8 CRCDATA    0x9 CRCSTACON (CRC coprocessor, not modelled)
 *   0xA KEYDATA    6 bytes transport key followed by 6 bytes new key
 *   0xB KEYSTACON  keyset (1-0), service key (6)
 *   0xC KEYADR     AUTH(7) KEYB(6) sector (3-0)
 *   0xE RCODE
 *
 * The register semantics follow the Mikron driver used by the E2300
 * application (MIKRON.C); no data sheet was available.  Frames complete
 * immediately: DV is set as soon as the last bit has been written and is
 * cleared when the host has read the whole answer.  When no card answers,
 * STACON reads DV|TE.
 *
 * The card is a MIFARE Classic 1K.  Its contents come from the "card"
 * property (a raw 1024 byte dump, block 0 first) or, without a file, from a
 * blank card built from the "uid" property with transport keys
 * FF FF FF FF FF FF.  Authentication compares the key in the MCM key RAM
 * with the sector trailer unless "check-keys" is off.  With "writeback" on,
 * every block written by the firmware is saved back to the file.  The
 * "present" property places or removes the card and can be changed while
 * the machine runs, e.g. "qom-set /machine/mifare present true".
 *
 * Every card operation is reported through the mifare_mcm_event trace event.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/mifare_mcm.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

enum {
    R_DATA = 0x0, R_STACON, R_ENABLE, R_BCNTS, R_BCNTR, R_BAUDRATE, R_TOC,
    R_MODE, R_CRCDATA, R_CRCSTACON, R_KEYDATA, R_KEYSTACON, R_KEYADR,
    R_RCODE = 0xe,
};

/* STACON read */
#define ST_DV   0x80
#define ST_TE   0x40
#define ST_PE   0x20
#define ST_CE   0x10
#define ST_BE   0x08
#define ST_AE   0x04
/* STACON write */
#define CTL_SOR 0x80
#define CTL_NRF 0x02
#define CTL_AC  0x01
/* KEYADR / KEYSTACON */
#define KEY_AUTH    0x80
#define KEY_B       0x40
#define KEY_SERVICE 0x40

/* Four-bit answers of the card */
#define MIFARE_ACK  0x0a
#define MIFARE_NAK  0x04

enum {
    CARD_OFF, CARD_IDLE, CARD_READY, CARD_ACTIVE, CARD_HALT,
};

static const char *const card_state_name[] = {
    "off", "idle", "ready", "active", "halt",
};

static void G_GNUC_PRINTF(2, 3) mcm_event(MifareMCMState *s,
                                          const char *fmt, ...)
{
    va_list ap;
    g_autofree char *msg = NULL;

    if (!trace_event_get_state_backends(TRACE_MIFARE_MCM_EVENT)) {
        return;
    }
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    trace_mifare_mcm_event(msg);
}

static char *hex(const uint8_t *buf, size_t len)
{
    GString *str = g_string_sized_new(len * 3);
    size_t i;

    for (i = 0; i < len; i++) {
        g_string_append_printf(str, "%s%02x", i ? " " : "", buf[i]);
    }
    return g_string_free(str, false);
}

static bool mcm_rf_on(MifareMCMState *s)
{
    return !(s->stacon & CTL_NRF);
}

/* The card powers up, or goes away, with the field or with "present". */
static void mcm_card_power(MifareMCMState *s)
{
    s->card_state = s->present && mcm_rf_on(s) ? CARD_IDLE : CARD_OFF;
    s->auth_sector = -1;
    s->pending_cmd = 0;
    s->value_valid = false;
}

static uint8_t *card_block(MifareMCMState *s, unsigned block)
{
    return &s->mem[block * MIFARE_BLOCK_SIZE];
}

static uint8_t card_bcc(MifareMCMState *s)
{
    return s->mem[0] ^ s->mem[1] ^ s->mem[2] ^ s->mem[3];
}

static void card_save(MifareMCMState *s)
{
    g_autoptr(GError) err = NULL;

    if (!s->writeback || !s->card_file) {
        return;
    }
    if (!g_file_set_contents(s->card_file, (const char *)s->mem,
                             sizeof(s->mem), &err)) {
        qemu_log_mask(LOG_GUEST_ERROR, "mifare-mcm: cannot save %s: %s\n",
                      s->card_file, err->message);
    }
}

/* Value block: value, ~value, value (little endian), addr, ~addr, ... */
static bool value_block_ok(const uint8_t *b)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (b[i] != b[i + 8] || (b[i] ^ b[i + 4]) != 0xff) {
            return false;
        }
    }
    return b[12] == b[14] && b[13] == b[15] && (b[12] ^ b[13]) == 0xff;
}

static void value_block_set(uint8_t *b, uint32_t v)
{
    stl_le_p(b, v);
    stl_le_p(b + 4, ~v);
    stl_le_p(b + 8, v);
}

static void mcm_answer(MifareMCMState *s, const uint8_t *data, size_t len,
                       uint8_t status)
{
    memcpy(s->rx, data, len);
    s->rx_len = len;
    s->rx_pos = 0;
    s->status = ST_DV | status;
}

static void mcm_answer_byte(MifareMCMState *s, uint8_t val, uint8_t status)
{
    mcm_answer(s, &val, 1, status);
}

static void mcm_no_answer(MifareMCMState *s)
{
    s->rx_len = s->rx_pos = 0;
    s->status = ST_DV | ST_TE;
}

static bool mcm_authorized(MifareMCMState *s, unsigned block)
{
    return block < MIFARE_1K_SIZE / MIFARE_BLOCK_SIZE &&
           s->auth_sector == (int)(block / 4);
}

/* A refused command sends the card back to idle, as on a real tag. */
static void mcm_nak(MifareMCMState *s, uint8_t status)
{
    mcm_answer_byte(s, MIFARE_NAK, status);
    s->card_state = CARD_IDLE;
    s->auth_sector = -1;
    s->pending_cmd = 0;
}

static void mcm_auth(MifareMCMState *s, uint8_t cmd, uint8_t block)
{
    unsigned sector = (block >> 2) & (MCM_SECTORS - 1);
    unsigned keyset = s->keystacon & 3;
    bool keyb = cmd & 1;
    const uint8_t *key = s->keys[keyset][keyb][sector];
    const uint8_t *trailer = card_block(s, sector * 4 + 3) + (keyb ? 10 : 0);
    g_autofree char *k = hex(key, 6);
    bool ok;

    if (s->keystacon & KEY_SERVICE) {
        ok = !s->check_keys;        /* the service key is not modelled */
    } else {
        ok = !s->check_keys || !memcmp(key, trailer, 6);
    }
    mcm_event(s, "AUTH key %c sector %u keyset %u key %s: %s",
              keyb ? 'B' : 'A', sector, keyset, k, ok ? "ok" : "FAILED");
    if (ok) {
        s->auth_sector = sector;
        s->rx_len = s->rx_pos = 0;
        s->status = ST_DV;
    } else {
        s->rx_len = s->rx_pos = 0;
        s->status = ST_DV | ST_AE;
        s->card_state = CARD_IDLE;
        s->auth_sector = -1;
    }
}

static void mcm_read_block(MifareMCMState *s, uint8_t block)
{
    uint8_t data[MIFARE_BLOCK_SIZE];
    g_autofree char *d = NULL;

    if (!mcm_authorized(s, block)) {
        mcm_event(s, "READ block %u: not authorized", block);
        mcm_nak(s, ST_BE);
        return;
    }
    memcpy(data, card_block(s, block), sizeof(data));
    if ((block & 3) == 3) {
        memset(data, 0, 6);         /* key A never reads back */
    }
    d = hex(data, sizeof(data));
    mcm_event(s, "READ block %u -> %s", block, d);
    mcm_answer(s, data, sizeof(data), 0);
}

/* First half of WRITE, DECREMENT, INCREMENT and RESTORE. */
static void mcm_two_part(MifareMCMState *s, uint8_t cmd, uint8_t block)
{
    bool value = cmd != 0xa0;

    if (!mcm_authorized(s, block) || block == 0 ||
        (value && ((block & 3) == 3 ||
                   !value_block_ok(card_block(s, block))))) {
        mcm_event(s, "%s block %u: refused",
                  value ? "VALUE" : "WRITE", block);
        mcm_nak(s, 0);
        return;
    }
    s->pending_cmd = cmd;
    s->pending_block = block;
    mcm_answer_byte(s, MIFARE_ACK, 0);
}

static void mcm_second_part(MifareMCMState *s)
{
    uint8_t cmd = s->pending_cmd, block = s->pending_block;
    g_autofree char *d = NULL;
    uint32_t v, arg;

    s->pending_cmd = 0;
    if (cmd == 0xa0) {
        if (s->tx_len < MIFARE_BLOCK_SIZE) {
            mcm_nak(s, ST_BE);
            return;
        }
        memcpy(card_block(s, block), s->tx, MIFARE_BLOCK_SIZE);
        d = hex(s->tx, MIFARE_BLOCK_SIZE);
        mcm_event(s, "WRITE block %u <- %s", block, d);
        card_save(s);
        mcm_answer_byte(s, MIFARE_ACK, 0);
        return;
    }

    memcpy(s->value_buf, card_block(s, block), MIFARE_BLOCK_SIZE);
    v = ldl_le_p(s->value_buf);
    arg = ldl_le_p(s->tx);
    switch (cmd) {
    case 0xc0:
        v -= arg;
        break;
    case 0xc1:
        v += arg;
        break;
    default:
        break;                      /* RESTORE copies the block as it is */
    }
    value_block_set(s->value_buf, v);
    s->value_valid = true;
    mcm_event(s, "%s block %u by %u -> %d (pending TRANSFER)",
              cmd == 0xc0 ? "DECREMENT" : cmd == 0xc1 ? "INCREMENT"
                                                      : "RESTORE",
              block, arg, (int32_t)v);
    mcm_no_answer(s);               /* the card does not answer on success */
}

static void mcm_transfer(MifareMCMState *s, uint8_t block)
{
    if (!mcm_authorized(s, block) || !s->value_valid || block == 0 ||
        (block & 3) == 3) {
        mcm_event(s, "TRANSFER block %u: refused", block);
        mcm_nak(s, 0);
        return;
    }
    memcpy(card_block(s, block), s->value_buf, 12);
    s->value_valid = false;
    mcm_event(s, "TRANSFER block %u <- %d", block,
              (int32_t)ldl_le_p(s->value_buf));
    card_save(s);
    mcm_answer_byte(s, MIFARE_ACK, 0);
}

/* The whole frame has been written: let the card answer it. */
static void mcm_process(MifareMCMState *s)
{
    const uint8_t *tx = s->tx;
    g_autofree char *frame = hex(s->tx, s->tx_len);

    trace_mifare_mcm_frame(frame, card_state_name[s->card_state]);

    if (s->card_state == CARD_OFF) {
        mcm_no_answer(s);
        return;
    }

    if (s->bcnts == 7) {                        /* REQA / WUPA */
        if (s->card_state == CARD_IDLE ||
            (tx[0] == 0x52 && s->card_state == CARD_HALT)) {
            static const uint8_t atqa[2] = { 0x04, 0x00 };

            s->card_state = CARD_READY;
            mcm_event(s, "%s -> ATQA 0004", tx[0] == 0x52 ? "WUPA" : "REQA");
            mcm_answer(s, atqa, sizeof(atqa), 0);
        } else {
            mcm_no_answer(s);
        }
        return;
    }

    if (tx[0] == 0x93 && s->tx_len >= 2 &&
        (s->card_state == CARD_READY || s->card_state == CARD_ACTIVE)) {
        if (tx[1] == 0x70 && s->tx_len >= 7) {  /* SELECT */
            if (memcmp(tx + 2, s->mem, 4) || tx[6] != card_bcc(s)) {
                mcm_no_answer(s);
                return;
            }
            s->card_state = CARD_ACTIVE;
            s->auth_sector = -1;
            mcm_event(s, "SELECT %02x%02x%02x%02x -> SAK 08",
                      tx[2], tx[3], tx[4], tx[5]);
            mcm_answer_byte(s, 0x08, 0);
        } else {                                /* ANTICOLLISION */
            uint8_t uid[5];

            memcpy(uid, s->mem, 4);
            uid[4] = card_bcc(s);
            mcm_event(s, "ANTICOLLISION -> UID %02x%02x%02x%02x",
                      uid[0], uid[1], uid[2], uid[3]);
            mcm_answer(s, uid, sizeof(uid), 0);
        }
        return;
    }

    if (s->card_state != CARD_ACTIVE) {
        mcm_no_answer(s);
        return;
    }

    if (s->pending_cmd) {
        mcm_second_part(s);
        return;
    }

    switch (tx[0]) {
    case 0x60 ... 0x64:
        mcm_auth(s, tx[0], tx[1]);
        break;
    case 0x30:
        mcm_read_block(s, tx[1]);
        break;
    case 0xa0:
    case 0xc0:
    case 0xc1:
    case 0xc2:
        mcm_two_part(s, tx[0], tx[1]);
        break;
    case 0xb0:
        mcm_transfer(s, tx[1]);
        break;
    case 0x50:
        mcm_event(s, "HALT");
        s->card_state = CARD_HALT;
        s->auth_sector = -1;
        mcm_no_answer(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "mifare-mcm: unknown command %s\n",
                      frame);
        mcm_no_answer(s);
        break;
    }
}

static void mcm_load_key(MifareMCMState *s)
{
    unsigned keyset = s->keystacon & 3;
    bool keyb = s->keyadr & KEY_B;
    unsigned sector = s->keyadr & (MCM_SECTORS - 1);
    g_autofree char *k = hex(s->keybuf + 6, 6);

    /* The transport key is not checked: the firmware supplies its own. */
    memcpy(s->keys[keyset][keyb][sector], s->keybuf + 6, 6);
    s->status &= ~ST_AE;
    mcm_event(s, "LOAD KEY keyset %u key %c sector %u = %s",
              keyset, keyb ? 'B' : 'A', sector, k);
}

static uint64_t mcm_read(void *opaque, hwaddr addr, unsigned size)
{
    MifareMCMState *s = opaque;
    uint8_t val;

    switch (addr) {
    case R_DATA:
        if (s->rx_pos < s->rx_len) {
            val = s->rx[s->rx_pos++];
            if (s->rx_pos == s->rx_len) {
                s->status &= ~ST_DV;
            }
        } else {
            val = 0;
        }
        break;
    case R_STACON:
        val = s->status;
        break;
    case R_ENABLE:
        val = s->enable;
        break;
    case R_BCNTS:
        val = s->bcnts;
        break;
    case R_BCNTR:
        val = s->bcntr;
        break;
    case R_BAUDRATE:
        val = s->baudrate;
        break;
    case R_TOC:
        val = s->toc;
        break;
    case R_MODE:
        val = s->mode;
        break;
    case R_CRCDATA:
        val = s->crcdata;
        break;
    case R_CRCSTACON:
        val = 0x80;                 /* ready, so polling loops end */
        break;
    case R_KEYSTACON:
        val = s->keystacon;
        break;
    case R_KEYADR:
        val = s->keyadr;
        break;
    case R_RCODE:
        val = s->rcode;
        break;
    default:
        val = 0;
        break;
    }
    trace_mifare_mcm_read(addr, val);
    return val;
}

static void mcm_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MifareMCMState *s = opaque;
    bool rf;

    trace_mifare_mcm_write(addr, val);
    switch (addr) {
    case R_DATA:
        if (s->tx_done) {
            s->tx_len = 0;
            s->tx_done = false;
            s->rx_len = s->rx_pos = 0;
            s->status = 0;
        }
        if (s->tx_len < MCM_FIFO_SIZE) {
            s->tx[s->tx_len++] = val;
        }
        if (s->tx_len * 8 >= MAX(s->bcnts, 1)) {
            s->tx_done = true;
            mcm_process(s);
        }
        break;
    case R_STACON:
        rf = mcm_rf_on(s);
        s->stacon = val;
        if (val & CTL_SOR) {
            s->status = 0;
            s->rx_len = s->rx_pos = 0;
            s->tx_done = true;
        }
        if (rf != mcm_rf_on(s)) {
            mcm_event(s, "RF field %s", mcm_rf_on(s) ? "on" : "off");
            mcm_card_power(s);
        }
        break;
    case R_ENABLE:
        s->enable = val;
        break;
    case R_BCNTS:
        s->bcnts = val;
        break;
    case R_BCNTR:
        s->bcntr = val;
        break;
    case R_BAUDRATE:
        s->baudrate = val;
        break;
    case R_TOC:
        s->toc = val;               /* answers are immediate */
        break;
    case R_MODE:
        s->mode = val;
        break;
    case R_CRCDATA:
    case R_CRCSTACON:
        qemu_log_mask(LOG_UNIMP, "mifare-mcm: CRC coprocessor not modelled\n");
        if (addr == R_CRCDATA) {
            s->crcdata = val;
        } else {
            s->crcstacon = val;
        }
        break;
    case R_KEYDATA:
        if (s->keybuf_len < sizeof(s->keybuf)) {
            s->keybuf[s->keybuf_len++] = val;
            if (s->keybuf_len == sizeof(s->keybuf) &&
                !(s->keyadr & KEY_AUTH)) {
                mcm_load_key(s);
            }
        }
        break;
    case R_KEYSTACON:
        s->keystacon = val;
        s->keybuf_len = 0;
        break;
    case R_KEYADR:
        s->keyadr = val;
        s->keybuf_len = 0;
        break;
    case R_RCODE:
        s->rcode = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mifare-mcm: write 0x%02" PRIx64 " to register 0x%"
                      HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps mcm_ops = {
    .read = mcm_read,
    .write = mcm_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static bool mcm_get_present(Object *obj, Error **errp)
{
    return MIFARE_MCM(obj)->present;
}

static void mcm_set_present(Object *obj, bool value, Error **errp)
{
    MifareMCMState *s = MIFARE_MCM(obj);

    if (s->present != value) {
        s->present = value;
        mcm_event(s, "card %s", value ? "placed on the reader" : "removed");
        mcm_card_power(s);
    }
}

/* Blank MIFARE Classic 1K: manufacturer block and transport trailers. */
static void mcm_blank_card(MifareMCMState *s)
{
    static const uint8_t trailer[MIFARE_BLOCK_SIZE] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x07,
        0x80, 0x69, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    unsigned i;

    memset(s->mem, 0, sizeof(s->mem));
    stl_be_p(s->mem, s->uid);
    s->mem[4] = card_bcc(s);
    s->mem[5] = 0x08;               /* SAK */
    s->mem[6] = 0x04;               /* ATQA */
    s->mem[7] = 0x00;
    for (i = 0; i < MCM_SECTORS; i++) {
        memcpy(card_block(s, i * 4 + 3), trailer, sizeof(trailer));
    }
}

static void mcm_realize(DeviceState *dev, Error **errp)
{
    MifareMCMState *s = MIFARE_MCM(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &mcm_ops, s, "mifare-mcm",
                          MCM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    if (s->card_file) {
        g_autofree char *data = NULL;
        g_autoptr(GError) err = NULL;
        gsize len;

        if (!g_file_get_contents(s->card_file, &data, &len, &err)) {
            error_setg(errp, "mifare-mcm: cannot read %s: %s",
                       s->card_file, err->message);
            return;
        }
        if (len != MIFARE_1K_SIZE) {
            error_setg(errp, "mifare-mcm: %s has %zu bytes, a MIFARE "
                       "Classic 1K dump has %d", s->card_file, len,
                       MIFARE_1K_SIZE);
            return;
        }
        memcpy(s->mem, data, MIFARE_1K_SIZE);
        if (s->mem[4] != card_bcc(s)) {
            warn_report("mifare-mcm: %s: UID check byte is wrong",
                        s->card_file);
        }
        s->present = true;
    } else {
        mcm_blank_card(s);
    }
}

static void mcm_reset(DeviceState *dev)
{
    MifareMCMState *s = MIFARE_MCM(dev);

    s->stacon = 0;
    s->status = 0;
    s->enable = s->bcnts = s->bcntr = s->baudrate = s->toc = s->mode = 0;
    s->crcdata = s->crcstacon = s->keystacon = s->keyadr = s->rcode = 0;
    s->tx_len = 0;
    s->tx_done = true;
    s->rx_len = s->rx_pos = 0;
    s->keybuf_len = 0;
    memset(s->keys, 0, sizeof(s->keys));
    mcm_card_power(s);
}

static void mcm_init(Object *obj)
{
    object_property_add_bool(obj, "present", mcm_get_present,
                             mcm_set_present);
    object_property_set_description(obj, "present",
                                    "Card placed on the reader");
}

static const VMStateDescription vmstate_mcm = {
    .name = "mifare-mcm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(stacon, MifareMCMState),
        VMSTATE_UINT8(status, MifareMCMState),
        VMSTATE_UINT8(enable, MifareMCMState),
        VMSTATE_UINT8(bcnts, MifareMCMState),
        VMSTATE_UINT8(bcntr, MifareMCMState),
        VMSTATE_UINT8(baudrate, MifareMCMState),
        VMSTATE_UINT8(toc, MifareMCMState),
        VMSTATE_UINT8(mode, MifareMCMState),
        VMSTATE_UINT8(crcdata, MifareMCMState),
        VMSTATE_UINT8(crcstacon, MifareMCMState),
        VMSTATE_UINT8(keystacon, MifareMCMState),
        VMSTATE_UINT8(keyadr, MifareMCMState),
        VMSTATE_UINT8(rcode, MifareMCMState),
        VMSTATE_UINT8_ARRAY(tx, MifareMCMState, MCM_FIFO_SIZE),
        VMSTATE_UINT32(tx_len, MifareMCMState),
        VMSTATE_BOOL(tx_done, MifareMCMState),
        VMSTATE_UINT8_ARRAY(rx, MifareMCMState, MCM_FIFO_SIZE),
        VMSTATE_UINT32(rx_len, MifareMCMState),
        VMSTATE_UINT32(rx_pos, MifareMCMState),
        VMSTATE_BUFFER_UNSAFE(keys, MifareMCMState, 0,
                              sizeof(((MifareMCMState *)0)->keys)),
        VMSTATE_UINT8_ARRAY(keybuf, MifareMCMState, 12),
        VMSTATE_UINT32(keybuf_len, MifareMCMState),
        VMSTATE_BOOL(present, MifareMCMState),
        VMSTATE_UINT8(card_state, MifareMCMState),
        VMSTATE_INT32(auth_sector, MifareMCMState),
        VMSTATE_UINT8(pending_cmd, MifareMCMState),
        VMSTATE_UINT8(pending_block, MifareMCMState),
        VMSTATE_UINT8_ARRAY(value_buf, MifareMCMState, MIFARE_BLOCK_SIZE),
        VMSTATE_BOOL(value_valid, MifareMCMState),
        VMSTATE_UINT8_ARRAY(mem, MifareMCMState, MIFARE_1K_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static Property mcm_properties[] = {
    DEFINE_PROP_STRING("card", MifareMCMState, card_file),
    DEFINE_PROP_BOOL("writeback", MifareMCMState, writeback, false),
    DEFINE_PROP_BOOL("check-keys", MifareMCMState, check_keys, true),
    DEFINE_PROP_UINT32("uid", MifareMCMState, uid, 0x12345678),
    DEFINE_PROP_END_OF_LIST(),
};

static void mcm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcm_realize;
    dc->reset = mcm_reset;
    dc->vmsd = &vmstate_mcm;
    dc->desc = "Mikron MIFARE Core Module with a MIFARE Classic 1K card";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, mcm_properties);
}

static const TypeInfo mcm_info = {
    .name = TYPE_MIFARE_MCM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MifareMCMState),
    .instance_init = mcm_init,
    .class_init = mcm_class_init,
};

static void mcm_register_types(void)
{
    type_register_static(&mcm_info);
}

type_init(mcm_register_types)
