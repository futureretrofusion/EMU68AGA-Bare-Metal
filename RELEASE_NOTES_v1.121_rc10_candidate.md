# FRF EMU68AGA Bare Metal v1.121 RC10 — floppy + guest soft reset

**Development candidate; hardware verification still required.**
Builds upon RC9 on the independent Pi 3 bare-metal FRF TinyUSB/DWC2
stack. RC8 remains the last published GitHub pre-release.

## Virtual floppy support

- OSD now has ADF SLOT (1–4), DF0 INSERT, DF0 EJECT, DF1 INSERT,
  and DF1 EJECT, with current disk present/empty feedback.
- The original AGA-Storm virtual floppy backend was already present:
  901120-byte DD ADFs and 1802240-byte HD ADFs. RC10 exposes existing
  functionality, it does NOT add a physical floppy interface.
- Insert from a selected slot into DF0 or DF1, read-only; ADF slots
  must contain images or the OSD displays NO ADF.
- A boot-time ADF, when present, is retained in slot 1 for reinsertion.
- New AmigaOS FRFADF CLI loader loads files from an AmigaDOS volume into
  ADF SLOT1–SLOT4, DF0 or DF1 via the FRF private $ADxx protocol.
- Dirty in-memory ADF media cannot be overwritten/ejected by the new
  OSD interface or FRF guest ADF commands, because writes are not
  automatically saved back to their source files.
- No OSD SD-card filesystem browser yet. Populate slots with FRFADF
  before starting a WHDLoad game.

## Amiga soft reset (NEW)

- **Ctrl + Left Win + Right Win** (Amiga Left/Right) = guest 68k restart.
- **Ctrl + Left Alt + F11** = alternative guest 68k restart.
- Select **AMIGA SOFT RESET** in the OSD twice to confirm guest reset.
- The keyboard publishes a cross-core request. The Emu68 JIT execution
  loop consumes it at a translated-block boundary, resets virtual CIAs
  and AGA, restores ROM overlay, reloads Kickstart initial SSP/PC, resets
  CPU exception/MMU control and invalidates translated-code cache.
- The Raspberry Pi, TinyUSB worker and mounted virtual disk images remain
  in place. This is not a host reboot.
- **Ctrl + Left Shift + F11** retains the separate host/Pi reboot shortcut.
- Mixed Ctrl+Alt+Shift+F11 prioritizes the guest reset to prevent
  accidentally triggering a Pi reboot.

## OSD and WHDLoad save states

- A scrolling OSD list prevents the longer RC10 menu from overflowing
  PAL-height screens. Existing scale, scanline, picture, volume and
  monitor controls remain.
- RC9 WHDLoad STATE SLOT 1–5 remains; SAVE STATE / LOAD STATE display
  NO ENGINE. **The .ss snapshot backend is not implemented.**
- Once implemented, .ss files belong alongside the corresponding
  WHDLoad game's .slave in its own game directory.

## Tested (without the Pi)

- Pi 3 big-endian AArch64 kernel compiled with FRF_DISTRIBUTION_BUILD=ON.
- HID boot-keyboard host regression: both guest reset combos,
  edge-triggering, distinction from Pi reboot and OSD F12 all passed.
- Existing RC9 slot and HDMI-volume unit tests passed.
- AmigaOS m68k FRFADF binary compiled; identified as loadseg executable.
- Compilation/link still excludes USPi and PiStormAGA USB additions.

## Outstanding real-hardware checks

- Both guest reset keyboard shortcuts reach the Kickstart reboot path.
- DF0/DF1 insertion/ejection while Workbench and games run, including
  multi-disk disk-change response and retained DF0 boot ADF.
- OSD scrolling, joystick and keyboard input across long menu.
- Audio/HDMI/USB survive repeated guest-only resets.
- FRFADF successfully uploads ADFs on the physical Pi 3.
- Native OSD F12 handling continues to need hardware verification.

## Credits

Emu68 by Michal Schulz and contributors; AGA-Storm / AGA-PiStorm by
Astair86 and contributors; TinyUSB by Ha Thach and contributors; UAE
and E-UAE by their original authors. FRF original RC10 OSD, guest
reset and FRFADF glue builds upon existing upstream mechanisms and
preserves their source-level licences. No commercial ROMs or ADFs are
included.
