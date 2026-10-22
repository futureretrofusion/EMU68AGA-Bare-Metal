# EMU68AGA Pi 3 — R14 Dyonder USB controller port (experimental)

2026-10-22. R11 keyboard/mouse hardware confirmed, R12 MSC detection user confirmed.
R13 controller still NOT detected; R14 physical pad still UNVERIFIED.

## Why R13 missed the Dyonder
The prior USPi driver for the known Dyonder/ShanWan USB receiver explicitly
handled VID:PID 2563:0576 with a 191-byte HID report descriptor. Gamepad
INPUT reports have report ID 01 and length TEN bytes **including the ID**.
R13's default mapping only handled 11-byte TinyUSB standard gamepad reports
and had no direct 2563 decoder. Linux also observed the receiver alternating
between 2563:0575 and 2563:0576 (a device mode transition).

## Port of established USPi protocol into the independent TinyUSB stack
- Detect exact 2563:0576 and alternate 2563:0575 in the HID mount callback.
- Reject boot keyboard/mouse interfaces even on a composite receiver.
- Report ID 01, len 10: bytes 1/2=15 buttons, byte 3=directional hat
  0..7, neutral 8; bytes 4..7=0..255 analogue axes. Translate first X/Y,
  d-pad and any gamepad button to Amiga JOY1DAT/fire on joystick port 2.
- Report ID 05 is relative mouse, not joystick; ignore it (no stale
  joystick updates). Report 02 is output only, not mapped as input.
- 2563:0575 registers as an alternate-mode candidate; only a valid
  ID1/10-byte report makes it actively drive the joystick. A different
  report format remains unrecognised rather than being guessed.
- Restore USB request recovery from prior RC9: initial interrupt-IN
  submission may fail transiently because host channels are busy.
  CPU1 retries pad HID requests at 500ms intervals when the endpoint
  is not already busy. This does not reinitialize the Pi3 USB bus.
- Retain xinput/gip, standard HID, keyboard, mouse and read-only MSC from R13.

## Real HDMI diagnostic
Normal POST: TINYUSB PI3 / DYONDER R14.
Left secondary panel: R14 PAD / DYONDER SCAN.
For known Dyonder:
  VENDOR: 25630576 or 25630575
  CLASS/IF: 030000xx (HID interface index xx)
  PROTOCOL: 00000004=0576 known; 00000005=0575 alternate candidate
  DY RX/IGN: high16 accepted ID1/10-byte joystick reports; low16 ignored.
  PAD/MSC: high16 pad candidate count, low16 MSC count.

USB_ONLY_TRACE stays on diagnostic POST and doesn't boot Amiga; it shows
the live bus/MMIO panel AND the Dyonder report panel. Both use a 1280x720
or similarly wide HDMI mode if possible.

## Tests
- Real source HID mount/report/retry path linked into standalone tests,
  native x86 and AArch64 **big-endian QEMU**; first submit deliberately
  fails then recovers on the 500ms retry.
- Exact ID1 10-byte neutral/left-fire/up report, ID5 mouse isolation,
  unplug cleanup and 0575 mode candidate tested.
- Standard HID classification and Xbox 360/Xbox One GIP mapping regressions
  pass. MSC little-endian BOT SCSI command tests pass.
- Both normal and USB-only bare-metal Pi3 kernels crosscompiled.
- None of these tests simulate a physical Dyonder receiver, so
  **R14 gamepad hardware is not yet confirmed working**.

## Suggested test
1. Back up the known-working kernel8.img (keep R11/R12 safe).
2. On a test Pi3 FAT32 SD boot partition install NORMAL/kernel8.img.
3. Cold boot with Dyonder wireless receiver attached and controller on.
4. Check PAD count and VID=25630576/25630575; press D-pad and buttons.
5. If input doesn't work, use USB_ONLY_TRACE/kernel8.img and photograph
   VENDOR, CLASS/IF, PROTOCOL, DY RX/IGN, PAD/MSC on the live screen.
6. If keyboard or mouse regression, restore the proven R11 kernel.
7. Do not assume 0575 is the same gamepad report mode without ID1
   confirmation; only a real receiver report makes JOY1DAT move.

Source isolated in branch pi3-tinyusb-r14-dyonder-uspi-port.
No merge to main. MSC writes remain disabled.
