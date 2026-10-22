# EMU68AGA Pi 3 — R21 7E05:0920 controller registration fix
22 October 2026. Experimental. Built on Studio Main.
## Ground truth
A zoom of the user's physical R20 HDMI screenshot, R18 EACH USB DEVICE:
  USB D01 7E050920, interface 03000000 (HID, protocol none).
The R18 USB-only photo identified HID address 1/interface 0/length 24,
and user reported button and stick activity in the live changed-bytes
trace. R20 incorrectly allowed only 220D:330C, a guessed identity.
R20 also required a very specific neutral report before PAD became 1.
Neither condition is justified by the hardware evidence.

## R21 changes
- Match only USB VID:PID 7E05:0920, interface 0, HID protocol none.
- Register the exact HID controller at mount time (PAD becomes 1 before
  waiting for a neutral packet). Do not claim ordinary keyboard/mouse.
- If the mount callback did not register a candidate, allow a fallback
  after any complete 24-byte report from this same VID/PID+interface,
  even with a button held or sticks off-center.
- Retain R20 experimental report byte mapping (analog X=p[3], Y=p[4],
  provisional hat=p[9], provisional fire=p[0/1/2/7/8]). This mapping is
  inferred from an R18 still; the manufacturer hasn't verified it.
  PAD recognition should be tested separately from actual joystick.
- The real TinyUSB HID receive callback is still the path for reports,
  mapping to aga_virtual_joy_set(1,...), i.e. Amiga joystick port 2.
- Existing standard HID/R19, ShanWan 2563 gamepad, XInput/GIP,
  keyboard, mouse, read-only MSC and USB DWC2 behaviour preserved.
- POST label is TINYUSB PI3 / PAD R21, and USB-only HDMI footer
  R21 PAD RX/OK/JOY/F shows how many mapped reports arrived.

## Tests
Native x86 and AArch64 BIG-ENDIAN QEMU unit test:
- 7E05:0920 recognized; erroneous 220D:330C rejected
- Interface 1 rejected (doesn't steal mouse)
- PAD becomes 1 at HID mount with a deliberately mouse-only fake
  descriptor and HID protocol NONE
- First report may have left/fire already held, no neutral gate
- R21 complete packet maps to JOY1DAT/fire; shortened packet rejected
- Unplug clears PAD/JOY/FIRE
- Missed mount callback: late registration on active complete 24B report
- R19 and legacy ShanWan mapping regressions
- x86 and ARM-BE all 8 DWC2 host model tests PASS; class-driver and
  signed/unsigned HID descriptor tests PASS.
- Normal and USB-only ARM64 big-endian Pi3 kernels compiled.
Physical Pi3 R21 controller input STILL NOT VERIFIED.

## Test instructions
NORMAL/kernel8.img boots AmigaOS. Copy it to the Pi3 FAT32 boot
partition after backing up the last working kernel. Attach controller
USB receiver before powering on. Check PAD 01 in POST if visible.
In Workbench, run AMIGA_DIAGNOSTIC/FRFJoyDiag from the ZIP. Move
D-pad and stick, press fire; watch JOY1DAT and FIRE.
If no input, use USB_ONLY_TRACE/kernel8.img. This intentionally does
NOT boot AmigaOS, but leaves USB diagnostics live. Photograph PAD count,
R21 PAD RX, OK, JOY, F and HID report source/length after pressing.
Do not infer joystick failure from frozen normal-boot POST alone.
