H8/300 system emulator
======================

E2300 boot monitor
------------------

The ``e2300`` machine models the H8/3003 in advanced mode 3 with a 16 MHz
clock, SCI1, the watchdog, and the GPIO used by the E2300 boot monitor.
SCI1 is connected to the first QEMU serial backend. The boot configures
it for asynchronous 8N1 at approximately 19,200 baud.

Run the original boot image from the QEMU checkout::

   build-h8/qemu-system-h8300 -M e2300 -bios /path/to/E2300bt1.bin \
       -display none -monitor none -serial stdio \
       -accel tcg,one-insn-per-tb=on

The serial prompt is ``>``. Enter ``S`` to view the boot status or
``X 102 2C2E`` to calculate the boot CRC. A terminal must send carriage
return at the end of each command.

The flash is a byte-wide 256 KiB Intel 28F002 bottom-boot device mapped
at 0x000000. A standalone 16 KiB boot image can contain an additional
two-byte transport checksum. QEMU recognizes a matching checksum and
leaves those two flash locations erased. Flash programming and erasing
complete immediately. Changes persist across guest resets for the
lifetime of QEMU and the input file is opened for reading only.

External RAM starts at 0x100000. The default is 1 MiB, and ``-m 512k``
models a board without the second RAM bank. The second flash socket is
empty. The empty banks return 0xff and discard writes.

The switches select the monitor by default. Use ``-M e2300,autoboot=on``
to select automatic startup. A combined raw flash image can include the
OS at 0x008000 and the application at 0x01c000. Place their payloads in
the flash without their two-byte transport checksums. The board model
covers the boot monitor's peripherals. The OS and application can require
additional devices.

The IC24 DIP switches, ``/machine/ic24``, are only used to replace the
boot program. ``-M e2300,boot-switches=normal`` (switches 1 and 2 ON,
3 and 4 OFF) is the default; ``boot-switches=boot-swap`` (1 and 2 OFF,
3 and 4 ON) inverts flash address line A16, so the CPU sees the flash
at 0x00000-0x0FFFF and 0x10000-0x1FFFF swapped, and likewise above.
Other positions log a guest error and keep A16 straight. The A16
inversion is inferred from the boot's Z1 and Z2 commands and from
SENDBIN; the board schematic is not available. To replace the boot as
SENDBIN describes, send the new boot with ``Z1`` (it erases the main
flash block, 0x8000-0x1FFFF), run ``qom-set /machine boot-switches
boot-swap`` and ``system_reset`` in the monitor, send ``Z2``, return
the switches to ``normal`` with another reset, and reload the OS and
the application. ``-trace e2300_ic24`` reports the switches.

The serial regression uses the original boot image, including its
transport checksum. It checks the boot CRC, RAM and parameter flash,
the Z1 upload and Z2 copy, watchdog reset, and the boot replacement
through IC24::

   python3 tests/h8300/test-e2300.py /path/to/E2300bt1.bin

Pass ``--accel tcg`` to exercise MMIO recovery with multi-instruction
translation blocks.
