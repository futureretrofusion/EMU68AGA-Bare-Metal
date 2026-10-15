# FRF Emu68 Standalone AGA

Experimental bare-metal Amiga/A1200-class system for Raspberry Pi 3, built from **Emu68** and **AGA-PiStorm** virtual AGA work.

FRF turns that stack into a standalone machine instead of requiring a physical Amiga/PiStorm bus.

## Compiled public boot alpha

Download the [v0.10.23 EMU68AGA BCM boot-test archive](distribution/FRF_EMU68AGA_BCM_PUBLIC_BOOT_ALPHA_v0.10.23.zip).

Extract the archive, copy the contents of `boot/` to the FAT32 Raspberry Pi boot partition, and add your **own legally obtained Kickstart 3.1 ROM** as `boot/kick31.rom` (the actual filename expected by `floppy.txt`). An AmigaOS/AGS hard-disk image or Emu68-compatible `0x76` partition is also needed and **not** included.

**Important alpha limitations:** This public build uses `FRF_USB_HOST=OFF` pending USB component licensing review, so USB keyboard, mouse and gamepad are not available. The build is compiled but has not been validated on a physical Pi 3A+. This is a boot evaluation package, not a complete plug-and-play system. Keep a backup of any working SD card.

## Current development state

Hardware-tested work includes:

- Emu68 68k JIT on Raspberry Pi 3
- virtual AGA/Alice/Lisa/Paula chipset
- 2 MiB Chip RAM
- 512 KiB slow/Ranger RAM support at `$C00000-$C7FFFF`
- 128 MiB Zorro III Fast RAM
- HDMI AGA video
- HDMI Paula audio
- FAT32 boot partition plus Emu68 `0x76` Amiga hard-disk partition
- restored Emu68 virtual Zorro autoconfig/ROM-board path
- `brcm-sdhc.device` / `devicetree.resource` SD boot support
- virtual CIA and floppy/ADF work
- optional diagnostics HUD, off by default

The local development branch also contains experimental USB HID work. That USB implementation is intentionally not distributed in this public tree yet; see `USB-LICENSING.md`.

## SD-card layout

Partition 1 is FAT32 for Raspberry Pi firmware, the FRF/Emu68 kernel, config files and a user-supplied Kickstart ROM. Partition 2 is MBR type `0x76` and holds Emu68/AmigaOS/CaffeineOS hard-disk content.

No Kickstart ROMs, ADFs, HDFs, CaffeineOS images, game files, or other copyrighted media are distributed here.

## Credits and project lineage

FRF is a derivative integration project. It would not exist without:

- **Emu68**, by Michal Schulz and contributors — https://github.com/michalsc/Emu68
- **AGA-PiStorm**, by Astair86 and contributors — https://astair86.itch.io/aga-pistorm-10-experimental-prototype
- **PiStorm**, originally by Claude Schwarz (`captain-amygdala`) and contributors
- **UAE / WinUAE** chipset work and contributors, including Bernd Schmidt, Alessandro Bissacco, Toni Wilen, and others credited by the donor source

FRF-specific bugs should not be reported upstream unless they reproduce on the unmodified upstream projects.

See `THIRD_PARTY_NOTICES.md` and the license files retained throughout the source tree.

## Licensing

This is a **multi-license derivative source tree**. FRF does not claim or attempt to relicense third-party work.

- Emu68 files retain **MPL-2.0**.
- AGA-PiStorm / UAE-derived portions retain their existing upstream license terms and notices.
- FRF changes made inside third-party files remain subject to the license applicable to those files.
- Only files that are original FRF work and explicitly identified as such are covered by `LICENSE-FRF.md`.

No upstream copyright or license notice is intentionally removed.

## Status

Experimental. Physical Raspberry Pi 3 behavior is considered authoritative.
