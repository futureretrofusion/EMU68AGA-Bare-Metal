# EMU68AGA Bare Metal v0.10.43

AGA-PiStorm 1.118 release adding working USB mass-storage writes to the hardware-verified v0.10.42 multi-button baseline.

## What changed
- USB mass-storage writes are enabled for the Pi 3 TinyUSB MSC path.
- `FRF_TINYUSB_MSC_WRITE` is now ON by default.
- The guest mailbox supports `FRF_UM_WRITE` sector requests.
- The matching Amiga `usbmass.device 0.2` supports `CMD_WRITE`, `TD_WRITE64`, and `NSCMD_TD_WRITE64`.
- `TD_PROTSTATUS` reports writable media when the USB device is live.
- Preserves AGA 1.118, v0.10.42 multi-button USB controller handling, compact POST box, and diagnostics-off release behavior.
- POST version line reads `v0.10.43 / AGA 1.118`.

## Hardware validation
USB mass-storage writing was verified on the live Amiga after replacing the old resident/on-disk `usbmass.device 0.1` with the matching `usbmass.device 0.2`.

## Installation
Install both components from the release package:
- `kernel8.img` on the Pi boot partition.
- `DEVS:usbmass.device` on the Amiga system volume.

Reboot after replacing `DEVS:usbmass.device` so the 0.2 driver is loaded.

## Rollback
Restore the v0.10.42 `kernel8.img` and the previous `DEVS:usbmass.device` together.
