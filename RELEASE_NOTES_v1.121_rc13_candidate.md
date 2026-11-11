# FRF EMU68AGA Bare Metal v1.121 RC13 — DF1 diagnosis

Experimental development kernel for Pi3; physical DF1 functionality
is **not verified**. This is based on RC12, preserving DF0 ADF boot,
DF0/DF1 delayed disk swaps, independent rotation, index pulses, PAL
soft reset, RTG handoff and independent FRF TinyUSB/DWC2 USB host.

## Changes

- RC12 incorrectly marked DF2/DF3 as connected. RC13 advertises
  only DF0 and DF1 as installed physical virtual drives, regardless
  of ADF media presence. Other slots stay absent.
- Fixed misleading source comment about floppy ID polarity.
  Normal 3.5 inch DD drive-ID sampled from /RDY with motor off is
  DRT_AMIGA = 0x00000000. No drive is DRT_EMPTY = 0xFFFFFFFF.
  No polarity logic was changed; it was already correct in RC12.
- Added an exact 32-clock motor/SEL drive-ID host regression:
  DF0 = 00000000, DF1 = 00000000, DF2/DF3 = FFFFFFFF;
  also checks DF1 media-ready/empty transitions.
- INDEX pulses from the rotating virtual drives are now delivered to
  CIA-B only for the selected drive. Previously, unselected spinning
  DF0 could signal index while DF1 was being used.
- Avoid fallback disk data from DF0 if a stopped DF1 is explicitly
  selected. Preserve the original fallback only when no drive is
  selected during the Kickstart disk DMA sequence.
- New AmigaOS m68k executable FRFDFTEST checks actual Kickstart
  disk.resource cached IDs and trackdisk.device OpenDevice for
  units 0 to 3, drive types, tracks, media/change counters. Optional
  sector-0 READ checks Paula disk DMA independently of mounting.

## The unresolved hardware report

On RC11, inserting Lionheart disk 2 into DF1 did NOT show a mounted
DF1 volume in Workbench. This is a documented hardware failure;
RC12 and RC13 have not been physically tested. The source-side
drive identification tests cannot establish Workbench functionality.

A custom/non-DOS game disk may not mount a Workbench filesystem
even with working trackdisk hardware, so first verify a known-good
DOS-formatted ADF in DF1. Use FRFDFTEST to separate three issues:
A) ID discovery and driver OpenDevice,
B) inserted/empty/change-state recognition,
C) sector-0 read and filesystem mount.

For Lionheart, use disk 2 in DF0 if the game's loader prompts
specifically for DF0; inserting into DF1 cannot redirect a
single-drive loader.

## Verification

- Emu68 Pi3 AArch64 big-endian kernel compile.
- FRFDFTEST cross compiled with AmigaOS m68k GCC.
- Host tests for motor-off drive IDs, DF1 media state, disk swap,
  PAL/RTG reset and USB guest-reset shortcuts.
- Physical DF1 mount and Lionheart remains UNVERIFIED.
- Original RC8 published release remains unchanged.
- WHDLoad .ss save states remain NO ENGINE.
