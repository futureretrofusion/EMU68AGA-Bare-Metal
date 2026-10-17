# AGA-PiStorm v1.117 integration

Status: Pi3 cross-compiled, physical test pending.
Source: AGA-Pistorm_universal_v1.117.7z (17 October 2026).

## Included in this branch

- Atomically publish vpos, hpos, LOF, LOL as one packed word.
- Update at reset, VPOSW/VHPOSW and each colour clock.
- Read VPOSR/VHPOSR from one consistent snapshot.
- Clamp the beam lead against that same snapshot for nonlegacy modes.
- Preserve our standalone Pi3 video, USB, storage and framebuffer code.

## Remaining upstream changes

The 1.117 archive also changes audio, sprites, playfield, blitter,
VideoCore, installer and Amiga tools compared with earlier packages.
These have NOT yet been ported; replacing entire directories would
discard essential EMU68AGA standalone changes.

## Tester instructions

Kernel at Studio Main Downloads/EMU68AGA_AGA117_BEAM_MERGE_TEST/BOOT_TEST/kernel8.img.
Back up existing Pi3 kernel8.img. Copy only this one file to FAT boot.
Preserve working config.txt, floppy.txt, Kickstart, overlays and partitions.
No physical hardware result has yet been confirmed.
Bug reports: futureretrofusion1984@gmail.com.
