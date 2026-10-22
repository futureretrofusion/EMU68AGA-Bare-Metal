# EMU68AGA R13 — Native TinyUSB gamepad discovery
2026-10-22 — experimental AArch64-BE Pi 3.

## Confirmed status
- R11 keyboard + mouse: physically working.
- R12 USB mass storage: user reports detected.
- R12 gamepad: not detected.
- R13 gamepad support: source compiled and software tested, physical Pi3 still unverified.

## New R13 features
- Native TinyUSB application class driver for wired Xbox 360/XInput compatible USB interface FF/5D/01, including Nacon-compatible devices.
- Xbox One GIP interface FF/47/D0 for known vendors; asynchronous USB power/init/LED/auth handshake and input report decode.
- HID configuration/report descriptor buffer increased from 256 to 1024 bytes.
- Existing standard HID gamepad parsing and Amiga joystick-port 2 mapping retained.
- ShanWan 2563:0576 and 8BitDo VID 2DC8 generic HID devices can be counted as candidates when their descriptor is unavailable or unknown. Unrecognised report formats are not blindly mapped into Amiga input.
- R13 HDMI POST contains a supplementary PAD USB INTERFACE SCAN panel:
  VENDOR: most recent VID:PID.
  CLASS/IF: USB interface class, subclass, protocol, interface number.
  PROTOCOL: 0 unsupported vendor, 1 XInput, 2 Xbox One GIP, 3 unknown generic HID.
  VEN EVENTS: actual interface class probes.
  PAD/MSC: high 16 bits recognized pad count and low 16 bits USB mass-storage count.
- The USB_ONLY_TRACE kernel deliberately DOES NOT boot Amiga and shows the DWC2 + class probe panels continuously.

## Verification
- Two AArch64 big-endian kernels compiled and linked, normal and diagnostic.
- x86 and AArch64-BE QEMU controller and USB packet tests passed, including standard HID, Xbox 360 XInput, Nacon detection, Xbox One GIP decoding, joy-port quadrature, fire, USB MSC command format.
- No claim of physical Xbox/XInput/gamepad enumeration until user Pi3 test.
- USB mass storage guest WRITES still disabled by default.

## Hardware sequence
1. Back up the known-working R11/R12 kernel8.img.
2. Put NORMAL/kernel8.img onto the test FAT boot partition; cold-boot Pi with keyboard/mouse and gamepad plugged in before power-on.
3. Observe R13 screen PAD count, interface scan. Try Amiga joystick port 2 in a game.
4. If PAD=0, use USB_ONLY_TRACE/kernel8.img. This stalls at the live debug screen by design. Photograph VID:PID, CLASS/IF, PROTOCOL, PAD/MSC as the controller is connected.
5. Restore R11 if input regresses. Keep R13 experimental separate from main.
