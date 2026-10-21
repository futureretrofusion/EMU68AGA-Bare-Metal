# EMU68AGA Pi 3 — R12 USB Mass Storage / HID Joypad Test

**Status: EXPERIMENTAL, NOT physically validated.** October 22, 2026.

## Proven baseline

R11 commit `4ed0712` is the FIRST version physically confirmed to enumerate and operate a USB mouse and USB keyboard on the user's Pi 3. Its immutable remote source branch is `pi3-tinyusb-r11-hardware-verified`. Keep it intact; R12 is a separate development branch.

## Changes

- **USB mass storage**: native TinyUSB SCSI/BOT class, little-endian CBW fixes for the AArch64 big-endian host (signature, tag and transfer count), read-capacity enumeration, 512-byte block device selection, asynchronous READ10 with a 64-byte-aligned 8KiB DMA buffer, and the existing Amiga Zorro-II FRUM guest mailbox board. Writes are explicitly disabled by default via `FRF_TINYUSB_MSC_WRITE=OFF` until real-hardware read tests pass. Non-512-byte logical-block devices are detected but not exposed to this 512-byte guest protocol. Mounting the device in Workbench requires the preexisting guest `usbmass.device` and its DOS mountlist to be installed.
- **Joypads**: identifies HID Generic Desktop joystick/gamepad interfaces from report descriptors, including report IDs, and logs VID:PID of USB devices. Standard TinyUSB 11-byte signed-axis/hat/buttons format maps controller 1 to Amiga joystick port 2 (JOY1DAT + fire). Other vendor-specific/XInput HID report layouts are *detected when exposed as HID*, but are not yet decoded; non-HID Xbox/GIP receivers still require class support.
- **Keyboard**: native Ctrl (either side) + **left Shift** + F11 invokes host `aga_reboot(0)` on CPU1 on a single press edge, not from inside an HID callback. It restores the documented original host reboot feature.
- **POST**: `TINYUSB PI3 / MSC PAD R12`. Displays mounted MSC count, HID PAD count and 512-byte capacity in sectors.
- **Big-endian correctness**: USB MSC CBW 12-byte header regression and HID descriptor item-header/one-byte usage-page fixes.

## Validation on Studio Main

Both AArch64 big-endian normal and USB-only diagnostic kernels compiled/linked. The normal keyboard unit test passed including repeat suppression. x86 and big-endian AArch64 QEMU DWC2 model tests passed for root port, host control transfer, USB BOT signature/tag/512-byte transfer length, gamepad descriptor detection, port 2 motion/fire, neutral and unplug behaviour.

**Not validated**: real USB flash drive block read or Workbench mount, USB flash drive writes, physical controllers or hotkey reboot on Pi hardware. DO NOT claim these are working until tested.

## Recommended hardware test order

1. Use a TEST SD card. Back up `kernel8.img` and copy R12 `NORMAL/kernel8.img` into the FAT32 boot partition. Keep R11 available on a separate SD.
2. Confirm keyboard and mouse continue to work FIRST, including after prolonged use.
3. Attach a spare USB stick with nonessential files. Check `MSC` count on POST, capacity blocks and USB0/FRUM guest visibility. Test read-only first; do not use an irreplaceable disk.
4. Attach an ordinary USB HID gamepad. Confirm `PAD` count and observe joystick port 2 movement and fire. For vendor/XInput receivers provide VID/PID and USB report layout if detected but not mapped.
5. Test Ctrl+**left** Shift+F11 after confirming system responsiveness. This **reboots the host Pi**, not just the 68k guest.
6. The `USB_ONLY_TRACE` image deliberately holds on the POST and does not boot Amiga.

Hardware verification required before merging into `main`. No copyright ROM files or private credentials are needed.
