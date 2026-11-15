#!/usr/bin/env python3
"""Exercise the original E2300 boot ROM through its SCI1 serial monitor.

The caller supplies E2300bt1.bin including its transport checksum. QEMU
opens it read-only. All writes and resets happen inside the emulated board.
The boot replacement check drives the IC24 switches through the HMP
monitor on a UNIX socket in a temporary directory.
"""

import argparse
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time


PROMPT = b">"
TIMEOUT = 20


def expect(serial, marker, expected=None):
    """Read through a response marker without consuming the next response."""
    deadline = time.monotonic() + TIMEOUT
    response = bytearray()
    while not response.endswith(marker):
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([serial], [], [], remaining)[0]:
            raise RuntimeError(f"Timed out waiting for {marker!r}: {bytes(response)!r}")
        byte = serial.read(1)
        if not byte:
            raise RuntimeError(f"QEMU closed SCI1: {bytes(response)!r}")
        response += byte
    if expected is not None and expected not in response:
        raise RuntimeError(f"Expected {expected!r}, received {bytes(response)!r}")


def send(serial, data):
    """Send bytes to QEMU's serial pipe within a bounded time."""
    deadline = time.monotonic() + TIMEOUT
    pending = memoryview(data)
    while pending:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([], [serial], [], remaining)[1]:
            raise RuntimeError("Timed out writing to SCI1")
        sent = os.write(serial.fileno(), pending[:select.PIPE_BUF])
        pending = pending[sent:]


def check_monitor(qemu, rom, accel):
    """Check CRC, memory, serial upload, and persistence across watchdog reset."""
    image = rom.read_bytes()
    if len(image) != 0x4002:
        raise ValueError("Expected the 16 KiB E2300 boot image and two-byte trailer")

    args = [
        qemu, "-M", "e2300", "-bios", rom, "-accel", accel,
        "-display", "none", "-monitor", "none", "-serial", "stdio",
    ]
    with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          bufsize=0) as proc:
        try:
            expect(proc.stdout, PROMPT)
            for command, reply in (
                (b"S\r", b"\r\n0001 4523 7A26 "),
                (b"X 102 2C2E\r", b"\r\n7A26\r\n"),
                (b"W 100800 55\r", b"W 100800 55\r\n"),
                (b"D 100800 100802\r", b"\r\n55 00 "),
                (b"E 0\r", b"\r\nFFFF\r\n"),
                (b"E 0 1234\r", b"\r\nF\r\n"),
            ):
                send(proc.stdin, command)
                expect(proc.stdout, PROMPT, reply)

            send(proc.stdin, b"Z1\r")
            expect(proc.stdout, b"Z1\r\n")
            send(proc.stdin, image)
            expect(proc.stdout, PROMPT, b"N\r\nF 8B2F\r\n")

            send(proc.stdin, b"B\r")
            expect(proc.stdout, PROMPT, b"B")
            for command, reply in (
                (b"D FFFFAB FFFFAC\r", b"\r\nBF "),
                (b"D 100800 100802\r", b"\r\n55 00 "),
                (b"E 0\r", b"\r\n1234\r\n"),
                (b"X 10000 14000\r", b"\r\n8B2F\r\n"),
                (b"Z2\r", b"\r\nF 8B2F\r\n"),
            ):
                send(proc.stdin, command)
                expect(proc.stdout, PROMPT, reply)
        finally:
            proc.kill()


def crc_arc(data):
    """CRC-16/ARC, the algorithm of the boot's header check and X command."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def new_boot(image):
    """Copy of the boot with version 16 renamed 99, header CRC and trailer."""
    boot = bytearray(image[:0x4000])
    begin = int.from_bytes(boot[0x104:0x108], "big")
    end = int.from_bytes(boot[0x108:0x10C], "big")
    if boot[0x10C:0x11C] != b"E2300BT V1.12/16":
        raise ValueError("Expected E2300BT V1.12/16")
    boot[0x11A:0x11C] = b"99"
    boot[0x100:0x102] = crc_arc(boot[begin:end]).to_bytes(2, "big")
    return bytes(boot) + ((0xFFFF - sum(boot)) & 0xFFFF).to_bytes(2, "big")


def swapped_hex(value):
    """The monitor prints CRCs with their bytes swapped."""
    return b"%02X%02X" % (value & 0xFF, value >> 8)


class Monitor:
    """Minimal HMP client."""

    def __init__(self, path):
        deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(TIMEOUT)
        self.read()

    def read(self):
        data = b""
        while not data.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError(f"QEMU closed the monitor: {data!r}")
            data += chunk
        return data

    def command(self, text):
        self.sock.sendall(text.encode() + b"\n")
        return self.read()


def check_boot_swap(qemu, rom, accel):
    """Replace the boot with Z1, IC24 boot-swap, Z2 and IC24 normal."""
    image = rom.read_bytes()
    boot = new_boot(image)
    old_crc = swapped_hex(crc_arc(image[0x102:0x2C2E]))
    new_crc = swapped_hex(crc_arc(boot[0x102:0x2C2E]))
    upload_crc = swapped_hex(crc_arc(boot[:0x4000]))
    with tempfile.TemporaryDirectory(prefix="e2300-") as tmp:
        mon = os.path.join(tmp, "hmp")
        args = [
            qemu, "-M", "e2300", "-bios", rom, "-accel", accel,
            "-display", "none", "-serial", "stdio",
            "-monitor", f"unix:{mon},server=on,wait=off",
        ]
        with subprocess.Popen(args, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, bufsize=0) as proc:
            try:
                monitor = Monitor(mon)
                expect(proc.stdout, PROMPT)

                # Z1 programs the new boot at 0x10000; the running one stays.
                send(proc.stdin, b"Z1\r")
                expect(proc.stdout, b"Z1\r\n")
                send(proc.stdin, boot)
                expect(proc.stdout, PROMPT, b"N\r\nF " + upload_crc + b"\r\n")
                send(proc.stdin, b"X 102 2C2E\r")
                expect(proc.stdout, PROMPT, b"\r\n" + old_crc + b"\r\n")

                # Switches 1 and 2 OFF, 3 and 4 ON: the new boot starts.
                monitor.command("qom-set /machine boot-switches boot-swap")
                monitor.command("system_reset")
                expect(proc.stdout, PROMPT)
                for command, reply in (
                    (b"X 102 2C2E\r", b"\r\n" + new_crc + b"\r\n"),
                    (b"D 11A 11C\r", b"\r\n39 39 "),
                    (b"D 1011A 1011C\r", b"\r\n31 36 "),
                    (b"Z2\r", b"\r\nF " + upload_crc + b"\r\n"),
                    (b"D 1011A 1011C\r", b"\r\n39 39 "),
                ):
                    send(proc.stdin, command)
                    expect(proc.stdout, PROMPT, reply)

                # Original setting: the new boot is now in the boot block.
                monitor.command("qom-set /machine boot-switches normal")
                monitor.command("system_reset")
                expect(proc.stdout, PROMPT)
                for command, reply in (
                    (b"X 102 2C2E\r", b"\r\n" + new_crc + b"\r\n"),
                    (b"D 11A 11C\r", b"\r\n39 39 "),
                ):
                    send(proc.stdin, command)
                    expect(proc.stdout, PROMPT, reply)

                # Undefined positions keep A16 straight.
                monitor.command("qom-set /machine/ic24 positions on:off:on:off")
                send(proc.stdin, b"D 11A 11C\r")
                expect(proc.stdout, PROMPT, b"\r\n39 39 ")
            finally:
                proc.kill()


def main():
    """Run the serial regression against an existing QEMU executable."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--qemu", type=Path, default=Path(
        "build-h8/qemu-system-h8300"))
    parser.add_argument("--accel", default="tcg,one-insn-per-tb=on")
    args = parser.parse_args()
    check_monitor(args.qemu.resolve(), args.rom.resolve(), args.accel)
    print(f"PASS: E2300 monitor, flash programming and watchdog ({args.accel})")
    check_boot_swap(args.qemu.resolve(), args.rom.resolve(), args.accel)
    print(f"PASS: E2300 boot replacement with IC24 ({args.accel})")


if __name__ == "__main__":
    main()
