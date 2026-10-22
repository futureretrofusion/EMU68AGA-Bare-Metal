# EMU68AGA Pi 3 R15 — Per-device USB trace / composite Dyonder HID
22 October 2026. Experimental, not a verified working gamepad release.

## Hardware evidence to date
R11 mouse and keyboard work on physical Pi 3.
R12 onwards detect USB mass storage on physical Pi 3.
R14 POST photograph shows MSC=01, PAD=00, 1 keyboard, 2 mouse
interfaces. Its LAST-VENDOR register displayed 2404:0078 with FF/00/FF/00
interface. This is ONE last-scanned interface, not a full device list.
There is no physical evidence yet that the Dyonder itself enumerates.
Do not treat HID FOUND as a successful gamepad claim.

## R15 fixes
- Builds on unchanged R11/R12 working DWC2, hub, mouse, keyboard and MSC.
- Reads TinyUSB real HID report-descriptor top-level Generic Desktop usage.
  A Dyonder 2563:0575/0576 HID interface with protocol BOOT MOUSE can
  be claimed as a gamepad *only when it also advertises a gamepad or
  joystick collection*. Ordinary keyboard and mouse HID interfaces are
  never claimed as gamepads.
- On a claimed Dyonder HID composite interface, interrupt report ID01,
  exactly 10 bytes, is decoded into Amiga port 2. Report ID05 routes
  to relative mouse handling when the HID protocol is BOOT MOUSE;
  it is not erroneously interpreted as generic 3-byte mouse data or
  joystick movement. Other packets are ignored.
- Retains tested 500ms deferred pad interrupt-IN retry and R14 0575
  candidate path.
- Adds R15 EACH USB DEVICE / ADDRESS POST panel to the RIGHT, underneath
  the standard system POST. Contains USB address Dxx, its actual
  VID:PID, the most recently observed interface CLS/SUB/PROTO/number,
  and flags. Flags hex values: 01 device mount callback,
  02 HID, 04 mass storage, 08 vendor-class interface.
  Several flags may be combined, for example 05=mounted MSC.
  Displays up to 7 connected USB addresses among slots 1..15.
  On removal the entry is cleared.
- Retains prior vendor panel on LEFT now labeled LAST IF (HISTORY) to
  distinguish it from the per-address table.

## Testing
- Source directly connected to both AArch64 BIG-ENDIAN firmware builds.
- Normal Pi3 kernel with Amiga guest boots as usual.
- USB_ONLY_TRACE kernel deliberately does NOT start Amiga; live
  DWC2 register trace + last-interface panel + per-address list.
- x86 and AArch64-BE stand-alone PAD unit test passed:
  ordinary boot-mouse rejected, Dyonder boot mouse with HID gamepad
  collection accepted, 10-byte ID01 joystick, ID05 isolated,
  initial failed HID submission recovered, 0575 alt candidate,
  ordinary HID gamepad still recognised.
- DWC2 software model: 8 cases pass in x86 and in ARM-BE QEMU.
- Physical Dyonder controller has NOT been seen or verified with R15.

## HOW TO TEST THE SPECIFIC FAILURE
1. Preserve known working R11/R12 kernel8.img.
2. Copy NORMAL/kernel8.img (uncompressed) to boot FAT32 Pi3 SD.
3. Cold boot with the Dyonder receiver inserted, controller turned on.
4. Inspect R15 EACH USB DEVICE / ADDRESS panel below normal POST.
   Identify an entry such as D04 ID 25630576 or 25630575.
   Do not confuse another device with LAST VID 24040078.
5. If PAD remains 00, use USB_ONLY_TRACE/kernel8.img and photograph
   the right-hand per-address table and left-hand vendor/Dyonder panel.
   It remains on POST indefinitely by design, so connect/disconnect
   receiver to see whether an address is added or removed.
6. If VID:PID is NOT 2563:0575/0576, share the exact address/ID/IF
   from the table; it needs a different device protocol, not more
   blind changes to the ShanWan decoder.
7. If the VID:PID IS 2563 but PAD=0, share the same panel. That would
   distinguish unclaimed HID interface from unusual descriptor format.
8. If PAD>0 but no movement, observe DY RX/IGN as buttons are pressed.

Do not merge this branch into main until hardware gamepad success.
MSC writes remain disabled. No non-redistributable ROMs packaged.
