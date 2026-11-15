# QEMU H8/3003 project instructions

## Repository and scope

- Work on branch `h83003` in the `igorsilvamenezes/qemu-h8300` fork.
  The upstream baseline is `ysat0/qemu-h8300`, branch `h8300`, commit
  `7db0398`.
- This repository owns the emulator, the shared firmware launcher, and its
  documentation. Firmware sources, build files, and test expectations
  belong to their respective projects in the sibling `H83003/` workspace.
- Read [the firmware workspace instructions](../H83003/codex.md) in full
  before planning, creating, editing, or validating anything in `H83003/`.
  Ask before recording a new standing convention there unless the user
  explicitly requests that it be recorded.
- Use English for source comments and project instructions. Communicate
  with the user in Portuguese.

## Emulator and launcher

- Build the `h8300-softmmu` target. The workspace launcher defaults to
  `build-h8/qemu-system-h8300`; use `--qemu` to select a different build.
- Preserve the KaneBebe startup fixes, the optional Port B model, and its
  trace event. KaneBebe emulates an H8/3069 in the H8/300H family. Enable
  the H8/3003 Port B model with `-M KaneBebe,h83003-portb=on`. Treat other
  H8/3003 peripherals as unsupported until their behavior is verified.
- Load the raw ROM with `-bios`. Keep
  `-accel tcg,one-insn-per-tb=on` in interactive and test runs because
  this fork has shown a failure with the default TCG configuration.
- Preserve the `h83003_portb_write` trace event. It records the PC of the
  instruction that performed the write, the register address, the value,
  and the access size. Use the event to associate a write with its PC.
  CPU trace samples provide additional evidence for control flow.
- Use [the shared launcher](scripts/qemu-h83003.md) for interactive and
  automated runs. Its dependencies are Python 3 and, for tests, GNU
  `timeout`.

```sh
qemu-h83003 build/blink.bin
qemu-h83003 --test build/blink.bin
qemu-h83003 --test build/blink.bin expectations.json
qemu-h83003 --test --profile expectations.json build/blink.bin
```

## Firmware expectations and validation

- Keep each JSON expectations file with the firmware sources. For
  `build/firmware.bin`, save it as `firmware.qemu.json` in the project
  root. The launcher checks next to the ROM first and then in the project
  root for ROMs under `build/`. An explicit path overrides lookup. The
  positional path requires `--test`; use either it or `--profile`.
- Use the shared launcher and declarative expectations for new QEMU tests.
  Preserve the `make run-qemu` and `make test-qemu` entry points and the
  `QEMU_LAUNCHER`, `QEMU`, and `QEMU_RUN_LOG` overrides.
- Interactive runs open the monitor and log to `build/qemu-run.log` by
  default. Use `stop`, `info registers`, `xp /1bx 0xffffd6`, `cont`,
  and `quit` to inspect or control the run.
- Automated runs use the host timeout set in the profile and log to
  `build/qemu-trace.log` by default. The expected timeout stops the
  firmware loop. The trace must also satisfy the profile for the launcher
  to return success. KaneBebe may warn that its network device has no peer.
- For shared changes, run the six reference projects with `make all test-qemu`
  as applicable. The C and Assembly variants cover a steady PB0 output and
  an idle loop, alternating PB0, and two full rotations across PB0 through
  PB3. Keep the expectations in each firmware project as the source for
  exact register writes, PCs, and minimum repetition counts.
- Report how to build and run the firmware, what evidence was checked,
  and which timing or electrical assumptions require measurement on hardware.
  The existing blink profiles verify GPIO sequences in emulation.

## Firmware conventions

- Use a separate numbered directory for each example, such as
  `00-asm-simple` or `02-c-blink-four`. A C counterpart keeps the
  Assembly example's number and descriptive name.
- Target the H8/3003 with the H8/300H core in advanced mode. Reference
  images contain a four-byte reset vector at `0x000000` pointing to
  `_start` at `0x000100`. The raw image starts at address `0x000000`.
- Provide a Makefile, linker script, and README. Generate the raw image,
  COFF, link map, and disassembly under `build/` with `make` and remove
  generated files with `make clean`. Use GNU `h8300-hms-*` tools with a
  configurable `CROSS` prefix and `h8300-hms-gcc` for C.
- Start source files, test scripts, linker scripts, and Makefiles with an
  English description of their purpose and relevant assumptions. For
  firmware C, document functions, structures, and variable declaration
  blocks. Comment each code line in Assembly, linker scripts, and test
  scripts. Group Makefile variables at the top, separate rules with blank
  lines, and wrap long commands.
- For simple C blink examples, define `DELAY_COUNT` in the source that
  uses it and document the timing assumptions. Keep the documented
  Assembly defaults of `888887` and `888885` for single and four LED
  blink, respectively. Their calculations assume a 16 MHz clock and
  external 16-bit ROM with three access states and three wait states.
  The reference profiles use `icount_shift` 9 for Assembly and 10 for C
  blink examples. Those settings control simulated time.
- Consult the local H8/3003 hardware manual, H8/300 programming manual,
  and H8/300H reference before selecting register addresses, instructions,
  or timings. The H8/3003 hardware manual covers Port B in section 9.9
  and instruction timing in tables A-2 and A-3.
- Reference LEDs are active high. Write the initial latch value to
  `PBDR` at `0xFFFFD6` before enabling outputs. Write `PBDDR` at
  `0xFFFFD4` directly because it is write-only. Use `0x01` for PB0 or
  `0x0F` for PB0 through PB3. Confirm polarity, wiring, and timing on
  the physical board.
- The Assembly steady LED project also offers `make test` with the
  neighboring `E2300_MICRO` simulator as a supplementary check.
