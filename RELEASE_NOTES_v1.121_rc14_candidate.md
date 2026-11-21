# FRF EMU68AGA Bare Metal v1.121 RC14 — Native USB scroll wheel

**Experimental Pi 3 test build**, based on the RC13 developer branch.
Physical CaffeineOS wheel functionality has NOT yet been verified.
The earlier DF1 fix and the AGA PAL/RTG reset work are retained.

## Wheel path

FRF TinyUSB USB HID mouse on ARM core 1 -> decode signed wheel delta ->
inject Amiga NewMouse raw key events into the EXISTING virtual
CIA-A keyboard queue -> AmigaOS keyboard.device/input.device ->
NewMouse-compatible application, FreeWheel, MUIWheel or AmigaOS 3.2
native support.

Each positive HID vertical notch emits raw 0x7A down/0xFA up
(NewMouse WHEEL_UP); negative HID vertical notch emits raw 0x7B
down/0xFB up (NewMouse WHEEL_DOWN). Clamps anomalous counts to eight
notches per USB report so scrolling does not dominate the keyboard
queue. No guest-side Poseidon USB stack or new HID device is required.

## Supported native USB formats

- Standard 4-byte mouse reports: [buttons, dx, dy, wheel].
- 3-byte boot-mouse reports continue to move/click but contain NO
  wheel information. They intentionally emit no wheel events.
- TinyUSB defaults boot mice to BOOT protocol, which suppresses wheel
  bytes. RC14 detects Generic Desktop Wheel usage in the mouse HID
  report descriptor and requests REPORT protocol **only for that
  mouse**. On unsupported devices it falls back to boot mode; the
  keyboard keeps its unchanged 8-byte boot report protocol.
- TinyUSB HID mouse reports with nonzero report IDs: a known mouse
  report ID is parsed from the mounted USB descriptor and the
  wheel is extracted at the fifth byte.
- Dyonder HID ID5 relative mouse report may contain a fifth wheel
  byte and uses that when present.
- Alternative FRF native DWC2 PIO mouse backend has the same
  four-byte wheel decoder; native PIO also now preserves middle-click.
  The release kernel uses TinyUSB, not this alternative backend.

Important: nonstandard bitfield HID descriptors and mice that
expose no wheel byte in the selected protocol may need additional
descriptor-based extraction / HID REPORT protocol support.

## On-screen diagnostic

Turn on OSD **MONITOR**. The existing temperature/FPS indicator
now includes **W0** (wheel notches received); turning the wheel
should increase W even if CaffeineOS doesn't scroll anything yet.

- W increases, no Workbench scroll -> inspect AmigaOS FreeWheel/
  NewMouse input handling and window/application compatibility.
- W remains zero -> no usable HID wheel byte was received or
  mapped. Inspect the USB report format, descriptor and protocol.
- Wheel goes opposite direction -> reverse the sign mapping.

The CaffeineOS configuration has not been read; some installations
use a hardware-specific wheel driver that may be unsuitable for
NewMouse raw input. FreeWheel is generally the commodity translating
these raw events to standard Workbench scroll behavior.

## Verification and limitations

- Pi3 big-endian kernel built, FRF_DISTRIBUTION_BUILD=ON.
- Host regression verifies up/down, down/up pair codes, signed
  deltas, optional report ID, absent wheel, max count clamping.
- Independent FRF TinyUSB/DWC2 stays the sole host USB stack.
- No Pi3 hardware wheel test or CaffeineOS compatibility
  confirmation yet. Known working RC13 should remain backed up.
- WHDLoad .ss Save State/Load State remains NO ENGINE.
