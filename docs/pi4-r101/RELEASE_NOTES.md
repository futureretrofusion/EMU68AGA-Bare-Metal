# EMU68AGA Bare Metal — Pi4 R101 Experimental Preview

Pi4-specific experimental source and binary preview on the `pi4-r101-boot-post` branch. Existing Pi3 `main` branch and releases remain unchanged.

## Changes
- Original EMU68AGA circuit-board boot splash preserved; small boot-only Pi4 POST window added.
- POST reports native VL805/xHCI host state and keyboard, mouse, gamepad and hub counts.
- Quiet production configuration with no persistent debug HUD, serial debug or AGA logs.
- Native Pi4 USB HID gamepad support retained.

## Known issues
- Cascaded Huasheng 214b:7260 USB2 hubs may drop out; use direct USB for initial tests.
- R101 has passed a clean cross-build and was installed on an SD card, but the new POST has not been separately verified by a recorded cold boot.
- Pi4 requires a compatible boot initramfs bundle specified in the [pi4] config.txt section. The file EMU68AGA_PI4_CAFFEINE_ROM_ONLY.bin was missing from the examined card and must be supplied separately by the user. No Kickstart ROM, Workbench or commercial software is distributed.
- R100 is the recommended rollback if R101 POST causes boot regression.

## Installation
Copy kernel8_R101_PI4_ORIGINAL_SPLASH_BOOT_POST.img to the FAT boot partition and select it in the [pi4] kernel= line in config.txt. Preserve [pi3], [all], and the existing initramfs setting, ensuring the referenced file exists. Back up your previous kernel and configuration.

## Licensing
AGA chipset GPL-2.0-only; Emu68 MPL-2.0; UAE/WinUAE and third-party attributions retained. Optional USPi is not linked in this Pi4 binary. See machine/LICENSE, machine/COPYING and machine/Emu68/LICENSE.
