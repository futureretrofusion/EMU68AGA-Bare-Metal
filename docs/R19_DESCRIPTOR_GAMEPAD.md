# EMU68AGA Pi 3 R19 — Descriptor-based USB gamepad to Amiga port 2

Built on Studio Main, 22 October 2026. Experimental hardware release.

## Purpose and evidence

The R18 USB-only hardware photograph proved 24-byte HID reports with
changing bytes arrive (source=1, address/interface=01/00). User confirmed
visible changes when manipulating all buttons and sticks. No valid
JOY1DAT/fire mapping was previously present for that 24-byte format.

Earlier generic TinyUSB gamepad code only decoded an assumed 11-byte
layout. R19 adds a real USB HID report-descriptor parser to locate actual
X and Y axes, hat/D-pad and button fields. It drives existing Amiga
joystick port 2 via aga_virtual_joy_set(1, joy, fire).

## Changes

- Parses HID short/long descriptor items, collections, local usages
  and usage ranges, global push/pop, sizes/counts and report IDs.
- Only claims HID application collections whose HID Usage Page is 01
  (Generic Desktop) and whose top-level Usage is Joystick (04) or
  Game Pad (05). Keyboard and unknown vendor-only reports are NOT
  blindly interpreted as joystick.
- Honors per-report-ID input offsets, bit positions, logical minimum,
  maximum, signed 8-bit and 16-bit axis representation and null hats.
- Supports complete HID payloads of different declared report lengths,
  including observed 24-byte-class USB packets. Rejects truncated
  input rather than updating the Amiga from partial reports.
- Maps left X/Y to Amiga joystick port 2 directions using 20-percent
  deadzone; maps 8-way hat and discrete D-pad usages (up/down/left/right).
  Any declared button in usage page 09 maps to primary Amiga FIRE.
- Preserves the original known Dyonder/ShanWan 2563:0575/0576
  10-byte ID1 receiver decoder; Xbox wired XInput/GIP and vendor
  passive capture are unchanged.
- Distinguishes descriptor-backed gamepad interfaces before normal
  USB HID boot-mouse routing, preventing 24-byte stick reports
  being injected as phantom mouse movements on composite devices.
- The existing USB keyboard, mouse, read-only MSC and Ctrl+LeftShift+F11
  reboot handling are retained without changing DWC2 polling code.

## POST telemetry

R19 HDMI USB packet panel footer displays:
- MAP FIELDS: number of parsed HID field entries for latest mapped pad.
- RX: reports received by descriptor-mapped pad.
- OK: full reports successfully decoded and written to JOY1DAT.
- JOY: last virtual Amiga JOY1DAT value (low 16).
- F: last virtual Amiga fire bit (01 pressed).

In NORMAL/kernel8.img, POST disappears when Amiga takes over the
framebuffer. This is intentional and does not mean the USB host stops.
In USB_ONLY_TRACE/kernel8.img the packet view remains live, but no
Amiga guest runs, so guest register read counters remain zero.

## Test recommended

1. Back up known-working R18 or R11 kernel.
2. Copy NORMAL/kernel8.img onto FAT32 Pi3 SD boot volume.
3. Cold-boot with gamepad receiver already connected; sync controller.
4. Confirm AmigaOS boots, keyboard/mouse and MSC still work.
5. Run AMIGA_DIAGNOSTIC/FRFJoyDiag from an Amiga Workbench Shell
   (binary included) and move the D-pad, left stick, press A/fire.
   JOY1DAT and FIRE should change. Also try an Amiga game set to port 2.
6. If input does not work, use USB_ONLY_TRACE/kernel8.img and photograph
   R19's MAP FIELDS, RX and OK values and the address/interface table:
   a. MAP FIELDS=0 => real HID descriptor lacks a parsed joystick/gamepad
      application or this is another device. Further descriptor capture
      needed; no inference from packet bytes alone.
   b. FIELDS>0, RX>0 but OK=0 => report ID/length/layout mismatch.
   c. OK grows and JOY changes => decoder writes virtual joystick;
      FRFJoyDiag checks Amiga guest register access.
7. Do not presume USB-only trace is an Amiga boot build. Normal is the
   one to use for Workbench and games.

## Software verification

- Pi3 AArch64 BIG ENDIAN NORMAL and USB_ONLY_TRACE kernels compiled/link.
- New parser tested x86 and AArch64 big-endian QEMU:
  24-byte HID report, unsigned axes, report ID, signed axes, 8-way hat,
  16 buttons, FIRE, keyboard descriptor rejected, truncated packet
  rejected.
- Real TinyUSB HID mount -> report received -> aga_virtual_joy_set()
  tested for neutral, left, button fire, truncated report and unplug
  on x86 and AArch64 BE using mocked external USB device.
- Previous ShanWan, XInput, HID, DWC2 host (8 modes x86/BE),
  vendor interrupt/bulk endpoint class driver tested.
- Physical Pi3 joystick gameplay NOT YET VERIFIED for R19.
- All actual USB report bytes remain captured for a follow-up if
  the physical controller's descriptor is vendor-specific.
