# EMU68AGA Pi 3 — R16 USB -> Amiga Input Link Diagnostic
22 October 2026. **Experimental — no physical R16 gamepad verification.**

## Status from user
R11 Pi3 keyboard/mouse confirmed working. R12 mass storage detected.
R15 Dyonder controller/receiver now reportedly pairs/synchronizes,
but no joystick movement/buttons reach Amiga software. Pairing is not
proof of a valid USB interrupt report, a decoded joystick report,
or a virtual Amiga hardware register update.

## What R16 adds
- Intact R11–R15 USB kernel, boot mouse/keyboard and read-only mass
  storage; no changes to their established functionality.
- R16 normal POST **USB -> AMIGA INPUT LINK** lower-left panel, under
  last vendor-interface panel. At 1280x720 it shows:
    HID RX          all actual completed TinyUSB HID reports
    HID DEV/IF      last USB device address (high16)/HID interface (low16)
    HID LEN/ID      last report length (high16) / first report byte ID
    HID RAW 0       first 4 raw HID report bytes, network order HEX
    VEND RX         successful interrupt-IN packets captured from the
                    *specific* 2404:0078 interface FF/00/FF
    VEND LEN/ID     vendor capture: USB daddr high byte, data length next
                    byte, first byte report ID low
    VEND RAW 0      first 4 raw vendor report bytes, HEX
    PAD DECODE      reports forwarded by standard HID/Dyonder pad handler
    DY VALID        validated ShanWan 10-byte ID01 pad reports
    JOY/FIRE        low16 last JOY1DAT sent to virtual AGA, bit16 fire
    JOY WRITES      number of virtual Amiga port2 joystick updates
    GUEST JOY RD    number of actual 68k custom-reg reads (JOY1DAT)
    GUEST CIA RD    number of actual CIA-A button-port reads
  Values refresh in CPU1 task (not in USB ISR). Before AmigaOS boots,
  GUEST JOY RD/CIA RD may be zero. On the USB_ONLY_TRACE build they
  stay zero by design because the 68k guest is not started.
- Actual AGA glue instruments JOY1DAT and CIAA button read access.
- Read-only **passive probe** for vendor-interface class FF/00/FF of
  VID:PID 2404:0078 seen in R14 photo. Probe only claims its interrupt
  IN endpoint when present; it logs raw packet length, ID and bytes.
  It does not pretend this is the Dyonder, does not send USB output,
  and NEVER writes unknown packets to the Amiga joystick.
  If VID:PID on R15 device table differs, that requires separate
  protocol-specific handling. No fake "PAD detected" result.
- A native AmigaOS 68000 Shell utility `FRFJoyDiag` and source
  `joydiag_amiga.c` are provided separately. Run `FRFJoyDiag`
  from a Workbench Shell. It polls JOY1DAT $DFF00C and CIAA PRA
  $BFE001 (joystick-2 fire bit7) for 600 * 3 PAL ticks (~36 sec),
  printing only changes. Move controller stick/D-pad and press fire.

## Readouts and next real hardware gate
**FIRST use NORMAL/kernel8.img** to confirm no keyboard, mouse, or MSC
regression, then run FRFJoyDiag from Workbench Shell:
- JOY1DAT changes when D-pad is pressed, FIRE 1 when trigger pressed:
  the full guest hardware input path is WORKING. A particular game
  might be using joystick port1 instead of port2 or requiring different
  button wiring; debug game expectations.
- JOY1DAT always unchanged: the guest is NOT seeing delivered gamepad
  input. Then boot USB_ONLY_TRACE/kernel8.img.
- In USB_ONLY_TRACE leave gamepad receiver connected and move it:
    HID RX increases + HID RAW changes, PAD DECODE or DY VALID zero:
      USB packets arrive but the report is not decoded by our HID path.
    VEND RX increases with changing VEND RAW, but PAD DECODE zero:
      device might use the 2404:0078 unknown vendor protocol.
      Attach photos of panel and raw words, to write its decoder.
    Neither HID nor VEND RX increases with button presses:
      receiver synced but host lacks correct interface/interrupt-IN,
      or it is not the connected USB device. Read right-hand EACH
      USB DEVICE table (VID/PID/class flags). USB-only guest read
      counters are expected to stay zero.
    PAD DECODE/DY VALID increases + JOY/FIRE changes:
      USB->AGA output call exists; run guest FRFJoyDiag to verify.
- If keyboard/mouse stop working, restore hardware-proven R11/R12.

## Bench tests
- R16 bare-metal normal and USB_ONLY_TRACE AArch64 big-endian
  Pi3 firmware builds linked.
- 8 native x86 DWC2 model scenarios passed.
- 8 AArch64-BE QEMU DWC2 model scenarios passed.
- True Dyonder composite HID mount, 0575/0576, ID1 pad, ID5 mouse
  isolation, failed initial transfer recovery tests passed x86 & BE.
- FF/00/FF passive vendor classification 2404:0078 and *no virtual
  joystick mapping* explicitly tested in BE model.
- Actual AmigaOS binary compiled and validated as Amiga LoadSeg format.
- No physical Dyonder/Amiga port reads available via tunnel at this
  stage, so do not claim R16 gamepad works until tested.

## Source policy
- New independent experimental branch, R11 known-good branch intact.
- No merge into main or guest MSC writes.
- No USB or Amiga input is synthesized purely to pass a benchmark.
