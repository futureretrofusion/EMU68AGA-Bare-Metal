# FRF EMU68AGA Bare Metal v1.121 RC9 — OSD WHDLoad menu candidate

This is a **developer build**, not a finished save-state system.
RC8 is the previous GitHub pre-release; this RC9 work is on a separate
feature branch pending hardware tests and full snapshot development.

## OSD changes

- STATE SLOT: Select one of five per-game WHDLoad state slots.
- SAVE STATE / LOAD STATE: Display NO ENGINE (unavailable). Selecting
  shows SAVE/LOAD NOT READY. No .ss file is written or restored.
- HDMI VOLUME: Mute, 100%, 150%, 200%, 300%, 400% using the existing
  live HDMI gain setter. These levels follow upstream AGA-Storm sound.
- DISPLAY DETAIL: LORES FX uses the TV horizontal filter; HIRES FX
  uses Sharp nearest-neighbour filtering. These are display **effects**
  and do not modify the game's AGA low/hires bitplane mode.
- SAVE FOR THIS GAME: retains original per-game picture settings;
  it is not the new save-state action.
- OSD hotkeys unchanged: LCtrl+LAlt+F12, alternate LCtrl+LShift+F12;
  up/down, left/right, Enter and Escape. The native OSD hotkey is
  itself not yet hardware-verified in RC9.

## Storage design

Once a complete snapshot engine exists, every game will keep its
.ss files inside its own WHDLoad drawer beside its .slave file, for
example Games:WHDLoad/IK+/IK+_slot01.ss through IK+_slot05.ss.
The WHDLoad wrapper must report the real drawer and game identity;
the kernel must not guess the path. No files are created by RC9.

## Source and attribution

The E-UAE savestate.c and savestate.h in Future Retro Fusion's
E-UAE fork provide an architectural reference. Toni Wilen and
the UAE/E-UAE contributors are credited; no E-UAE savestate
implementation is claimed as working or copied in RC9.
AGA-Storm and Emu68 licences/credits remain unchanged.
PiStormAGA USB additions remain excluded; FRF TinyUSB/DWC2 retained.

## Verification

- AArch64 BE Pi 3 kernel compiled with FRF_DISTRIBUTION_BUILD=ON.
- Portable menu stepping regression passed.
- Compiled kernel contains new menu labels and version.
- No PiStormAGA USB files or USPi link objects used.
- NOT verified on physical Pi 3; full snapshots not implemented.
