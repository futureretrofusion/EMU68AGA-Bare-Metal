# EMU68AGA Pi 3 — R22 live USB-to-Amiga gamepad link diagnosis
Built on Studio Main, 22 October 2026. Experimental diagnostic release.

## Hardware evidence
R21 physical boot POST shows PAD=01 for VID:PID 7E05:0920 (HID
interface 0). This proves correct HID device recognition, but
R21 PAD RX/OK/JOY remain zero in the PREBOOT screenshot. The normal
POST framebuffer stops drawing when Amiga video starts, so a frozen
preboot zero cannot establish whether USB receives packets later.
R18 USB-only mode had received 24-byte input activity after a delay,
and the user observed reactions to buttons and analogue sticks.

## What R22 changes
R22 keeps R21's controller matching/mapping, TinyUSB DWC2, keyboard,
mouse and read-only mass-storage firmware intact. It writes a LIVE
read-only 84-byte versioned debug structure (magic PAD2) into UNUSED
offset 0x3400 of the already existing 64 KiB FRUM Zorro-II USB board.
The legacy usbmass.device mailbox (at offset 0x0000) is unchanged,
and legacy prior debug offset 0x3000 is untouched.

CPU1 updates the snapshot every 200ms. The record includes:
  tick_ms: continuously advancing CPU1 time, even after guest boot
  host_ticks: actual TinyUSB task-loop iterations
  PAD: detected controller interface count
  dRX: HID reports received from EXACT 7E05:0920/interface0
  len: length of the last receiver report, ideally 24 bytes
  raw0/raw1: first 8 actual received bytes, no guessed mapping
  HID: total received HID packets from any device
  claimRX: reports entered into R21 controller compatibility decoder
  OK: fully decoded compatibility reports
  reject: controller decoding rejects
  JOY1 + FIRE: ACTUAL values in virtual Amiga hardware state
  W: count of Amiga joystick-register writes from CPU1
  GJOYRD: count of 68k guest reads of JOY1DAT
  CIARD: count of 68k guest reads of CIAA button register
  msc_present, msc_blocks

Native AmigaOS 68000 Shell program AMIGA_DIAGNOSTIC/FRFPadLive:
- Finds existing FRUM Zorro-II expansion board manufacturer 0x6D73,
  product 0x69 using expansion.library/FindConfigDev.
- Reads read-only telemetry from board address + 0x3400.
- Does NOT poke JOY1DAT or the USB/mass-storage command mailbox.
- Displays two live lines at ~0.5s intervals for approximately 45s.
- Checks an odd/even sequence number for consistent snapshots.
- Works after AmigaOS has taken the screen; no frozen preboot POST.

### Physical test — preferred
1. Back up the last working R21 kernel8.img.
2. Copy NORMAL/kernel8.img to Pi3 FAT32 SD boot partition.
3. Cold boot with controller USB receiver connected. Start Workbench.
4. Copy AMIGA_DIAGNOSTIC/FRFPadLive to an Amiga-accessible volume.
5. From Workbench Shell, run FRFPadLive (or C:FRFPadLive if copied C:).
6. Press the controller D-pad left, move the analogue stick, then press A.
7. Photograph several consecutive output lines. Key comparisons:
    host increasing, dRX increasing but OK=0:
       HID packets reach CPU1 but are rejected/not decoded.
    host increasing, dRX=0 but HID increases:
       USB is active but receiver packets not being delivered;
       report routing or length/ID is different.
    OK increasing and JOY1 changing, GJOYRD increasing:
       USB->Amiga emulated register path is active; investigate
       game-specific joystick-port mapping/boot input handling.
    OK increasing and JOY1 changing but guest read count 0:
       guest isn't reading port 2 in that test; FRFJoyDiag reads it.
    host not increasing / tick unchanged:
       CPU1 USB worker is stalled after guest boot.
    No board:
       Zorro-II expansion board may not have autoconfigured. In that
       case use existing FRFJoyDiag to read guest JOY1DAT directly.

The USB_ONLY_TRACE/kernel8.img remains available for USB-only tests,
but it never starts the Amiga guest (guest JOY/CIA reads stay zero).

## Software verification (not hardware verification)
- Normal and USB_ONLY_TRACE Pi3 AArch64 BIG ENDIAN kernel compiled.
- FRFPadLive native AmigaOS LoadSeg executable compiled.
- New 84-byte shared ABI and mailbox bounds validated on x86 and
  ARM64 big-endian QEMU; old FRUM data layout unchanged.
- Existing R21 7E05:0920 early PAD claim/mount/active first HID
  packet -> JOY1DAT and CIA fire passed x86 and ARM big-endian.
- All 8 ARM big-endian DWC2 controller modes passed.
- Physical Pi3 R22 test has NOT yet been performed. It is a diagnostic
  to resolve the remaining live input path, not a claimed joystick fix.
