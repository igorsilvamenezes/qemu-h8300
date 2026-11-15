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
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/char/renesas_sci.h"
#include "hw/display/hdsp211x.h"
#include "hw/misc/mifare_mcm.h"
#include "hw/char/sc28c94.h"
#include "hw/h8300/h83003_onchip.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "trace/trace-system.h"

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
    uint8_t syscr;
    bool autoboot;
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

/* Level of the pins configured as inputs. */
static uint8_t port_pins(E2300MachineState *s, hwaddr reg)
{
    switch (reg) {
    case 0xffffcf: /* P8DR bits 1-3, IC2 boot mode switches */
        return s->autoboot ? 0x02 : 0;
    case 0xffffd7: /* PCDR bit 4, IC2 boot mode switch */
        return s->autoboot ? 0x10 : 0;
    default:       /* undriven inputs are pulled up */
        return 0xff;
    }
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
    s->syscr = 0x0b;
    memory_region_set_enabled(&s->iram, true);
    s->flash_status = 0x80;
    flash_set_mode(s, FLASH_ARRAY);
    timer_del(s->wdt_timer);
    s->tcsr = 0x18;
    s->tcnt = 0;
    s->rstcsr = s->wdt_reset_pending ? 0xbf : 0x3f;
    s->wdt_reset_pending = false;

    qemu_devices_reset(reason);
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
                           const char *name, int serial, hwaddr base, int vec)
{
    SysBusDevice *sbd;
    int i;

    object_initialize_child(OBJECT(s), name, sci, TYPE_RENESAS_SCI);
    sbd = SYS_BUS_DEVICE(sci);
    qdev_prop_set_chr(DEVICE(sci), "chardev", serial_hd(serial));
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

static void e2300_init(MachineState *machine)
{
    E2300MachineState *s = E2300_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *owner = OBJECT(machine);
    DeviceState *onchip;
    qemu_irq *tpc;
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

    /* SCI1 is the boot monitor port (-serial #0); SCI0 is -serial #1. */
    e2300_init_sci(s, &s->sci1, "sci1", 0, H83003_SCI1_BASE, H83003_VEC_SCI1);
    e2300_init_sci(s, &s->sci0, "sci0", 1, H83003_SCI0_BASE, H83003_VEC_SCI0);
}

static bool e2300_get_autoboot(Object *obj, Error **errp)
{
    return E2300_MACHINE(obj)->autoboot;
}

static void e2300_set_autoboot(Object *obj, bool enabled, Error **errp)
{
    E2300_MACHINE(obj)->autoboot = enabled;
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
                                          "Set boot switches for OS startup");
}

static const TypeInfo e2300_type = {
    .name = TYPE_E2300_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(E2300MachineState),
    .class_init = e2300_class_init,
};

static void e2300_register_types(void)
{
    type_register_static(&e2300_type);
}

type_init(e2300_register_types)
