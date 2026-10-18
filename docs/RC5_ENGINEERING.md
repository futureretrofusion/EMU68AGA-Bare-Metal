# EMU68AGA RC5 — USB0 and USB gamepad investigation

Built on 18 October 2026 for Raspberry Pi 3, retaining the RC4/AGA v1.117 baseline. This is a diagnostic candidate; physical USB write success and gamepad gameplay remain unverified.

## FAT95 and USB mass storage

An alternative USB0 mount file changes `HighCyl = 0` to `HighCyl = 1` for automatic MBR FAT partition detection, as documented by the current fat95 project. **Back up the current mount file first**. Keep `usbmass.device` as installed; RC5 does not assume that it is an old version.

The read-only 68k program `usbmass-mountinfo` reports `TD_PROTSTATUS`, capacity and the actual MBR partition without writing any sectors. It prints a candidate FAT95 mount definition. The Z2 diagnostic board records read requests, write requests, failures, last LBA and signed result, allowing a distinction between FAT95 refusing a request and the underlying USB host failing a write.

Do not test writes on media containing irreplaceable data.

## Gamepad

The `usbpad-info` AmigaShell diagnostic reads the reserved `$3000` area of the existing USB mass-storage Zorro-II board. CPU1 publishes a telemetry snapshot every 100 ms (without doing slow shared-memory writes inside the USB interrupt callback). It shows paired receiver enumeration count, successful/ignored/failed input transfers, HID report length and ID, first eight raw bytes, decoded callbacks, JOY1DAT and fire state.

The current ShanWan 2563:0576 decoder still expects report ID 1 and exactly 10 bytes, pending physical report evidence. RC4's bounded retry protects keyboard responsiveness. This release does **not** yet assert that gamepad gameplay works.

## Test procedure

Copy only the new `kernel8.img` to the Pi3 FAT boot partition after backing up the old one. Keep the working ROM/config/overlays. Copy the two compiled 68k tools to `C:`. Boot with the receiver already attached. Run `usbpad-info` before and after moving sticks/pressing buttons; run `usbmass-mountinfo` on the USB stick and save its output. Test an optional small file write only on disposable media after mount validation, then run `usbpad-info` again.

Restore the old kernel and DOSDriver definition if anything regresses. Report actual diagnostics to futureretrofusion1984@gmail.com.
