# EMU68AGA BCM AMIGA

**Future Retro Fusion** — standalone, bare-metal Amiga/AGA system for Raspberry Pi 3A+, integrating Emu68 and AGA-PiStorm virtual chipset work. No physical Amiga or PiStorm adapter required.

**Current status: experimental alpha.** Our local v0.10.21 configuration has booted AGS Classic and run WHDLoad games, including return to AGS. The experimental v0.10.23 kernel needs hardware confirmation. It is not a turnkey operating-system image.

## Start here

- [Installation and AGS 3.1 Classic SD-card guide](docs/INSTALL_AGS31_CLASSIC.md) — complete partition layout, boot files, Kickstart, AGS and troubleshooting.
- [Public ROM-free boot diagnostic ZIP](distribution/FRF_EMU68AGA_BCM_PUBLIC_BOOT_ALPHA_v0.10.23.zip) — **USB input is disabled in this specific public package**. Do not mistake it for the fully usable local test build.
- [v0.10.23 build notes](RELEASE_NOTES_v0.10.23.md)
- [USB redistribution status](USB-LICENSING.md)

A practical public alpha must include working keyboard/mouse input. The local development kernel has USB functionality, but the present public boot diagnostic does not. We are working to distribute the complete USB-enabled configuration with appropriate upstream notices and source compliance.

## Features exercised in local development

- Emu68 68k JIT, virtual AGA graphics (Alice/Lisa/Paula), Copper, blitter and CIA emulation.
- HDMI video and Paula audio, 2 MiB Chip RAM, 512 KiB Ranger memory and Zorro III Fast RAM.
- 32 GB microSD layout with FAT32 boot and Emu68 `0x76` Amiga/AGS partitions and an Emu68 ROM-board/autoconfig path.
- USB keyboard/mouse in the local build, with experimental joypad-to-Amiga-port-2 mapping.
- Amiga soft-reset/autoconfig fixes in v0.10.21; WHDLoad game compatibility still varies by title/demo.
- Built-in EMU68AGA BCM splash artwork, with Future Retro Fusion branding.

## Project credits and licensing

This is a hobby project, **but non-commercial use does not remove redistribution licence obligations**. FRF credits its own standalone integration to **Future Retro Fusion**, and preserves upstream credits and source-file licences.

Emu68: Michal Schulz and contributors; AGA-PiStorm: Astair86 and contributors; PiStorm: Claude Schwarz and contributors; UAE/WinUAE: their respective contributors. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSE-FRF.md](LICENSE-FRF.md), and source headers.

No Kickstart ROM, AmigaOS/AGS software, WHDLoad games or third-party disk images are distributed here.
