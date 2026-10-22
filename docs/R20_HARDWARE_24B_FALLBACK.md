# EMU68AGA R20 — Hardware-observed 24B HID controller compatibility
22 October 2026. EXPERIMENTAL — physical joystick operation is not verified.

## Observed failure
R18 USB-only screenshot showed HID source 1, USB address 1/interface 0,
24-byte packets, multiple changing fields that the user directly
observed reacting to all buttons and sticks. The device table showed
VID:PID 220D:330C for address 1. Neutral bytes in R18:
first three zero, bytes 3..6 approx 80 80 80 80 hex, byte 9 FF.
R19 PAD remains 00 on the physical Pi despite software tests with a
synthetic descriptor declaring Generic Desktop Game Pad usage. R19
therefore fails to claim a device whose descriptor doesn't present
that usage. This is distinct from early screenshot USB arrival delay.

## R20 compatibility
- Changes do NOT alter the DWC2/hub/keyboard/mouse/MSC transport.
- Retains descriptor-driven standard HID and old 2563 ShanWan mapping.
- Introduces a restricted fallback only for USB VID=220D, PID=330C,
  HID interface 0, packet length exactly 24 bytes.
- Lazily attaches as a candidate only when a *real, complete neutral
  packet* contains the hardware-observed signature:
  bytes 0,1,2,7,8=00, bytes3..6 within 60..A0, byte9=FF.
  It does not need a synthetic joystick collection in the HID
  descriptor, and can't cause a generic unrelated mouse to become a pad.
- After attach, left X/Y analog bytes 3/4 are mapped to Amiga port2,
  20% dead-zone. Byte9 interprets FF as neutral hat, 0..7 as compass.
  Fire uses observed but unverified low-order button candidates
  bytes 0,1,2,7,8. This layout is inferred, **not manufacturer
  verified**: actual button/hat mapping may need refinement.
- R20 HDMI reports PAD RX, OK, JOY, F in bottom-left of live packet
  screen. R20 late-attached candidate included in PAD count.
- Source controller association is exact VID/PID and interface:
  no assumption that the separate 2404:0078 vendor-class interface
  belongs to the Dyonder. The existing read-only vendor probe stays.
- Ctrl+LeftShift+F11, existing keyboard/mouse, read-only MSC retained.

## Test
The NORMAL/kernel8.img boots AmigaOS. Place it on the FAT32 Pi3 boot
partition after backing up your known-working kernel.
With controller receiver inserted and synced, wait for a neutral frame,
then move left stick, D-pad and press fire. Run Amiga FRFJoyDiag from
the ZIP (Workbench Shell), watch JOY1DAT and FIRE.
The USB_ONLY_TRACE/kernel8.img intentionally does NOT boot AmigaOS.
Use it if normal still doesn't respond. Photograph R20 PAD RX / OK /
JOY / F together with the EACH USB DEVICE table:
- PAD 00, RX 0: exact 220D:330C HID matching neutral report not
  observed; verify correct device identity/size and descriptor.
- PAD >00 / RX grows / JOY changes: hardware report mapped to Amiga.
- PAD >00 / RX grows / JOY never changes while moving stick: the
  provisional byte layout is wrong; use the full 64-byte XOR screen
  to learn true offsets and refine code.
- USB-only mode keeps guest reads at zero because Amiga is not running.

## Verification
R20 true host HID receive/lazy attach/virtual Amiga port2 writes tested
using a 24-byte realistic synthetic 220D:330C report on x86 and
AArch64 BIG ENDIAN QEMU. Late PAD=1, left/up/fire, unplug pass.
R19 descriptor, ShanWan, XInput, DWC2 model x86/BE all pass.
Both Pi3 normal and USB-only kernels compile/link and boot-image
extraction is verified. **Pi3 physical R20 test still pending.**
