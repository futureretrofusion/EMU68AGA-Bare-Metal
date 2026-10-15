# AmigaDOS volume/mount stability for standalone EMU68AGA

**Status: private AmigaOS startup patch and DOSList diagnostic cross-built;
not yet tested after a physical Pi boot.** The main firmware is unchanged.

## Confirmed findings from the owner's read-only AGS Workbench volume

The inspected disk has **Workbench:, Work:, WHDLoad:, Games:, Media:**
Amiga filesystem volumes visible through the Linux read-only Amifuse mounts.

The existing `S:Startup-Sequence` does three risky things on bare metal:

- If `WHDLoad:` is not in `Assign VOLS`, calls a classic Amiga
  `scsi.device` recovery and hardware menu, potentially issuing a reboot.
- Calls `C:Mount DEVS:DOSDrivers/~(#?.info)`, mounting all DOSDrivers,
  including legacy `CF0` (CompactFlash), `MS0` (floppy), and
  `emu68` (optional FAT transfer volume), regardless of attached hardware.
- Unconditionally mounts the custom `Workbench:System/ami-share`.

These are credible sources of **boot-time** hangs or hangs following a
reboot into Workbench/AGS. The inspection does **not** prove they cause
WHDLoad freezes during a game. The `WHDLoad-Wrapper` scripts inspected
contain no DOS mount calls during normal gameplay.

## What we change

`tools/prepare_dos_mount_stability.py` builds a reversible patch from
the owner's existing Workbench `S:Startup-Sequence` and `S:User-Startup`.
The script reads its source without modifying the live Amiga filesystem
and refuses to patch unknown or already-modified startup layouts.

1. Remove obsolete `scsi.device`/hardware-picker reboot fallback.
2. Do not auto-mount missing CompactFlash, floppy and optional FAT volumes.
   Keep only `PIPE` and `AUDIO` handlers after verifying their files exist.
3. Remove unconditional `ami-share` mount attempt; manual use remains possible.
4. Start AGS from `S:User-Startup` only when `WHDLoad:AGS2` exists.
   Otherwise start Workbench instead of rebooting.
5. Preserve the original scripts byte-for-byte for rollback.

**Important:** Do not suppress or fake real RDB/AmigaDOS volumes. The ROM
disk driver must still register actual partitions, volume names, BootNodes
and filesystem handlers; any genuinely missing volume needs a separate
device/RDB investigation.

## Diagnose before and after WHDLoad

`tools/dosmountaudit.c` is a read-only 68k AmigaOS utility. It takes a
bounded, nonblocking DOSList snapshot and releases the DOSList lock
**before** printing or accessing DOS files.

Compile with an AmigaOS GCC cross-compiler:

```sh
m68k-amigaos-gcc -m68020 -Os -Wall -Wextra -Werror -noixemul \
   -o DosMountAudit tools/dosmountaudit.c
```

Copy the executable to `C:DosMountAudit`. In a Shell on the Amiga,
run `DosMountAudit >RAM:DOS_BEFORE.txt`, launch/exit a game, then
run `DosMountAudit >RAM:DOS_AFTER.txt`. Compare volume names and
`task=yes/no` before and after. A handler task field is only an
indicator, **not** proof the filesystem is healthy.

## Run patch preparation

```sh
python3 tools/prepare_dos_mount_stability.py \
  /path/to/read-only/Workbench-volume \
  /path/to/new/output-folder
```

The result contains two replacement scripts under `S/`, byte-for-byte
originals under `originals/`, and a unified `DIFF.txt`. Install them
only after backing up your actual AmigaOS startup files; reboot and test.

Do not modify a running Amiga system's RDB, FAT boot files, Kickstart or
kernel for this diagnostic. The owner's production disk remains untouched.

## What would establish a real fix

A physical test should show:
- Workbench starts without hardware-selection or repeated mount stalls.
- AGS starts with `WHDLoad:` present.
- Games return to AGS/Workbench repeatedly.
- RDB volumes, USB HID and optional manual FAT transfer work.
- No new missing-volume or locked-DOSList reports in the diagnostic.

If a game still stalls, investigate `WHDLoad-Cleanup`, filesystem-handler
readiness and Emu68 reset state separately, rather than claiming that
startup mount changes fixed the running emulator.
