# EMU68AGA Pi3 — R23 controller-connected keyboard repeat isolation
22 October 2026. EXPERIMENTAL — physical keyboard-with-gamepad test pending.

## Reported problem
R22 physical boot had PAD=01, but joystick data to Amiga remains
unverified. User reports endless Amiga keyboard repeat ONLY when the
gamepad is connected. This is authoritative physical evidence;
it is NOT a generic Amiga keymap problem.

## R23 changes
1. Strict native TinyUSB HID keyboard firewall:
   - Only exactly 8-byte USB Boot Keyboard reports enter the
     HID-to-Amiga raw keyboard translator (previously accepted len>=8).
   - Never accept keyboard reports from known gamepad VID:PID
     7E05:0920 on any interface. A composite game controller which
     exposes a keyboard-compatible HID interface cannot synthesize
     repeated Amiga keys.
   - No global block on ordinary keyboard and mouse interfaces.
2. On actual keyboard HID-interface disconnection, emit the required
   up transitions for held keys. Prevents stuck key state when a
   controller attachment re-enumerates the hub.
3. Controller USB interrupt-IN fairness:
   - Only the exact R21 receiver interface runs paced at 10ms (100Hz),
     scheduled from CPU1 TinyUSB worker, not immediately resubmitted
     from its HID report callback. This reduces potential channel
     starvation of physical keyboard release packets.
   - Mouse and real keyboard remain on their original immediate
     receive/requeue paths.
   - Legacy ShanWan 2563, descriptor-driven HID and Xbox handling keep
     their original scheduling/recovery semantics.
4. R23 extends the existing read-only Zorro-II FRUM diagnostic ABI at
   +0x3400: appends keyboard report counts, gamepad packets blocked from
   keyboard translation, malformed keyboard lengths, current held-key
   count, total key transitions, CIA keyboard queue depth/drops and
   serial handshake timeout count. Prior R22 field prefix preserved.
   Structure version is 2.
5. R23 native AmigaOS Shell monitor FRFPadLive includes a third
   "R23 KBD" line with the live counters. It does not alter key state.

## Test on physical Pi3
- SAVE your current R21/R22 kernel8.img before testing.
- Install NORMAL/kernel8.img on FAT32 boot partition, boot AmigaOS
  with keyboard + controller receiver attached.
- Type a few letters, release keys, then move/press controller buttons.
  Check whether the keyboard immediately stops on release and whether
  gamepad alone can cause keyboard characters.
- No guest input success can be claimed from POST PAD=01 alone.
- If repeat persists, transfer AMIGA_DIAGNOSTIC/FRFPadLive_R23
  onto an Amiga accessible volume and run from a Workbench Shell.
  Observe "R23 KBD" counters:
    boot      = accepted ordinary boot-keyboard packets
    blockedPad= controller packets blocked from keyboard emulation
    badLen    = malformed non-8-byte boot keyboard reports
    keys      = Amiga raw key transitions emitted from keyboard reports
    held      = host keyboard keys currently held
    queue     = CIA keyboard queue depth
    drops     = lost/overflowed queued raw transitions
    ackTO     = CIA serial acknowledgement timeout count
  Also observe HID dRX/OK/JOY to investigate controller input.
  If held>0 after physical release, verify keyboard interrupt polling
  / missing key-up; if held=0 but repeats, investigate guest CIA
  serial handshake/host repeat state.
- USB_ONLY_TRACE/kernel8.img keeps diagnostics visible but NEVER boots
  the Amiga. NORMAL is the appropriate version for keyboard testing.

## Verified on Studio Main
- Crosscompiled normal and USB-only big-endian AArch64 Pi3 kernels.
- Native AmigaOS 68000 FRFPadLive_R23 compiled.
- x86 and ARM64-BE host tests prove:
  a 24B receiver report is not interpreted as keyboard;
  8B receiver Boot Keyboard spoof is rejected;
  standard 8B keyboard press/release emits exactly one down/up;
  1000 repeated held-key reports emit NO duplicates;
  explicit all-up releases held key.
- R21 actual pad mount and 10ms poll timing, legacy controllers and
  packet decoding regressions PASS x86 and ARM-BE.
- 8 DWC2 host-model scenarios PASS each x86 and ARM-BE.
- Updated mailbox struct ABI and bounds PASS x86 and ARM-BE.

**NOT PHYSICALLY VERIFIED**: A software-test pass is not evidence
that the actual gamepad no longer delays USB keyboard releases.
R23 is a conservative fix plus measurable live counters; additional
DWC2 scheduling work may be necessary if repeat persists.
