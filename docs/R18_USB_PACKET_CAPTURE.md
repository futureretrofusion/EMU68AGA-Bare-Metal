# EMU68AGA Pi3 R18 — Full USB Packet Byte Capture
22 October 2026. Diagnostic only; no physical controller button mapping yet.

## What the user reported
The R17 input-link telemetry now shows input activity. The earlier R16 screen
showed zero receive counters. This is progress toward receiving USB data,
but the user has not yet supplied the packet bytes or confirmed that
the Amiga JOY1DAT or fire signal changes.

The R17 2404:0078 probe is intentionally read-only, and its input link
only displayed the first FOUR report bytes. That is insufficient to
reverse engineer D-pad, buttons, hat and analog axes without guessing.
No publicly verified report layout has been established for this exact ID.

## R18 implementation
- New independent passive 64-byte USB packet capture, from *real*
  HID report callbacks and the R17 2404:0078 vendor IN completion callback.
- Selects vendor packets when they exist; otherwise shows the most recently
  received HID report. SOURCE=2 for vendor, SOURCE=1 for HID, 0 none.
- A large lower-left 604x416 pixel HDMI panel at >=980x704 resolution
  shows the actual first 64 bytes as a row of eight hexadecimal digits
  per four bytes. Columns:
    BASE = first seen USB report with the same real length/report ID,
    LIVE = most recently received report,
    XOR  = exact bit changes LIVE xor BASE.
  Orange XOR hex !=0 indicates potentially changing controls.
- Tracks RX packet count, changed-packet count, last changed byte count
  and packed ADDR/EP/LEN/ID, in that hexadecimal byte order.
  NOTE: on HID the second byte refers to the interface index; on
  vendor class it refers to the USB endpoint address.
- Resets BASE when report type (first byte or actual length) changes.
  Does not produce synthetic joystick data. Does not transmit control
  packets to the receiver. Does not affect existing USB enumeration.
- R17 live endpoint probe, input link, device table all remain available.
- Keeps legacy Dyonder 2563:0575/0576 report-ID-1 decoder,
  real XInput and GIP mappings, and keyboard, mouse and MSC bridge.

## Pi3 real hardware test
1. Save working R17 or R11 kernel. Prefer a test SD.
2. Put USB_ONLY_TRACE/kernel8.img on FAT32 boot volume and cold boot
   with controller receiver connected and wireless gamepad synchronized.
   This version intentionally does NOT boot Workbench.
3. Let the HDMI image settle. Photograph the R18 BYTE DIFFERENCES panel
   with the controller untouched (neutral).
4. Hold D-PAD LEFT or move the left stick fully LEFT. Photograph again.
5. Release and hold one FIRE/A button. Photograph a third time.
6. Include SOURCE, RX, CHANGED PKTS, ADDR/EP/LEN/ID and all rows
   BASE/LIVE/XOR. This allows accurate mapping of real packet offsets.
7. If SOURCE=1 rather than 2, compare the reported HID device address
   to the R18 device table. It could be the keyboard or mouse rather
   than the controller. No unsupported inference.

## Status and limits
- Both AArch64 big-endian Pi3 kernels compiled/linked.
- x86 and AArch64-BE regression tests pass for 64-byte packet capture,
  byte-difference XOR and report-ID/length baseline change.
- Previously working DWC2, HID, vendor BULK/INT probe and Dyonder
  regression suites passed on x86 and AArch64-BE.
- Physical input capture and Amiga joystick mapping are NOT yet proven
  by this new build.
- USB mass storage guest writes remain disabled.
