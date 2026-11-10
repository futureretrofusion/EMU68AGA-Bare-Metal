# FRF EMU68AGA Bare Metal v1.121 RC11 — ADF disk-change and PAL reset fixes

Status: experimental Pi 3 developer kernel. Build and host tests pass,
physical hardware verification required. Based on RC10; FRF TinyUSB/DWC2
remains the USB host. No PiStormAGA USB or USPi code is linked.

## DF0/DF1 change detection

- Prior direct ADF-to-ADF replacement was instantaneous. Workbench may
  continue seeing the original volume because no empty-drive interval
  was visible to trackdisk.
- RC11 stages an observable ~0.55 second empty-drive interval on
  replacement, then installs the new DD/HD ADF from a preallocated
  copied buffer. During that interval it reports no disk.
- Virtual floppy INDEX edges now reach CIA-B FLAG via a lock-free atomic
  handoff from the AGA scanline worker to the guest CIA service.
- Both DF0 and DF1 now rotate independently. Paula DMA reads the
  selected drive; with no drive selected, the old DF0-first fallback is
  retained for compatibility with Kickstart.
- Dirty in-memory ADFs still cannot be overwritten or ejected through
  the FRF helpers because direct persistence has not been implemented.
- The OSD retains its four ADF slots, DF0/DF1 insert/eject and the
  Amiga-side FRFADF command. Boot-time ADF stays in slot 1.

## Amiga soft reset and PAL

- On the guest-only reset keyboard chords OR a virtual-chipset RESET
  instruction, old Picasso96 RTG display-list ownership is revoked.
- The HVS frame worker returns to native PAL presentation, including
  an opaque PAL-shaped output while no bitplanes are drawn yet.
- After the reset, RTG is allowed to reacquire HDMI only after native
  display has become active and subsequently yielded to a new valid
  external P96 HVS list.
- This is an AGA PAL *presentation* reset, not an automatic reprogramming
  of the HDMI monitor's hardware refresh-rate/modeline.
- RC11 build enables the experimental FRF_RTG_HANDOFF=ON option to test
  correct RTG/AGA ownership transitions. Other installations may have
  that option OFF and will not exercise the same state machine.

## Lionheart ADF

- Lionheart Disk 1 is a possible custom-boot workload whose timing
  may need CIA-B INDEX. Adding the INDEX events is a plausible fix.
- No copy of the exact user-tested lionheart1.adf was available in the
  accessible project files. Accordingly **Lionheart boot is unverified**;
  do not claim a successful game boot from this developer build.
- Need hardware result and ADF header/hash to determine if any further
  MFM, loader, timing, or other chipset corrections are needed.

## Tests

- AArch64 big-endian Pi3 kernel built with FRF_DISTRIBUTION_BUILD=ON
  and FRF_RTG_HANDOFF=ON.
- Host floppy test: empty media interval, delayed insert, disk-change
  callbacks, independently spinning DF0/DF1, dirty-media refusal.
- Host RTG reset test: old P96 denied, native PAL first, later new RTG
  activation allowed.
- RC10 HID reset-key tests remain unchanged and pass.
- Physical Pi3 Workbench disk-change, Lionheart boot and RTG/AGA reset
  still need tests.

## Attribution

AGA-Storm / AGA-PiStorm by Astair86 and collaborators; Emu68 by
Michal Schulz and collaborators; TinyUSB by Ha Thach and collaborators;
UAE/WinUAE/E-UAE contributors acknowledged in the repository.
