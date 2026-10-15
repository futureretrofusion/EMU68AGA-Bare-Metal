# EMU68AGA BARE METAL AMIGA

![EMU68AGA Bare Metal boot splash](assets/EMU68AGA_Bare_Metal_Splash.png)

**Future Retro Fusion** — standalone, bare-metal Amiga/AGA system for Raspberry Pi 3A+, integrating Emu68 and AGA-PiStorm virtual chipset work. No physical Amiga or PiStorm adapter required.

## What we are doing

**We are taking the ARM-based Amiga emulation already demonstrated by PiStorm and AGA-PiStorm and making it run without an Amiga motherboard.**

With AGA-PiStorm's virtual AGA active on a physical PiStorm, the Raspberry Pi runs the emulated 68k CPU and most of the Amiga chipset in software. Communication with the real Amiga is then relatively limited: some hardware coordination remains, and optional outputs can use physical Paula audio or the RGB port through the original chipset. It is **minimal, not necessarily zero**.

**EMU68AGA Bare Metal removes that remaining physical-Amiga dependency.** The Raspberry Pi runs Emu68 and the virtual AGA chipset, with virtual Chip RAM, registers, interrupts and hardware interfaces; HDMI provides video and audio, USB provides input, and SD storage supplies the drives. No Amiga motherboard, PiStorm adapter or original Amiga chips are needed.

Our contribution is the **standalone integration, replacement of Amiga-bus dependencies, hardware-interface emulation and compatibility work**. The 68k emulator and virtual AGA foundation come from **Emu68, AGA-PiStorm and their contributors**, with UAE/WinUAE heritage as credited below. This remains an experimental system, not a claim of complete Amiga compatibility.

**Current status: experimental alpha.** A local hardware test has now confirmed the **v0.10.25 USB-enabled kernel boots with the unmodified v0.10.22 boot support files**. This exact pairing matters: later repackaged boot folders did not work for the tester. The USB-enabled kernel is not part of the public download while the upstream GPL licence conflict remains unresolved. This is not a turnkey operating-system image.

## Start here

- [Installation and AGS 3.1 Classic SD-card guide](docs/INSTALL_AGS31_CLASSIC.md) — complete partition layout, boot files, Kickstart, AGS and troubleshooting.
- [Bare Metal splash and embedded artwork](docs/BARE_METAL_SPLASH.md) — exact source/build instructions.
- [v0.10.25 hardware-tested boot pairing](docs/BOOT_COMBINATION_v0.10.25.md) — exact boot-file provenance, SHA-256 and tests.
- [v0.10.25 ROM-free boot support ZIP](distribution/EMU68AGA_BARE_METAL_v0.10.25_BOOT_SUPPORT_ROM_FREE.zip) — exact v0.10.22 configuration; **does not contain a kernel or Kickstart ROM and is not bootable by itself**.
- [Older public v0.10.23 diagnostic ZIP](distribution/FRF_EMU68AGA_BARE_METAL_PUBLIC_BOOT_ALPHA_v0.10.23.zip) — USB is disabled; not the tested combination.
- [v0.10.25 preparation notes](RELEASE_NOTES_v0.10.25.md)
- [USB integration source](docs/USB_SOURCE.md) — FRF adapter only, not bundled USPi and not a redistributable combined binary.
- [USB redistribution status](USB-LICENSING.md)

A practical public alpha **must** have working keyboard/mouse input.
**`FRF_USB_HOST=ON` is now the default.** The local development kernel has
working FRF USB integration using USPi. A fresh public-source build fails
with an explicit missing-dependency message until the matching adapted
USPi sources are provided; setting OFF produces only a no-input diagnostic,
**not** a usable release. Public redistribution of the combined binary
remains blocked by upstream licensing. **A GPL-2.0-only + GPL-3.0-or-later static-link incompatibility must be resolved before publishing the working USB binary.** A clean-room compatible USB replacement or permission from the necessary upstream copyright holders is required.

## Features exercised in local development

- Emu68 68k JIT, virtual AGA graphics (Alice/Lisa/Paula), Copper, blitter and CIA emulation.
- HDMI video and Paula audio, 2 MiB Chip RAM, 512 KiB Ranger memory and Zorro III Fast RAM.
- 32 GB microSD layout with FAT32 boot and Emu68 `0x76` Amiga/AGS partitions and an Emu68 ROM-board/autoconfig path.
- USB keyboard/mouse in the local build, with experimental joypad-to-Amiga-port-2 mapping.
- Amiga soft-reset/autoconfig fixes in v0.10.21; WHDLoad game compatibility still varies by title/demo.

## Project credits and licensing

This is a hobby project, **but non-commercial use does not remove redistribution licence obligations**. FRF credits its own standalone integration to **Future Retro Fusion**, and preserves upstream credits and source-file licences.

Emu68: Michal Schulz and contributors; AGA-PiStorm: Astair86 and contributors; PiStorm: Claude Schwarz and contributors; UAE/WinUAE: their respective contributors. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSE-FRF.md](LICENSE-FRF.md), and source headers.

No Kickstart ROM, AmigaOS/AGS software, WHDLoad games or third-party disk images are distributed here.
