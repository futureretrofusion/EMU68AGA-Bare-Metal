# FRF EMU68AGA Bare Metal v1.121 RC8 — experimental Pi 3 release

**Release status: pre-release / hardware test candidate (9 November 2026).**

This version merges AGA-Storm v1.121 onto our v1.120-derived FRF bare-metal
Pi 3 fork. It is not an official AGA-Storm or Emu68 release.

## Included

- AGA-Storm v1.121 chipset / HDMI video and OSD source changes,
  selected Emu68 cache/CMA integration, and the FRF independent
  TinyUSB/DWC2 backend on CPU1.
- FRF mouse, keyboard, gamepad and mass-storage paths retained, without
  compiling PiStormAGA's aga_usb.c or aga_pad.c.
- The new OSD keyboard gate follows **active native AGA video** rather
  than assuming a PiStorm-style sandbox; Ctrl+Left Alt+F12 and
  Ctrl+Left Shift+F12 are mapped at HID level.
- Temporary OSD automatic-open diagnostic and key counter overlay removed.
- Boot POST and footer show FRF 1.121 / RC8.
- Updated build configuration rejects USPi-backed distribution builds.

## Tested and untested

- **Confirmed by hardware user test in RC6/RC7:** OSD graphics rendered
  automatically over an AGA game; USB keyboard activity appeared on
  the diagnostic counters; reset shortcut continued to work.
- **RC8 not yet hardware-verified:** the OSD shortcut, navigation,
  close/reopen and game compatibility. Treat these as proposed fixes,
  not confirmed features.
- **Build verified:** AArch64 big-endian Pi 3 kernel links, and HID hotkey
  host regression passes.
- No Kickstart, AmigaOS, commercial games or disk images are included.

## Hardware testing

Back up the currently booting kernel before copying the release's
`kernel8.img` to your FAT boot partition. Match the filename to your
`config.txt` `kernel=` setting. Do **not** replace your Amiga storage or
other boot support files just to test RC8. Restart into a **native AGA
game**; the menu should remain hidden until the keyboard shortcut.

Keys: **Left Ctrl + Left Alt + F12** toggles menu; alternatively
**Left Ctrl + Left Shift + F12**. Up/Down selects; Left/Right changes
settings; Enter selects; Escape or F12 closes. The host restart
shortcut remains Ctrl + Left Shift + F11.

On failure, restore the previous `kernel8.img`.

## Included download assets

- `kernel8.img` — standalone experimental Pi 3 kernel; not a
  bootable disk image or full operating system.
- `FRF_EMU68AGA_v1.121_RC8_SOURCE.tar.gz` — matching source,
  licence notices and build instructions from this Git revision.
- `SHA256SUMS.txt` — checksums of downloadable artifacts.

## Attribution and licences

AGA-Storm by Astair86 and contributors; Emu68 by Michal Schulz and
contributors; the UAE / WinUAE / E-UAE contributors, PiStorm/PiStorm600
authors and upstream testers, plus FRF contributors, are credited in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Original copyright and
licence notices are preserved. See [USB-LICENSING.md](USB-LICENSING.md)
for the distinction between historical USPi builds and this TinyUSB build.
