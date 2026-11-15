#!/usr/bin/env python3
"""Exercise the original E2300 boot ROM through its SCI1 serial monitor.

The caller supplies E2300bt1.bin including its transport checksum. QEMU
opens it read-only. All writes and resets happen inside the emulated board.
"""

import argparse
import os
from pathlib import Path
import select
import subprocess
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


if __name__ == "__main__":
    main()
