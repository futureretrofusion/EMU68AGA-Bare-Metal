# WHDLoad Save States — RC9 OSD and planned backend

Status: OSD interface compiled. Full save/load is NOT implemented.
No .ss files are created by RC9.

## Storage location

Save inside each WHDLoad game's own AmigaDOS drawer, alongside its
.slave executable, NOT in the Raspberry Pi FAT boot partition or a
central shared save-state folder.

Example:
  Games:WHDLoad/InternationalKaratePlus/IK+.slave
  Games:WHDLoad/InternationalKaratePlus/IK+_slot01.ss
  Games:WHDLoad/InternationalKaratePlus/IK+_slot02.ss
  Games:WHDLoad/InternationalKaratePlus/IK+_slot03.ss
  Games:WHDLoad/InternationalKaratePlus/IK+_slot04.ss
  Games:WHDLoad/InternationalKaratePlus/IK+_slot05.ss

The WHDLoad wrapper / agaboot companion must identify the *actual*
current game drawer and slave name and communicate this to the
standalone kernel. The ARM side must not guess the AmigaDOS path.
Use a temporary file, complete validation and atomic rename once
saving is implemented.

## RC9 OSD entries

- STATE SLOT: selects 1-5, currently works in the menu
- SAVE STATE: explicitly NO ENGINE; no file is written
- LOAD STATE: explicitly NO ENGINE; no state is restored
- HDMI VOLUME: existing HDMI output gain, MUTE / 100/150/200/300/400%
- DISPLAY DETAIL: existing HVS TV/Sharp presentation filter,
  labelled LORES FX / HIRES FX. This does not change the emulated
  game's actual bitplane resolution!
- SAVE FOR THIS GAME: existing AGA-Storm picture settings,
  separate from the snapshot save system

## E-UAE reference implementation

Future Retro Fusion e-uae.68k.0.8.30 GitHub repo:
  src/savestate.c
  src/include/savestate.h
  src/custom.c (custom-chip state)
  src/newcpu.c (CPU state)

The savestate.c header credits Toni Wilen (1999-2001) and Future Retro
Fusion. Preserve the original authorship and licences if any source
is incorporated. The E-UAE code serializes CPU/FPU/CIA/audio/custom
registers, RAM and disk state, but its own comments warn that
hard-disk, autoconfig and expansion boards are not fully saved.
Emu68's JIT CPU state and the AGA-Storm chipset have different internal
layouts, so the E-UAE routines cannot be used unchanged.

## Required implementation

1. Game-drawer and slave identity handshake via WHDLoad wrapper.
2. Safely pause Emu68, AGA workers, DMA, audio, timers and devices
   at a mutually consistent boundary.
3. Versioned portable sections for CPU state, Chip/Bogo/Fast/Z3 RAM,
   full AGA/Paula/CIA/Copper/blitter internals, disk and runtime context.
4. Checksums, kernel compatibility validation, restore-time JIT
   invalidation and timing/interrupt rebasing.
5. In-memory IK+ WHDLoad round-trip tests; only after these pass
   enable atomic on-disk .ss writes and multiple save slots.

RC9 deliberately does not claim that Save State and Load State work.
