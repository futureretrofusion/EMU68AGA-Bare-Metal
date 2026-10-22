# EMU68AGA Pi 3 R17 USB IN Endpoint Trace
October 22 2026 — Experimental, physical gamepad input not yet verified.

## Evidence from R16
- R16 photo: USB device address 4 carries VID:PID 2404:0078, vendor interface FF/00/FF/00, flags 09. Flags 09 mean device mounted and vendor interface scanned, NOT that an endpoint is active.
- HID RX=0, VEND RX=0, PAD=0 in photographed early POST. Wireless receiver synchronisation is not proof of active USB data transfers.
- This device is not confirmed to be the Dyonder. The earlier ShanWan 2563:0575/0576 parser remains available.

## Source changes
- Read-only vendor probe for exact VID:PID 2404:0078 and FF/00/FF accepts both BULK IN and INTERRUPT IN endpoints. R16 accepted interrupt only and silently returned on unmatched endpoints.
- USB endpoint descriptor bAttributes transfer type read directly from wire byte bits 1:0 for correct big-endian ARM operation.
- R17 2404:0078 USB IN PROBE POST panel shows:
  STAGE/ADDR (packed SS DD II KK): stage, USB address, interface, driver kind
    01 scanned; 02 no suitable IN endpoint; 03 endpoint found;
    04 endpoint open failed; 05 custom class driver bound;
    06 configured; 07 USB IN submitted; 08 IN completed;
    09 IN transfer error.
  LAST EP/TYP: packed EE TT SSSS: endpoint address, USB transfer
    type (02 bulk, 03 interrupt), max packet size in bytes.
  OPEN EVENTS, IN SUBMIT OK, IN SUBMIT NO, IN RX OK, IN RX ERR.
- Preserve device-by-device table and earlier raw HID, raw vendor,
  decoded-pad and virtual Amiga register-read telemetry.
- Unknown vendor packets are NOT mapped to joystick and no vendor
  control/output commands are sent. The existing keyboard, mouse, MSC,
  supported XInput and Dyonder HID paths are unchanged.

## Physical test
Use USB_ONLY_TRACE/kernel8.img on test SD to keep live POST showing,
with gamepad receiver inserted. After 15 seconds pressing buttons
photograph R17 USB IN PROBE and R17 USB -> AMIGA INPUT LINK.
Stage 02 => no IN descriptor (LAST EP/TYP can show transfer type).
Stage 04 => HCD cannot open endpoint.
Stage 05/06 => driver bound/configured but not submitting IN.
Stage 07 / IN SUBMIT OK>0 / IN RX OK=0 => transfer pending/NAK.
Stage 08 / IN RX OK>0 => USB data arrived; inspect raw packet fields.
Stage 09 => USB transfer failures.
All zeros => 2404:0078 not enumerated; inspect USB address table.
USB_ONLY_TRACE does not start Amiga, guest register reads stay zero.
Normal kernel boots Amiga and includes R16 FRFJoyDiag to check JOY1DAT.

## Validation
Normal Pi3 and USB-only Pi3 AArch64 BE kernels compiled.
Real TinyUSB custom class-driver tested against synthetic bulk-IN and
interrupt-IN descriptor input through its actual open, configure,
transfer submit and completion callbacks on both x86 and AArch64-BE.
All eight DWC2 controller cases pass in x86 and ARM-BE after one
intermittent simulation retry on x86.
Dyonder HID report/mount tests pass both architectures.
No physical Dyonder input success claimed.
