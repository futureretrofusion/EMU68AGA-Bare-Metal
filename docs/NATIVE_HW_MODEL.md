# Native hardware presence model (experimental)

**Goal:** EMU68AGA Bare Metal should run AmigaOS, AGS and games even
when no physical Amiga peripheral is attached. Software should see a
consistent **missing/idle** state, not hang waiting for phantom signals.
We must also allow a future physical hardware adapter to supply the
same register and interrupt interface without changing Amiga software.

## First implemented boundary: A1200 Gayle/IDE/PCMCIA

Enable only on a standalone AArch64 build with
`-DFRF_VIRTUAL_GAYLE_ABSENT=ON`. Off by default until hardware-tested.

`machine/Emu68/src/frf_native_hw/frf_native_bus.[ch]` intercepts
recognized Gayle and IDE accesses **before** the generic unmapped-bus
fallback in `vectors.c`. For an empty Gayle/IDE interface:

- IDE status / alternate-status access returns the absent-drive, not-busy
  `0x7F` convention used by UAE. Other IDE registers read as open bus.
- PCMCIA card-detect, IDE/card interrupt status are inactive, and writes
  cannot conjure an interrupt or complete a nonexistent operation.
- Gayle interrupt-enable and configuration registers retain their values.
- Normal chipset, CIA and SD/RDB device accesses keep their existing paths.
- Operations use constant-time register reads/writes and no polling loops.

The module includes a future hardware backend interface through
`frf_native_bus_set_backend()`; a hardware provider can intercept accesses
with bounded deadlines, and must report errors/no device cleanly if absent.
It is **not** a complete Gayle chip or a full physical IDE bridge.

This is emulation-side compatibility work, **not an AGS software patch**.
Do not modify Workbench Startup-Sequence, WHDLoad, DOSDrivers or RDB images
in order to test the firmware. The previous DOS mounting script workaround
is superseded as a proposed solution; use it only as an independent
diagnostic if deliberately requested.

## Still missing

- Real hardware testing of Gayle mapping, no-card semantics, and desktop
  boot/game recovery (currently not established as the stall root cause).
- Correct hardware personality selection (A500 vs A1200 vs CD32),
  the full IDE register protocol and eventual real IDE passthrough.
- An appropriate RTC/no-clock model and unused hardware probe logging.
- Safe missing-device completion for optional peripheral drivers, including
  third-party PCMCIA, floppy, SCSI or external storage. A DOS mount is an
  OS-level operation, not a chip register: actual mounted volumes and
  RDB partition information must continue to come from the real
  storage devices exposed to AmigaOS.

## Tests

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  -Imachine/Emu68/src/frf_native_hw \
  machine/Emu68/src/frf_native_hw/frf_native_bus.c \
  tests/test_native_bus.c -o /tmp/test_native_bus
/tmp/test_native_bus
```

Tests cover no-IDE busy, no-card, no phantom IRQ, controlled Gayle register
writes, open-bus aliases, untouched AGA addresses, and a future backend
callback replacing absent responses. These are unit tests, not proof that
the target ROM or mounted disk stops waiting.

**Test sequence on Pi 3A+:** with the known-working v0.10.22 boot files
keep the SD-card unchanged and swap only `kernel8.img`. Boot AGS and
Workbench, launch/exits games multiple times, and check physical USB
input, native SD partitions and no console wait loops. On a failure,
restore the previously verified kernel; retain logs and the exact
last requested DOS volume/hardware device. No physical passthrough is
enabled in this initial build.

## Reference

- Linux m68k Gayle register definitions:
  https://codebrowser.dev/linux/linux/arch/m68k/include/asm/amigayle.h.html
- UAE/FS-UAE Gayle absent IDE convention:
  https://sources.debian.org/src/fs-uae/2.8.0%2Bdfsg-2~bpo8%2B1/src/gayle.cpp
