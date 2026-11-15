/*
 * E2300 board with an H8/3003 in advanced mode 3.
 *
 * Models the external memory, I/O ports, watchdog and both SCIs, and
 * instantiates the H8/3003 on-chip modules (interrupt controller, ITU,
 * DMAC, TPC, A/D) used by the E2300 boot monitor and operating system.
 * Register definitions follow the H8/3003 Hardware Manual, sections 3,
 * 9, 12 and 13.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/char/renesas_sci.h"
#include "hw/char/rfm433.h"
#include "hw/display/hdsp211x.h"
#include "hw/misc/battery_sensor.h"
#include "hw/misc/dip_switch.h"
#include "hw/misc/gpio_beeper.h"
#include "hw/misc/led.h"
#include "hw/misc/mifare_mcm.h"
#include "hw/misc/push_button.h"
#include "hw/misc/ntc_thermistor.h"
#include "hw/misc/turnstile.h"
#include "hw/char/sc28c94.h"
#include "hw/h8300/h83003_onchip.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "trace/trace-system.h"
#include "trace.h"

#define TYPE_E2300_MACHINE MACHINE_TYPE_NAME("e2300")
OBJECT_DECLARE_SIMPLE_TYPE(E2300MachineState, E2300_MACHINE)

#define E2300_CLOCK_HZ 16000000
#define E2300_FLASH_SIZE (256 * KiB)
#define E2300_RAM_BASE 0x100000
#define H83003_IRAM_BASE 0xfffd10
#define H83003_IRAM_SIZE 0x200
#define H83003_WDT_BASE 0xffffa8
#define H83003_SCI0_BASE 0xffffb0
#define H83003_SCI1_BASE 0xffffb8
#define H83003_GPIO_BASE 0xffffc5
#define H83003_GPIO_SIZE 0x17   /* P4DDR (0xFFFFC5) to P5PCR (0xFFFFDB) */
#define PORT_REG(a) ((a) - H83003_GPIO_BASE)
#define H83003_MODE_BASE 0xfffff1
#define E2300_DISPLAY_BASE 0x800000  /* CS4: HDSP-211x, FL on A5 */
#define E2300_MIFARE_BASE 0x810010   /* CS4: MIFARE Core Module */
#define E2300_QUART_BASE 0x810080    /* CS4: SC28C94 quad UART on IRQ0 */
#define E2300_I2C_SCL 0x80           /* PB7, open drain through PBDDR */
#define E2300_I2C_SDA 0x04           /* PB2, open drain through PBDDR */
#define E2300_RTC_ADDR 0x68          /* DS1307-compatible clock, 0xD0/0xD1 */

/*
 * Front panel outputs on port 4, set by the OS lamps() call.  The yellow
 * and green LEDs are not in lamps() bit order: RED 0x10 drives P4.0,
 * GREEN 0x40 P4.1, YELLOW 0x20 P4.2 and BEEP 0x80 P4.3, all active high.
 */
enum {
    E2300_PANEL_RED,
    E2300_PANEL_GREEN,
    E2300_PANEL_YELLOW,
    E2300_PANEL_BEEP,
    E2300_PANEL_LINES
};

/*
 * IC2, four DIP switches read by the boot monitor (ini_hw) and by the OS
 * readpa() ("read polling address").  A switch that is ON pulls its pin
 * low; readpa() returns the inverted pins, so switch n adds 1 << (n - 1):
 *
 *   switch 1: PCDR bit 4    readpa  1  application debug output (printd)
 *   switch 2: P8DR bit 1            2
 *   switch 3: P8DR bit 2            4
 *   switch 4: P8DR bit 3            8  application test menu on SCI1
 *
 * All ON (15) keeps the boot monitor at its '>' prompt for SENDBIN; any
 * other value lets it start the OS.  The numbering of the switches on the
 * part is inferred from readpa(); the board schematic is not available.
 * (Older boards, OS hardware variant < 4, read the same switches on
 * P5DR bits 4-7; the OS identifies this board as variant 4.)
 */
#define E2300_IC2_SWITCHES 4
#define E2300_IC2_ALL_ON 0xf

/*
 * Turnstile, driven by the OS gate() and checkgate() calls.  gate() sets
 * P4.7 to energize the release solenoid (the application passes
 * turnstile_open_status, 1 by default, so high releases the arm).
 * checkgate() returns the rotation switches as read, 1 = closed: on this
 * board (OS hardware variant 4) switch 1 is P6.2 and switch 2 is P6.1;
 * variant 2 boards read switch 1 on P6.0, so it is mirrored there.  The
 * application waits for switch 1, drops the solenoid and waits for switch
 * 2 to end the passage.  Both switches are open with the arm at rest.
 */
#define E2300_GATE_SOLENOID 0x80     /* P4.7 */
#define E2300_GATE_SW1 0x05          /* P6.2, and P6.0 for variant 2 */
#define E2300_GATE_SW2 0x02          /* P6.1 */

/*
 * Backup battery detector on PC6, high when the lithium cell is low.  The
 * OS samples it once at startup and sets bit 0x80 ("battery ok") of the
 * status word at 0x100000 when the pin is low; the boot clears that word
 * at every power-up.  The application shows "BAT ERR" when the bit is
 * clear (MBODY.C print_software_info).  So a change of the battery shows
 * after the next reset, as the board's production test describes it ("as
 * recorded at the last power-up").  The threshold of the real detector is
 * not documented; the device defaults to 3 V and 2.5 V.
 */
#define E2300_BATTERY_LOW 0x40       /* PC6 */
#define E2300_BATTERY_OK_MV 3000
#define E2300_BATTERY_LOW_MV 2000

/*
 * Driver button, read by the V02.13/6 application (drv_bt_detect()): when
 * readpa() returns 2 for at least one second, and then the button is let
 * go, the application releases the turnstile (drv_button()).  readpa()
 * reads the IC2 pins, so the button must pull the IC2 switch 2 line, P8.1,
 * low, in parallel with the switch [inferred from readpa(); the board
 * schematic is not available].  It is only seen as 2 with the other
 * switches OFF, the normal setting.
 */
#define E2300_DRIVER_BUTTON 0x02     /* P8.1, low while pressed */

/*
 * On-board temperature sensor, read by the OS gettemp() (0xB9EA) for the
 * application log.  It converts AN5 once and turns the result into a
 * resistance as if the input were a divider with 100 kohm to AVref,
 * R = 100000 * ADDR / (65536 - ADDR), scales it by the calibration word
 * at 0x6016 (written by cal_therm(), 0x2000 = 1.0) and looks it up in a
 * 125-entry table at 0x19B9A, -40 to 84 degrees Celsius.  That table
 * follows the beta model of an NTC to within 0.15%: R25 = 20.4 kohm,
 * B = 3516 K.  gettemp() reports the first table entry at or below the
 * reading, i.e. the temperature rounded up to a whole degree.  The board
 * values of the divider are not known; these reproduce the OS readings.
 */
#define E2300_THERM_AN 5
#define E2300_THERM_R25 20400       /* ohms at 25 degrees Celsius */
#define E2300_THERM_BETA 3516       /* kelvin */
#define E2300_THERM_RREF 100000     /* ohms, AN5 to AVref */

/* Byte-wide Intel 28F002 bottom-boot flash. */
enum E2300FlashMode {
    FLASH_ARRAY, FLASH_ID, FLASH_STATUS, FLASH_PROGRAM, FLASH_ERASE,
};

struct E2300MachineState {
    MachineState parent_obj;
    H8300CPU cpu;
    H83003OnChipState onchip;
    RSCIState sci0;
    HDSP211XState display;
    MifareMCMState mifare;
    SC28C94State quart;
    RSCIState sci1;
    qemu_irq wovi;
    qemu_irq panel[E2300_PANEL_LINES];  /* P4.0-P4.3 */
    uint8_t panel_level;
    TurnstileState *turnstile;
    qemu_irq gate;                      /* P4.7, release solenoid */
    uint8_t gate_switches;              /* TURNSTILE_SW1/SW2 bits, 1 = closed */
    PushButtonState *driver_button;
    bool driver_button_pressed;
    MemoryRegion flash;
    MemoryRegion absent;
    MemoryRegion iram;
    MemoryRegion gpio;
    MemoryRegion mode;
    MemoryRegion wdt;
    enum E2300FlashMode flash_mode;
    uint8_t flash_status;
    hwaddr flash_dirty_start, flash_dirty_end;
    uint8_t port[H83003_GPIO_SIZE];
    I2CBus *i2c_bus;
    bitbang_i2c_interface i2c;
    bool i2c_scl;           /* levels driven by the CPU */
    bool i2c_sda;
    bool i2c_sda_line;      /* SDA as seen on the wire, with the slave */
    uint8_t syscr;
    bool autoboot;
    bool radio;
    bool has_turnstile;
    BatterySensorState *battery;
    bool battery_low;       /* PC6 level from the battery detector */
    char *battery_mode;     /* "battery" machine property */
    char *switches;         /* "switches" machine property */
    DIPSwitchState *ic2;
    int ic2_traced;         /* positions last reported, -1 for none */
    QEMUTimer *wdt_timer;
    int64_t wdt_start;
    uint8_t tcsr, tcnt, rstcsr;
    bool wdt_reset_pending;
};

static void flash_set_mode(E2300MachineState *s, enum E2300FlashMode mode)
{
    s->flash_mode = mode;
    memory_region_rom_device_set_romd(&s->flash, mode == FLASH_ARRAY);
    if (mode == FLASH_ARRAY && s->flash_dirty_end) {
        hwaddr size = s->flash_dirty_end - s->flash_dirty_start;

        /* Invalidate cached code when the modified array becomes visible. */
        memory_region_flush_rom_device(&s->flash, s->flash_dirty_start, size);
        s->flash_dirty_end = 0;
    }
}

static void flash_mark_dirty(E2300MachineState *s, hwaddr addr, hwaddr size)
{
    s->flash_dirty_start = s->flash_dirty_end ? MIN(s->flash_dirty_start, addr)
                                            : addr;
    s->flash_dirty_end = MAX(s->flash_dirty_end, addr + size);
}

static uint64_t flash_read(void *opaque, hwaddr addr, unsigned size)
{
    E2300MachineState *s = opaque;

    if (s->flash_mode == FLASH_ID) {
        return addr & 1 ? 0x7d : 0x89;
    }
    return s->flash_status;
}

static void flash_erase_block(E2300MachineState *s, hwaddr addr)
{
    static const hwaddr boundaries[] = {
        0, 0x4000, 0x6000, 0x8000, 0x20000, E2300_FLASH_SIZE,
    };
    uint8_t *data = memory_region_get_ram_ptr(&s->flash);
    size_t i;

    for (i = 1; i < ARRAY_SIZE(boundaries); i++) {
        if (addr < boundaries[i]) {
            hwaddr start = boundaries[i - 1];
            hwaddr size = boundaries[i] - start;

            memset(data + start, 0xff, size);
            flash_mark_dirty(s, start, size);
            return;
        }
    }
}

static void flash_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    E2300MachineState *s = opaque;
    uint8_t *data = memory_region_get_ram_ptr(&s->flash);
    bool vpp = s->port[PORT_REG(0xffffc5)] & s->port[PORT_REG(0xffffc7)] &
               0x40;

    /* Complete program and erase synchronously, as observed through SR.7. */
    if (s->flash_mode == FLASH_PROGRAM) {
        if (!vpp) {
            s->flash_status |= 0x18;
        } else {
            if ((data[addr] & val) != val) {
                s->flash_status |= 0x10;
            }
            data[addr] &= val;
            flash_mark_dirty(s, addr, 1);
        }
        flash_set_mode(s, FLASH_STATUS);
        return;
    }
    if (s->flash_mode == FLASH_ERASE) {
        if (val != 0xd0) {
            s->flash_status |= 0x30;
        } else if (!vpp) {
            s->flash_status |= 0x28;
        } else {
            flash_erase_block(s, addr);
        }
        flash_set_mode(s, FLASH_STATUS);
        return;
    }

    switch (val) {
    case 0xff:
        flash_set_mode(s, FLASH_ARRAY);
        break;
    case 0x90:
        flash_set_mode(s, FLASH_ID);
        break;
    case 0x70:
        flash_set_mode(s, FLASH_STATUS);
        break;
    case 0x50:
        s->flash_status = 0x80;
        break;
    case 0x10:
    case 0x40:
        flash_set_mode(s, FLASH_PROGRAM);
        break;
    case 0x20:
        flash_set_mode(s, FLASH_ERASE);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "e2300: unknown flash command 0x%02" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.max_access_size = 1,
};

/* Empty expansion space reads as an undriven, pulled-up data bus. */
static uint64_t absent_read(void *opaque, hwaddr addr, unsigned size)
{
    return UINT64_MAX;
}

static void absent_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps absent_ops = {
    .read = absent_read,
    .write = absent_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* Data register -> data direction register, or -1 for other registers. */
static int port_ddr_of(hwaddr reg)
{
    switch (reg) {
    case 0xffffc7: return 0xffffc5;     /* P4 */
    case 0xffffca: return 0xffffc8;     /* P5 */
    case 0xffffcb: return 0xffffc9;     /* P6 */
    case 0xffffcf: return 0xffffcd;     /* P8 */
    case 0xffffd2: return 0xffffd0;     /* P9, including the SCI pins */
    case 0xffffd3: return 0xffffd1;     /* PA, including TPC outputs */
    case 0xffffd6: return 0xffffd4;     /* PB, including TPC outputs */
    case 0xffffd7: return 0xffffd5;     /* PC */
    default: return -1;
    }
}

/*
 * The real-time clock hangs on a bit-banged I2C bus: PB7 is SCL and PB2 is
 * SDA, both pulled up and driven low by setting the PBDDR bit with the
 * PBDR bit at 0.
 */
static void e2300_i2c_update(E2300MachineState *s)
{
    uint8_t ddr = s->port[PORT_REG(0xffffd4)];
    uint8_t dr = s->port[PORT_REG(0xffffd6)];
    bool scl = !(ddr & E2300_I2C_SCL) || (dr & E2300_I2C_SCL);
    bool sda = !(ddr & E2300_I2C_SDA) || (dr & E2300_I2C_SDA);

    if (scl != s->i2c_scl) {
        s->i2c_scl = scl;
        s->i2c_sda_line = bitbang_i2c_set(&s->i2c, BITBANG_I2C_SCL, scl);
    }
    if (sda != s->i2c_sda) {
        s->i2c_sda = sda;
        s->i2c_sda_line = bitbang_i2c_set(&s->i2c, BITBANG_I2C_SDA, sda);
    }
}

/*
 * Drive the front panel LEDs and beeper, and the turnstile solenoid, from
 * the P4 output pins.
 */
static void e2300_panel_update(E2300MachineState *s, bool force)
{
    uint8_t level = s->port[PORT_REG(0xffffc7)] & s->port[PORT_REG(0xffffc5)];
    uint8_t changed = force ? 0xff : level ^ s->panel_level;
    int i;

    s->panel_level = level;
    for (i = 0; i < E2300_PANEL_LINES; i++) {
        if (changed & BIT(i)) {
            qemu_set_irq(s->panel[i], extract32(level, i, 1));
        }
    }
    if (changed & E2300_GATE_SOLENOID) {
        qemu_set_irq(s->gate, !!(level & E2300_GATE_SOLENOID));
    }
}

/* The driver button was pressed or released. */
static void e2300_driver_button(void *opaque, int n, int level)
{
    E2300MachineState *s = opaque;

    s->driver_button_pressed = level != 0;
}

/* A turnstile rotation switch opened or closed. */
static void e2300_gate_switch(void *opaque, int n, int level)
{
    E2300MachineState *s = opaque;

    s->gate_switches = deposit32(s->gate_switches, n, 1, level != 0);
}

/* Level of the pins configured as inputs. */
static uint8_t port_pins(E2300MachineState *s, hwaddr reg)
{
    uint8_t pins;

    switch (reg) {
    case 0xffffd6: /* PBDR bits 7 and 2, I2C clock and data */
        return ~((s->i2c_scl ? 0 : E2300_I2C_SCL) |
                 (s->i2c_sda_line ? 0 : E2300_I2C_SDA));
    case 0xffffcf: /* P8DR bits 1-3, IC2 switches 2-4, and driver button */
        pins = ~dip_switch_get(s->ic2) & 0xe;
        if (s->driver_button_pressed) {
            pins &= ~E2300_DRIVER_BUTTON;
        }
        return pins;
    case 0xffffd7: /* PCDR bit 4, IC2 switch 1; bit 6, battery low */
        return (~dip_switch_get(s->ic2) & 0x1) << 4 |
               (s->battery_low ? E2300_BATTERY_LOW : 0);
    case 0xffffcb: /* P6DR bits 0-2, turnstile switches */
        if (s->turnstile) {
            pins = ~(E2300_GATE_SW1 | E2300_GATE_SW2);

            if (s->gate_switches & BIT(TURNSTILE_SW1)) {
                pins |= E2300_GATE_SW1;
            }
            if (s->gate_switches & BIT(TURNSTILE_SW2)) {
                pins |= E2300_GATE_SW2;
            }
            return pins;
        }
        return 0xff;
    default:       /* undriven inputs are pulled up */
        return 0xff;
    }
}

static const char *e2300_switch_mode(uint32_t positions)
{
    switch (positions) {
    case E2300_IC2_ALL_ON:
        return "boot monitor (SENDBIN)";
    case 0:
        return "normal";
    case 1:
        return "normal, application debug output";
    case 8:
        return "application test menu on SCI1";
    default:
        return "normal (polling address only)";
    }
}

/* Report the IC2 positions and what the firmware makes of them. */
static void e2300_switches_trace(E2300MachineState *s)
{
    uint32_t positions = dip_switch_get(s->ic2);
    g_autofree char *text = NULL;

    if (s->ic2_traced == positions) {
        return;
    }
    s->ic2_traced = positions;
    text = dip_switch_format(positions, E2300_IC2_SWITCHES);
    trace_e2300_switches(text, positions, e2300_switch_mode(positions));
}

/* An IC2 output changed; the pins are read from the bank directly. */
static void e2300_switch_changed(void *opaque, int n, int level)
{
    e2300_switches_trace(opaque);
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    E2300MachineState *s = opaque;
    hwaddr reg = H83003_GPIO_BASE + addr;
    int ddr_reg = port_ddr_of(reg);

    if (ddr_reg >= 0) {
        uint8_t ddr = s->port[PORT_REG(ddr_reg)];

        return (s->port[addr] & ddr) | (port_pins(s, reg) & ~ddr);
    }
    switch (reg) {
    case 0xffffda: /* P4PCR */
    case 0xffffdb: /* P5PCR */
        return s->port[addr];
    default:       /* DDRs are write-only; P7 is an input port */
        return 0xff;
    }
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    E2300MachineState *s = opaque;
    hwaddr reg = H83003_GPIO_BASE + addr;

    s->port[addr] = val;
    /* Port B writes are traced with the PC of the writing instruction. */
    if (reg == 0xffffd4 || reg == 0xffffd6) {
        trace_h83003_portb_write(s->cpu.env.pc, reg, val, size);
        e2300_i2c_update(s);
    }
    if (reg == 0xffffc5 || reg == 0xffffc7) {
        e2300_panel_update(s, false);
    }
}

/* TPC outputs: lines 0-7 drive PA0-PA7, lines 8-15 drive PB0-PB7. */
static void e2300_tpc_out(void *opaque, int n, int level)
{
    E2300MachineState *s = opaque;
    uint8_t *dr = &s->port[PORT_REG(n < 8 ? 0xffffd3 : 0xffffd6)];

    *dr = deposit32(*dr, n & 7, 1, level != 0);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.max_access_size = 1,
};

static uint64_t mode_read(void *opaque, hwaddr addr, unsigned size)
{
    E2300MachineState *s = opaque;

    return addr == 0 ? 0xc3 : s->syscr;
}

static void mode_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    E2300MachineState *s = opaque;

    if (addr == 1) {
        s->syscr = val | 0x02;
        memory_region_set_enabled(&s->iram, s->syscr & 1);
    }
}

static const MemoryRegionOps mode_ops = {
    .read = mode_read,
    .write = mode_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.max_access_size = 1,
};

/* Watchdog counter clock divisors for the eight TCSR.CKS values. */
static int64_t wdt_tick_ns(E2300MachineState *s)
{
    static const unsigned divisors[] = { 2, 32, 64, 128, 256, 512, 2048, 4096 };

    return NANOSECONDS_PER_SECOND * divisors[s->tcsr & 7] / E2300_CLOCK_HZ;
}

static uint8_t wdt_count(E2300MachineState *s, int64_t now)
{
    int64_t ticks = s->tcsr & 0x20 ? (now - s->wdt_start) / wdt_tick_ns(s) : 0;

    return s->tcnt + ticks;
}

static void wdt_schedule(E2300MachineState *s, int64_t now)
{
    s->wdt_start = now;
    if (s->tcsr & 0x20) {
        timer_mod(s->wdt_timer, now + (256 - s->tcnt) * wdt_tick_ns(s));
    } else {
        timer_del(s->wdt_timer);
    }
}

/* WOVI is requested while OVF = 1 in interval timer mode. */
static void wdt_update_irq(E2300MachineState *s)
{
    qemu_set_irq(s->wovi, (s->tcsr & 0x80) && !(s->tcsr & 0x40));
}

static void wdt_overflow(void *opaque)
{
    E2300MachineState *s = opaque;

    if (s->tcsr & 0x40) {
        s->wdt_reset_pending = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        /* Interval timer mode: set OVF and request WOVI. */
        s->tcsr |= 0x80;
        s->tcnt = 0;
        wdt_schedule(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        wdt_update_irq(s);
    }
}

static uint64_t wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    E2300MachineState *s = opaque;

    switch (addr) {
    case 0:
        return s->tcsr;
    case 1:
        return wdt_count(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    case 3:
        return s->rstcsr;
    default:
        return 0xff;
    }
}

static void wdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    E2300MachineState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (addr == 0 && (val >> 8) == 0xa5) {
        s->tcnt = val & 0x20 ? wdt_count(s, now) : 0;
        s->tcsr = (s->tcsr & val & 0x80) | (val & 0x67) | 0x18;
        wdt_schedule(s, now);
        wdt_update_irq(s);
    } else if (addr == 0 && (val >> 8) == 0x5a) {
        s->tcnt = val;
        wdt_schedule(s, now);
    } else if (addr == 2 && val == 0xa500) {
        s->rstcsr &= 0x7f;
    } else if (addr == 2 && (val >> 8) == 0x5a) {
        s->rstcsr = (s->rstcsr & 0x80) | (val & 0x40) | 0x3f;
    }
}

static bool wdt_accepts(void *opaque, hwaddr addr, unsigned size,
                        bool is_write, MemTxAttrs attrs)
{
    return is_write ? size == 2 && (addr == 0 || addr == 2) : size == 1;
}

static const MemoryRegionOps wdt_ops = {
    .read = wdt_read,
    .write = wdt_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .valid.accepts = wdt_accepts,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static void e2300_reset(MachineState *machine, ShutdownCause reason)
{
    E2300MachineState *s = E2300_MACHINE(machine);
    const uint8_t *data = memory_region_get_ram_ptr(&s->flash);

    memset(s->port, 0, sizeof(s->port));
    bitbang_i2c_init(&s->i2c, s->i2c_bus);
    s->i2c.state = STOPPED;
    s->i2c_scl = s->i2c_sda = s->i2c_sda_line = true;
    s->syscr = 0x0b;
    memory_region_set_enabled(&s->iram, true);
    s->flash_status = 0x80;
    flash_set_mode(s, FLASH_ARRAY);
    timer_del(s->wdt_timer);
    s->tcsr = 0x18;
    s->tcnt = 0;
    s->rstcsr = s->wdt_reset_pending ? 0xbf : 0x3f;
    s->wdt_reset_pending = false;
    s->gate_switches = 0;
    /* The driver button keeps its state: a reset does not let go of it. */

    qemu_devices_reset(reason);
    /* The LED devices reset to "on"; show the pins, all low after reset. */
    e2300_panel_update(s, true);
    cpu_reset(CPU(&s->cpu));
    /* Reset fetches from the live flash, so programming survives reset. */
    cpu_set_pc(CPU(&s->cpu), ldl_be_p(data) & 0xffffff);
}

static void e2300_load_flash(E2300MachineState *s, const char *filename)
{
    uint8_t *data = memory_region_get_ram_ptr(&s->flash);
    g_autofree uint8_t *image = g_malloc(E2300_FLASH_SIZE + 1);
    ssize_t size;

    memset(data, 0xff, E2300_FLASH_SIZE);
    if (!filename) {
        error_report("e2300 requires an image specified with -bios");
        exit(EXIT_FAILURE);
    }

    size = load_image_size(filename, image, E2300_FLASH_SIZE + 1);
    if (size < 4 || size > E2300_FLASH_SIZE) {
        error_report("e2300: could not load flash image '%s' "
                     "(expected 4 to %zu bytes)", filename,
                     (size_t)E2300_FLASH_SIZE);
        exit(EXIT_FAILURE);
    }
    /* A standalone 16 KiB boot file can include its two-byte host checksum. */
    if (size == 16 * KiB + 2) {
        uint16_t sum = 0;
        size_t i;

        for (i = 0; i < size - 2; i++) {
            sum += image[i];
        }
        if (lduw_be_p(image + size - 2) == (uint16_t)~sum) {
            size -= 2;
        }
    }
    memcpy(data, image, size);
}

static void e2300_init_sci(E2300MachineState *s, RSCIState *sci,
                           const char *name, Chardev *chr, hwaddr base, int vec)
{
    SysBusDevice *sbd;
    int i;

    object_initialize_child(OBJECT(s), name, sci, TYPE_RENESAS_SCI);
    sbd = SYS_BUS_DEVICE(sci);
    qdev_prop_set_chr(DEVICE(sci), "chardev", chr);
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", E2300_CLOCK_HZ);
    qdev_prop_set_uint32(DEVICE(sci), "rev", 0);
    qdev_prop_set_bit(DEVICE(sci), "level-irq", true);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(DEVICE(&s->onchip),
                                                    vec + i));
    }
}

/* The battery detector output changed. */
static void e2300_battery_changed(void *opaque, int n, int level)
{
    E2300MachineState *s = opaque;

    s->battery_low = level != 0;
}

/* Turn the "battery" property (ok, low or a voltage in mV) into mV. */
static bool e2300_battery_mv(const char *value, uint32_t *mv, Error **errp)
{
    uint64_t v;

    if (!g_ascii_strcasecmp(value, "ok")) {
        *mv = E2300_BATTERY_OK_MV;
    } else if (!g_ascii_strcasecmp(value, "low")) {
        *mv = E2300_BATTERY_LOW_MV;
    } else if (!qemu_strtou64(value, NULL, 0, &v) && v <= UINT32_MAX) {
        *mv = v;
    } else {
        error_setg(errp, "battery must be ok, low or a voltage in mV");
        return false;
    }
    return true;
}

static void e2300_init_battery(E2300MachineState *s)
{
    uint32_t mv;

    s->battery = battery_sensor_create_simple(OBJECT(s), "backup-battery",
                                              "backup battery");
    qdev_connect_gpio_out_named(DEVICE(s->battery), "low", 0,
                                qemu_allocate_irq(e2300_battery_changed, s,
                                                  0));
    /* -global battery-sensor.voltage=... works too without "battery". */
    if (s->battery_mode) {
        e2300_battery_mv(s->battery_mode, &mv, &error_fatal);
        object_property_set_uint(OBJECT(s->battery), "voltage", mv,
                                 &error_fatal);
    }
}

/* Translate the "switches" presets into dip-switch positions. */
static const char *e2300_switch_preset(const char *name)
{
    static const struct {
        const char *name, *positions;
    } presets[] = {
        { "monitor", "0xf" },
        { "normal", "0" },
        { "test", "8" },
        { "debug", "1" },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(presets); i++) {
        if (!g_ascii_strcasecmp(name, presets[i].name)) {
            return presets[i].positions;
        }
    }
    return name;
}

static void e2300_init_switches(E2300MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_DIP_SWITCH);
    const char *positions = NULL;
    qemu_irq *in;
    int i;

    qdev_prop_set_uint32(dev, "num-switches", E2300_IC2_SWITCHES);
    qdev_prop_set_string(dev, "label", "IC2");
    /* -global dip-switch.positions=... works too when "switches" is unset. */
    if (s->switches) {
        positions = e2300_switch_preset(s->switches);
    } else if (!DIP_SWITCH(dev)->pending) {
        positions = s->autoboot ? "normal" : "monitor";
        positions = e2300_switch_preset(positions);
    }
    if (positions) {
        object_property_set_str(OBJECT(dev), "positions", positions,
                                &error_fatal);
    }
    object_property_add_child(OBJECT(s), "ic2", OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    s->ic2 = DIP_SWITCH(dev);

    in = qemu_allocate_irqs(e2300_switch_changed, s, E2300_IC2_SWITCHES);
    for (i = 0; i < E2300_IC2_SWITCHES; i++) {
        qdev_connect_gpio_out_named(dev, "switch", i, in[i]);
    }
    e2300_switches_trace(s);
}

static void e2300_init(MachineState *machine)
{
    E2300MachineState *s = E2300_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *owner = OBJECT(machine);
    DeviceState *onchip;
    qemu_irq *tpc;
    Chardev *chr;
    int i;

    if (machine->ram_size != 512 * KiB && machine->ram_size != 1 * MiB) {
        error_report("e2300: RAM size must be 512 KiB or 1 MiB");
        exit(EXIT_FAILURE);
    }

    memory_region_init_rom_device(&s->flash, NULL, &flash_ops, s,
                                  "e2300.flash", E2300_FLASH_SIZE,
                                  &error_fatal);
    e2300_load_flash(s, machine->firmware);
    memory_region_add_subregion(sysmem, 0, &s->flash);

    memory_region_init_io(&s->absent, owner, &absent_ops, s,
                          "e2300.expansion", 2 * MiB - E2300_FLASH_SIZE);
    memory_region_add_subregion_overlap(sysmem, E2300_FLASH_SIZE,
                                        &s->absent, -1);
    memory_region_add_subregion(sysmem, E2300_RAM_BASE, machine->ram);

    memory_region_init_ram(&s->iram, NULL, "e2300.iram", H83003_IRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, H83003_IRAM_BASE, &s->iram);
    memory_region_init_io(&s->gpio, owner, &gpio_ops, s, "e2300.gpio",
                          H83003_GPIO_SIZE);
    memory_region_add_subregion(sysmem, H83003_GPIO_BASE, &s->gpio);
    memory_region_init_io(&s->mode, owner, &mode_ops, s, "e2300.mode", 2);
    memory_region_add_subregion(sysmem, H83003_MODE_BASE, &s->mode);
    memory_region_init_io(&s->wdt, owner, &wdt_ops, s, "e2300.wdt", 4);
    memory_region_add_subregion(sysmem, H83003_WDT_BASE, &s->wdt);
    s->wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_overflow, s);

    object_initialize_child(owner, "cpu", &s->cpu, TYPE_H8300_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);

    /* On-chip modules decode the whole 0xFFFF00 page below board devices. */
    object_initialize_child(owner, "onchip", &s->onchip, TYPE_H83003_ONCHIP);
    onchip = DEVICE(&s->onchip);
    qdev_prop_set_uint64(onchip, "clock-frequency", E2300_CLOCK_HZ);
    sysbus_realize(SYS_BUS_DEVICE(onchip), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(onchip), 0, H83003_ONCHIP_BASE, -1);

    /* DS1307-compatible real-time clock on the bit-banged I2C bus. */
    s->i2c_bus = i2c_init_bus(onchip, "i2c");
    bitbang_i2c_init(&s->i2c, s->i2c_bus);
    s->i2c_scl = s->i2c_sda = s->i2c_sda_line = true;
    i2c_slave_create_simple(s->i2c_bus, "ds1338", E2300_RTC_ADDR);

    /*
     * Front panel display.  Its text can be logged with
     * -chardev file,id=disp,path=display.log -global hdsp-211x.chardev=disp
     * or with -trace hdsp211x_display.
     */
    object_initialize_child(OBJECT(machine), "display", &s->display,
                            TYPE_HDSP211X);
    sysbus_realize(SYS_BUS_DEVICE(&s->display), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->display), 0, E2300_DISPLAY_BASE);

    /*
     * IC2 boot and mode switches.  Set them with switches=<positions>,
     * where <positions> is monitor, normal, test, debug, a list such as
     * on:off:off:on (switch 1 first) or a number such as 8; without it,
     * autoboot=on means all OFF and autoboot=off all ON.  "qom-set /machine
     * switches test" or "qom-set /machine/ic2 positions 0x8" moves them at
     * run time; -trace e2300_switches (or dip_switch_set) reports them.
     */
    e2300_init_switches(s);

    /*
     * Front panel lamps on port 4, one device each so that they show up
     * separately: -trace led_change_intensity reports the LEDs (desc 'red',
     * 'green', 'yellow') and -trace "gpio_beeper*" the beeper; "qom-get
     * /machine/beeper on" reads it.
     */
    s->panel[E2300_PANEL_RED] = qdev_get_gpio_in(DEVICE(
        led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_HIGH,
                          LED_COLOR_RED, "red")), 0);
    s->panel[E2300_PANEL_GREEN] = qdev_get_gpio_in(DEVICE(
        led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_HIGH,
                          LED_COLOR_GREEN, "green")), 0);
    s->panel[E2300_PANEL_YELLOW] = qdev_get_gpio_in(DEVICE(
        led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_HIGH,
                          LED_COLOR_YELLOW, "yellow")), 0);
    s->panel[E2300_PANEL_BEEP] = qdev_get_gpio_in(DEVICE(
        gpio_beeper_create_simple(OBJECT(machine), "beeper", "beep")), 0);

    /*
     * Backup battery detector on PC6.  battery=low (or a voltage in mV
     * below the 2500 mV threshold) makes the application show "BAT ERR";
     * "qom-set /machine battery low" or "qom-set /machine/backup-battery
     * voltage 2200" change it at run time, seen by the firmware after the
     * next reset.  -trace battery_sensor_level reports the detector.
     */
    e2300_init_battery(s);

    /*
     * Turnstile on P4.7 (solenoid) and P6 (switches), unless turnstile=off,
     * which leaves P6 pulled up: checkgate() then reports a completed
     * rotation at once.  A passenger passes one second after each release;
     * -global turnstile.auto=off disables that, "qom-set /machine/gate
     * push on" pushes the arm and "qom-get /machine/gate passages"
     * counts them.  -trace "turnstile*" shows the solenoid and switches.
     */
    if (s->has_turnstile) {
        s->turnstile = turnstile_create_simple(OBJECT(machine), "gate",
                                               "gate");
        s->gate = qdev_get_gpio_in(DEVICE(s->turnstile), 0);
        for (i = 0; i < TURNSTILE_SWITCHES; i++) {
            qdev_connect_gpio_out_named(DEVICE(s->turnstile), "switch", i,
                                        qemu_allocate_irq(e2300_gate_switch,
                                                          s, i));
        }
    }

    /*
     * Driver button on P8.1.  "qom-set /machine/driver-button press on"
     * presses it for 3 s (-global push-button.hold-time=<ms> changes that);
     * "qom-set /machine/driver-button pressed on|off" holds and releases
     * it.  The application looks at the button once per main loop pass and
     * then wants it held for one more second, so a press must outlast the
     * loop: with the radio still searching for a base, 1.5 s is not always
     * enough.  -trace push_button_state shows the button.
     */
    s->driver_button = push_button_create_simple(OBJECT(machine),
                                                 "driver-button", "driver");
    qdev_connect_gpio_out(DEVICE(s->driver_button), 0,
                          qemu_allocate_irq(e2300_driver_button, s, 0));

    /*
     * Temperature sensor on AN5, /machine/thermistor.  "qom-set
     * /machine/thermistor temperature 31500" sets 31.5 degrees Celsius
     * (25 by default); -trace ntc_thermistor_update shows the A/D input.
     * gettemp() is only right with a calibration word at 0x6016 (0x2000
     * for these values, "E 16 2000" at the boot monitor): with a blank
     * one (0xffff) 25 degrees read as -19.
     */
    ntc_thermistor_create_simple(OBJECT(machine), "thermistor", "board",
                                 E2300_THERM_R25, E2300_THERM_BETA,
                                 E2300_THERM_RREF,
                                 qdev_get_gpio_in_named(onchip, "an",
                                                        E2300_THERM_AN));

    /*
     * Proximity card reader.  Give it a card with
     * -global mifare-mcm.card=card.bin, or place a blank one with
     * -global mifare-mcm.present=on; "qom-set /machine/mifare present ..."
     * changes it at run time and -trace mifare_mcm_event shows the traffic.
     */
    object_initialize_child(OBJECT(machine), "mifare", &s->mifare,
                            TYPE_MIFARE_MCM);
    sysbus_realize(SYS_BUS_DEVICE(&s->mifare), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mifare), 0, E2300_MIFARE_BASE);

    /*
     * Quad UART used by the operating system (inisams, testsams and
     * putrs/getrs/checkrs channels 0x10-0x13).  Each channel can be
     * connected with -global sc28c94.chardevN=<id>.
     */
    object_initialize_child(OBJECT(machine), "quart", &s->quart,
                            TYPE_SC28C94);
    sysbus_realize(SYS_BUS_DEVICE(&s->quart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->quart), 0, E2300_QUART_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->quart), 0,
                       qdev_get_gpio_in_named(onchip, "irq-pin", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(onchip), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), H8300_CPU_IRQ));
    s->cpu.env.ack = qdev_get_gpio_in_named(onchip, "ack", 0);
    tpc = qemu_allocate_irqs(e2300_tpc_out, s, H83003_TPC_LINES);
    for (i = 0; i < H83003_TPC_LINES; i++) {
        qdev_connect_gpio_out_named(onchip, "tpc", i, tpc[i]);
    }
    s->wovi = qdev_get_gpio_in(onchip, H83003_VEC_WOVI);

    /*
     * SCI1 is the boot monitor port and the radio port (-serial #0); SCI0
     * is -serial #1.  The application drives an RFM433-LC radio modem on
     * SCI1 ("+++", ATS200=n, ATO).  With radio=on (the default) the modem
     * sits between SCI1 and -serial #0, which then is the radio side: in
     * transparent mode, the modem's power-on state, data passes unchanged,
     * so the boot monitor still works.  -trace "rfm433*" shows the modem.
     */
    chr = serial_hd(0);
    if (s->radio) {
        chr = rfm433_new("e2300.radio", chr, &error_fatal);
    }
    e2300_init_sci(s, &s->sci1, "sci1", chr, H83003_SCI1_BASE, H83003_VEC_SCI1);
    e2300_init_sci(s, &s->sci0, "sci0", serial_hd(1), H83003_SCI0_BASE,
                   H83003_VEC_SCI0);
}

static bool e2300_get_autoboot(Object *obj, Error **errp)
{
    return E2300_MACHINE(obj)->autoboot;
}

static void e2300_set_autoboot(Object *obj, bool enabled, Error **errp)
{
    E2300_MACHINE(obj)->autoboot = enabled;
}

static char *e2300_get_switches(Object *obj, Error **errp)
{
    E2300MachineState *s = E2300_MACHINE(obj);

    if (s->ic2) {
        return dip_switch_format(dip_switch_get(s->ic2), E2300_IC2_SWITCHES);
    }
    return g_strdup(s->switches ? s->switches : "");
}

static void e2300_set_switches(Object *obj, const char *value, Error **errp)
{
    E2300MachineState *s = E2300_MACHINE(obj);

    if (s->ic2) {
        /* At run time, move the switches of the existing bank. */
        object_property_set_str(OBJECT(s->ic2), "positions",
                                e2300_switch_preset(value), errp);
        return;
    }
    g_free(s->switches);
    s->switches = g_strdup(value);
}

static bool e2300_get_radio(Object *obj, Error **errp)
{
    return E2300_MACHINE(obj)->radio;
}

static void e2300_set_radio(Object *obj, bool enabled, Error **errp)
{
    E2300_MACHINE(obj)->radio = enabled;
}

static bool e2300_get_turnstile(Object *obj, Error **errp)
{
    return E2300_MACHINE(obj)->has_turnstile;
}

static void e2300_set_turnstile(Object *obj, bool enabled, Error **errp)
{
    E2300_MACHINE(obj)->has_turnstile = enabled;
}

static char *e2300_get_battery(Object *obj, Error **errp)
{
    E2300MachineState *s = E2300_MACHINE(obj);

    if (s->battery) {
        return g_strdup(battery_sensor_is_low(s->battery) ? "low" : "ok");
    }
    return g_strdup(s->battery_mode ? s->battery_mode : "ok");
}

static void e2300_set_battery(Object *obj, const char *value, Error **errp)
{
    E2300MachineState *s = E2300_MACHINE(obj);
    uint32_t mv;

    if (!e2300_battery_mv(value, &mv, errp)) {
        return;
    }
    if (s->battery) {
        /* At run time, change the voltage of the existing battery. */
        object_property_set_uint(OBJECT(s->battery), "voltage", mv, errp);
        return;
    }
    g_free(s->battery_mode);
    s->battery_mode = g_strdup(value);
}

static void e2300_instance_init(Object *obj)
{
    E2300_MACHINE(obj)->radio = true;
    E2300_MACHINE(obj)->has_turnstile = true;
    E2300_MACHINE(obj)->ic2_traced = -1;
}

static void e2300_instance_finalize(Object *obj)
{
    g_free(E2300_MACHINE(obj)->switches);
    g_free(E2300_MACHINE(obj)->battery_mode);
}

static void e2300_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "E2300 (H8/3003 boot monitor and operating system)";
    mc->init = e2300_init;
    mc->reset = e2300_reset;
    mc->default_cpu_type = TYPE_H8300_CPU;
    mc->default_ram_id = "e2300.ram";
    mc->default_ram_size = 1 * MiB;
    mc->max_cpus = 1;
    object_class_property_add_bool(oc, "autoboot",
                                   e2300_get_autoboot, e2300_set_autoboot);
    object_class_property_set_description(oc, "autoboot",
        "IC2 switches all OFF (OS startup) when no 'switches' is given; "
        "default all ON (boot monitor)");
    object_class_property_add_str(oc, "switches",
                                  e2300_get_switches, e2300_set_switches);
    object_class_property_set_description(oc, "switches",
        "IC2 DIP switches: monitor, normal, test, debug, a list such as "
        "on:off:off:on (switch 1 first) or a bit mask such as 8");
    object_class_property_add_bool(oc, "radio",
                                   e2300_get_radio, e2300_set_radio);
    object_class_property_set_description(oc, "radio",
                                          "RFM433-LC radio modem on SCI1");
    object_class_property_add_bool(oc, "turnstile",
                                   e2300_get_turnstile, e2300_set_turnstile);
    object_class_property_set_description(oc, "turnstile",
        "Turnstile on P4.7 (solenoid) and P6 (switches); "
        "off leaves P6 pulled up");
    object_class_property_add_str(oc, "battery",
                                  e2300_get_battery, e2300_set_battery);
    object_class_property_set_description(oc, "battery",
        "Backup battery: ok (default), low or a voltage in mV; "
        "the firmware sees a change after the next reset");
}

static const TypeInfo e2300_type = {
    .name = TYPE_E2300_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(E2300MachineState),
    .instance_init = e2300_instance_init,
    .instance_finalize = e2300_instance_finalize,
    .class_init = e2300_class_init,
};

static void e2300_register_types(void)
{
    type_register_static(&e2300_type);
}

type_init(e2300_register_types)
