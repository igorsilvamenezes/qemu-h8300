/*
 * Stamptronic RFM433-LC / LC+ radio modem (as used in the RF2000 radio box)
 *
 * Model of the modem's serial side, following "Radio Modem RFM433-LC,
 * Interface & Commands", version uP1.5, and the RF2000 repair manual:
 *
 * - After power on the modem is in transparent mode, channel 0, 9600 8N1.
 *   Characters from the host are sent over the air and characters received
 *   over the air go to the host.
 * - "+++" sent after a pause switches to AT command mode; the modem answers
 *   with the single character '0'.  It is recognised in either mode.
 * - In AT mode every line ends with CR and starts with "AT".  Commands are
 *   answered with one result character, '0' (OK) or '1' (error), without
 *   echo or line end:
 *     ATO          back to transparent mode
 *     ATP0, ATP1   enter / leave low power mode
 *     ATSnnn=m     set register nnn (200, 210-214, 216, 20A, 20B)
 *     ATSnnn?      read register nnn, answered as "=m" CR
 *     ATS23x=m     busy time of channel x over m * 10 ms, "=  0" CR
 *     ATS240?      busy time of every channel, "000;...;000" CR
 *     ATS241?      the same, repeated until the host sends a character
 *     AT/S         configuration summary, AT/V software version
 *     ATT0-ATT3    transmitter test, stopped by any character
 *   Lines that do not start with "AT" are ignored.
 *
 * The radio itself is reduced to a character backend ("air"): there is no
 * channel separation, carrier, RF delay or 40 byte buffer limit, and the
 * serial format registers do not change how the host UART is driven.  Air
 * traffic is dropped while the modem is in AT mode or asleep.  Answers are
 * delayed a little (50 ms for "+++", 10 ms for commands) and every character
 * to the host is paced at the configured baud rate.
 *
 * Modem activity is reported through the rfm433_* trace events.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "sysemu/reset.h"
#include "hw/char/rfm433.h"
#include "trace.h"

#define RFM433_OUT_SIZE     512
#define RFM433_LINE_SIZE    64
#define RFM433_CHANNELS     10
#define RFM433_GUARD_NS     (20 * SCALE_MS)   /* silence before "+++" */
#define RFM433_PLUS_NS      (50 * SCALE_MS)   /* "+++" answer delay */
#define RFM433_CMD_NS       (10 * SCALE_MS)   /* command answer delay */
#define RFM433_VERSION      "RFM433-LC V1.51"

enum { MODE_DATA, MODE_AT, MODE_TEST, MODE_SCAN };

typedef struct {
    Chardev parent;

    CharBackend air;
    QEMUTimer *out_timer;       /* paces characters to the host */
    QEMUTimer *reply_timer;     /* releases a delayed answer */
    QEMUTimer *scan_timer;      /* repeats the S241 scan */

    Fifo8 out;                  /* characters waiting for the host */
    char reply[RFM433_OUT_SIZE];
    size_t reply_len;

    int mode;
    bool asleep;
    int64_t last_rx_ns;         /* last character from the host */
    int plus;                   /* '+' characters held back in data mode */
    char line[RFM433_LINE_SIZE];
    size_t line_len;

    /* S registers */
    uint8_t channel;            /* S200 */
    uint8_t baud;               /* S210 */
    uint8_t bits;               /* S211 */
    uint8_t parity;             /* S212 */
    uint8_t stop;               /* S213 */
    uint8_t char_timeout;       /* S214 */
    uint8_t sync_tx;            /* S20A */
    uint8_t sync_rx;            /* S20B */
    uint8_t scan_ascii;         /* S216 */
} RFM433State;

DECLARE_INSTANCE_CHECKER(RFM433State, RFM433, TYPE_CHARDEV_RFM433)

static const unsigned baud_rate[] = { 0, 1200, 2400, 4800, 9600, 19200, 38400 };

static const char *mode_name(int mode)
{
    static const char *const names[] = { "transparent", "AT", "test", "scan" };
    return names[mode];
}

static void rfm433_set_mode(RFM433State *s, int mode)
{
    if (s->mode != mode) {
        s->mode = mode;
        trace_rfm433_mode(mode_name(mode));
    }
}

static int64_t byte_ns(RFM433State *s)
{
    /* start bit, data bits, parity, stop bits */
    int bits = 1 + s->bits + (s->parity > 1) + s->stop;

    return NANOSECONDS_PER_SECOND * bits / baud_rate[s->baud];
}

/* Send queued characters to the host, one per character time. */
static void rfm433_out_kick(void *opaque)
{
    RFM433State *s = opaque;
    Chardev *chr = CHARDEV(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (fifo8_is_empty(&s->out)) {
        return;
    }
    if (qemu_chr_be_can_write(chr) > 0) {
        uint8_t c = fifo8_pop(&s->out);

        qemu_chr_be_write(chr, &c, 1);
    }
    if (!fifo8_is_empty(&s->out)) {
        timer_mod(s->out_timer, now + byte_ns(s));
    }
}

static void rfm433_queue(RFM433State *s, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len-- && !fifo8_is_full(&s->out)) {
        fifo8_push(&s->out, *p++);
    }
    if (!timer_pending(s->out_timer)) {
        rfm433_out_kick(s);
    }
}

static void rfm433_reply_fire(void *opaque)
{
    RFM433State *s = opaque;

    rfm433_queue(s, s->reply, s->reply_len);
    s->reply_len = 0;
}

/* Answer the host after @delay ns; answers given meanwhile are appended. */
static void G_GNUC_PRINTF(3, 4)
rfm433_reply(RFM433State *s, int64_t delay, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(s->reply + s->reply_len, sizeof(s->reply) - s->reply_len,
                  fmt, ap);
    va_end(ap);
    s->reply_len = MIN(s->reply_len + MAX(n, 0), sizeof(s->reply) - 1);
    if (!timer_pending(s->reply_timer)) {
        timer_mod(s->reply_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    }
}

static void rfm433_scan_all(RFM433State *s)
{
    char buf[RFM433_CHANNELS * 4];
    int i;

    for (i = 0; i < RFM433_CHANNELS; i++) {
        /* The air is never busy: every channel reports 0 % occupation. */
        pstrcpy(buf + 4 * i, sizeof(buf) - 4 * i, i ? ";000" : "000");
    }
    rfm433_reply(s, RFM433_CHANNELS * 10 * SCALE_MS, "%s\r", buf);
}

static void rfm433_scan_repeat(void *opaque)
{
    RFM433State *s = opaque;

    if (s->mode == MODE_SCAN) {
        rfm433_scan_all(s);
        timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  RFM433_CHANNELS * 10 * SCALE_MS);
    }
}

/* Register access; returns false for an unknown register or bad value. */
static bool rfm433_reg(RFM433State *s, const char *reg, bool write,
                       unsigned val, unsigned *out)
{
    static const struct {
        const char *name;
        size_t off;
        unsigned min, max;
    } regs[] = {
        { "200", offsetof(RFM433State, channel), 0, 9 },
        { "210", offsetof(RFM433State, baud), 1, 6 },
        { "211", offsetof(RFM433State, bits), 7, 8 },
        { "212", offsetof(RFM433State, parity), 1, 3 },
        { "213", offsetof(RFM433State, stop), 1, 2 },
        { "214", offsetof(RFM433State, char_timeout), 1, 255 },
        { "20A", offsetof(RFM433State, sync_tx), 1, 255 },
        { "20B", offsetof(RFM433State, sync_rx), 1, 255 },
        { "216", offsetof(RFM433State, scan_ascii), 0, 1 },
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        uint8_t *p = (uint8_t *)s + regs[i].off;

        if (strcmp(reg, regs[i].name)) {
            continue;
        }
        if (!write) {
            *out = *p;
            return true;
        }
        if (val < regs[i].min || val > regs[i].max) {
            return false;
        }
        *p = val;
        if (p == &s->channel) {
            trace_rfm433_channel(val);
        }
        return true;
    }
    return false;
}

/* Execute an AT command line (without "AT" and CR); returns the result. */
static bool rfm433_command(RFM433State *s, const char *cmd)
{
    char reg[4];
    unsigned val;
    const char *p;

    if (!*cmd) {
        return true;
    }
    if (!strcmp(cmd, "O")) {
        rfm433_set_mode(s, MODE_DATA);
        return true;
    }
    if (!strcmp(cmd, "P0") || !strcmp(cmd, "P1")) {
        s->asleep = cmd[1] == '0';
        trace_rfm433_mode(s->asleep ? "standby" : "awake");
        return true;
    }
    if (!strcmp(cmd, "/V")) {
        rfm433_reply(s, RFM433_CMD_NS, RFM433_VERSION "\r\n");
        return true;
    }
    if (!strcmp(cmd, "/S")) {
        rfm433_reply(s, RFM433_CMD_NS,
                     "Canal : %u\r\nDebit : %u\r\nLong : %u\r\n"
                     "Parit : %c\r\nStop : %u\r\nTime : %03u\r\n",
                     s->channel, baud_rate[s->baud], s->bits,
                     "NOE"[s->parity - 1], s->stop, s->char_timeout);
        return true;
    }
    if (cmd[0] == 'T' && cmd[1] >= '0' && cmd[1] <= '3' && !cmd[2]) {
        rfm433_set_mode(s, MODE_TEST);
        return true;
    }
    if (cmd[0] != 'S' || strlen(cmd) < 5) {
        return false;
    }

    pstrcpy(reg, sizeof(reg), cmd + 1);
    p = cmd + 4;
    if (!strcmp(reg, "240") || !strcmp(reg, "241")) {
        rfm433_scan_all(s);
        if (reg[2] == '1') {
            rfm433_set_mode(s, MODE_SCAN);
            timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      RFM433_CHANNELS * 10 * SCALE_MS);
        }
        return true;
    }
    if (*p == '?' && !p[1]) {
        if (!rfm433_reg(s, reg, false, 0, &val)) {
            return false;
        }
        rfm433_reply(s, RFM433_CMD_NS, "=%u\r", val);
        return true;
    }
    if (*p != '=' || qemu_strtoui(p + 1, NULL, 10, &val) < 0) {
        return false;
    }
    if (reg[0] == '2' && reg[1] == '3' && g_ascii_isdigit(reg[2])) {
        /* Busy time of one channel, measured over val * 10 ms. */
        if (val < 1 || val > 255) {
            return false;
        }
        rfm433_reply(s, val * 10 * SCALE_MS, "=  0\r");
        return true;
    }
    return rfm433_reg(s, reg, true, val, &val);
}

static void rfm433_line(RFM433State *s)
{
    char *line = s->line;
    bool ok;

    s->line[s->line_len] = '\0';
    s->line_len = 0;
    if (!*line) {
        return;
    }
    if (g_ascii_strncasecmp(line, "AT", 2)) {
        trace_rfm433_command(line, "ignored");
        return;
    }
    line = g_ascii_strup(line + 2, -1);
    ok = rfm433_command(s, line);
    trace_rfm433_command(line, ok ? "OK" : "ERROR");
    g_free(line);
    /* The result follows any text answer of the command. */
    rfm433_reply(s, RFM433_CMD_NS, "%c", ok ? '0' : '1');
}

static void rfm433_enter_at(RFM433State *s)
{
    rfm433_set_mode(s, MODE_AT);
    s->line_len = 0;
    rfm433_reply(s, RFM433_PLUS_NS, "0");
}

static void rfm433_to_air(RFM433State *s, const uint8_t *buf, int len)
{
    if (!s->asleep && qemu_chr_fe_backend_connected(&s->air)) {
        qemu_chr_fe_write_all(&s->air, buf, len);
    }
}

/* A character from the host. */
static void rfm433_rx(RFM433State *s, uint8_t c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool pause = now - s->last_rx_ns >= RFM433_GUARD_NS;

    s->last_rx_ns = now;

    switch (s->mode) {
    case MODE_TEST:
    case MODE_SCAN:
        /* Any character stops the test or the permanent scan. */
        timer_del(s->scan_timer);
        rfm433_set_mode(s, MODE_AT);
        return;

    case MODE_DATA:
        /* Hold back '+' after a pause until it is clear it is not "+++". */
        if (c == '+' && (s->plus || pause)) {
            if (++s->plus == 3) {
                s->plus = 0;
                rfm433_enter_at(s);
            }
            return;
        }
        if (s->plus) {
            static const uint8_t plusses[] = "++";

            rfm433_to_air(s, plusses, s->plus);
            s->plus = 0;
        }
        rfm433_to_air(s, &c, 1);
        return;

    case MODE_AT:
        if (c == '\r') {
            rfm433_line(s);
        } else if (c != '\n' && s->line_len < sizeof(s->line) - 1) {
            s->line[s->line_len++] = c;
            if (s->line_len == 3 && !memcmp(s->line, "+++", 3)) {
                rfm433_enter_at(s);
            }
        }
        return;
    }
}

static int rfm433_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    RFM433State *s = RFM433(chr);
    int i;

    for (i = 0; i < len; i++) {
        rfm433_rx(s, buf[i]);
    }
    return len;
}

static void rfm433_chr_accept_input(Chardev *chr)
{
    RFM433State *s = RFM433(chr);

    if (!timer_pending(s->out_timer)) {
        rfm433_out_kick(s);
    }
}

static int rfm433_air_can_read(void *opaque)
{
    RFM433State *s = opaque;

    return fifo8_num_free(&s->out);
}

static void rfm433_air_read(void *opaque, const uint8_t *buf, int len)
{
    RFM433State *s = opaque;

    /* Only a modem in transparent mode passes received data to the host. */
    if (s->mode == MODE_DATA && !s->asleep) {
        rfm433_queue(s, buf, len);
    }
}

static void rfm433_reset(void *opaque)
{
    RFM433State *s = opaque;

    timer_del(s->out_timer);
    timer_del(s->reply_timer);
    timer_del(s->scan_timer);
    fifo8_reset(&s->out);
    s->reply_len = 0;
    s->mode = MODE_DATA;
    s->asleep = false;
    s->last_rx_ns = INT64_MIN / 2;
    s->plus = 0;
    s->line_len = 0;
    s->channel = 0;
    s->baud = 4;
    s->bits = 8;
    s->parity = 1;
    s->stop = 1;
    s->char_timeout = 5;
    s->sync_tx = 15;
    s->sync_rx = 8;
    s->scan_ascii = 0;
}

static void rfm433_chr_open(Chardev *chr, ChardevBackend *backend,
                            bool *be_opened, Error **errp)
{
    RFM433State *s = RFM433(chr);

    fifo8_create(&s->out, RFM433_OUT_SIZE);
    s->out_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rfm433_out_kick, s);
    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rfm433_reply_fire, s);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rfm433_scan_repeat, s);
    rfm433_reset(s);
    qemu_register_reset(rfm433_reset, s);
}

static void rfm433_finalize(Object *obj)
{
    RFM433State *s = RFM433(obj);

    qemu_unregister_reset(rfm433_reset, s);
    qemu_chr_fe_deinit(&s->air, false);
    timer_free(s->out_timer);
    timer_free(s->reply_timer);
    timer_free(s->scan_timer);
    fifo8_destroy(&s->out);
}

Chardev *rfm433_new(const char *id, Chardev *air, Error **errp)
{
    Chardev *chr = qemu_chardev_new(id, TYPE_CHARDEV_RFM433, NULL, NULL,
                                    errp);
    RFM433State *s;

    if (!chr) {
        return NULL;
    }
    s = RFM433(chr);
    if (air) {
        if (!qemu_chr_fe_init(&s->air, air, errp)) {
            object_unparent(OBJECT(chr));
            return NULL;
        }
        qemu_chr_fe_set_handlers(&s->air, rfm433_air_can_read,
                                 rfm433_air_read, NULL, NULL, s, NULL, true);
    }
    return chr;
}

static void rfm433_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->internal = true;
    cc->open = rfm433_chr_open;
    cc->chr_write = rfm433_chr_write;
    cc->chr_accept_input = rfm433_chr_accept_input;
}

static const TypeInfo rfm433_type_info = {
    .name = TYPE_CHARDEV_RFM433,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(RFM433State),
    .instance_finalize = rfm433_finalize,
    .class_init = rfm433_class_init,
};

static void rfm433_register_types(void)
{
    type_register_static(&rfm433_type_info);
}

type_init(rfm433_register_types)
