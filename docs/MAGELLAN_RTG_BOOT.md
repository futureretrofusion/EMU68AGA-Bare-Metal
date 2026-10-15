# Magellan / Picasso96 RTG on standalone EMU68AGA

## Observed in the user's read-only October 2026 image

Inspected `MagellanBackupOct2026.img` (about 116 GiB). It has:
- MBR slot 0: FAT32 Pi boot partition, begins at sector 2048.
- MBR slot 1: type 0x76 containing valid Amiga RDB, begins at sector
  2301952; PFS3 volumes include Workbench, AGS_Drive, Work, WHDLoad,
  Games, Premium, ScummVM and Emulators.
- Workbench: `DEVS:Monitors/emu68-VideoCore` and
  `Libs:Picasso96/VideoCore.card` (VideoCore 1.5) are installed.
- Workbench `S:Startup-Sequence` sets
  `Picasso96/BoardType VideoCore.card` and loads the monitor.
- The ScreenMode preference selects a Picasso96/RTG screen.

**The FAT boot files still belong to physical PiStorm:**
`config.txt` selects `kernel/Emu68-pistorm*` via GPIO. There is no
standalone `kernel8.img` at the FAT root and the necessary `sdhc`
overlay is not enabled in that original config. Trying to boot the
unmodified image on an Amiga-less Pi3A+ is not supported.

Use the owner-verified standalone v0.10.22 boot files with a USB-enabled
kernel built from the owner-verified v0.10.25 source, preserving the RDB.
The original 116 GiB image should remain untouched; replace the FAT boot
files **on a flashed SD-card copy**, never the source image.

## Optional experimental HDMI ownership handoff

Enabled with `-DFRF_RTG_HANDOFF=ON` **and** `FRF_VIRTUAL_AGA=ON`.
Implemented in `arm/aga_hvs.c` and the pure policy
`arm/frf_rtg_handoff.h`.

The Pi3's VideoCore.card writes the GPU's DISPLIST1 to display a
Picasso96 screen. Existing AGA code always runs in standalone mode
and can overwrite this external display list while rendering native
video. PiStorm's old RTG yield path only activates for PROGRAM/RUN,
not for a persistent standalone machine.

This experiment checks for valid external HVS display lists and yields
the HDMI plane to them, while leaving the virtual AGA running.
When the native chipset transitions from inactive to active again,
the AGA framebuffer is allowed to reclaim HDMI, enabling games.
No program, P96 card binary, mountlist or RDB sector is modified.
The guard is OFF by default pending real hardware tests.

## Tests

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -I machine/arm    tests/test_rtg_handoff.c -o /tmp/test_rtg_handoff
/tmp/test_rtg_handoff
```

The test exercises P96 takeover, no immediate steal-back, native screen
return, P96 restoration after a game and exclusion of boot firmware lists.

This alone does not prove the VideoCore.card has successfully initialized.
That driver needs a working `devicetree.resource`, GPU mailbox access,
a valid `/emu68/vc4-mem` reservation and functioning HDMI outputs.
The legacy VideoCore.card source also contains unbounded GPU response
waits; debug them separately if HVS list switching is not observed.

## Critical regression checklist

1. Confirm the Bare Metal splash and native AGS boot first.
2. Boot the same RDB with Picasso96 and a Magellan RTG screen.
3. Use USB keyboard/mouse, open windows, verify mouse pointer and text.
4. Launch and exit native AGA/ECS WHDLoad games repeatedly, confirming
   HDMI switches back to RTG desktop without freezing.
5. Confirm SD RDB volumes and clock/timer interrupts still function.
6. If still blank: record exact last screen/UART log and test native
   Workbench screen mode separately; restore the verified kernel.

**Status: build and mocked handoff test pass. Pi3A+ RTG functionality
requires physical confirmation and may need additional mailbox work.**
