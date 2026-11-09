# RC10 Pi 3 test procedure

## Installation

Make a backup of the existing FAT boot partition kernel8.img.
Copy BOOT/kernel8.img from the RC10 test ZIP in its place.
Use the same proven ROM, firmware, config and Amiga RDB partitions.
No disk formatting or partition changes required.

## OSD

Use Left Ctrl+Left Alt+F12 (alternative Left Ctrl+Left Shift+F12)
while native AGA graphics are active. Navigate with arrows/Enter,
Escape to close. Scroll to the floppy section:

- ADF SLOT: choose 1–4.
- DF0 INSERT: mount selected loaded slot into DF0 (read-only).
- DF0 EJECT: eject current DF0 media (rejects dirty disks).
- DF1 INSERT and DF1 EJECT: corresponding second virtual drive.
- AMIGA SOFT RESET: press Enter twice to confirm.
  Restart from Kickstart without Raspberry Pi power-cycle.

The OSD has NO ADF for an unloaded slot. The boot ADF is saved into
slot 1 automatically if boot supplied one. There is no SD file picker
in the OSD yet.

## Populating swap slots

Copy the included AmigaOS FRFADF tool into your booted Amiga, e.g. C:.
Run from AmigaShell, specifying an AmigaDOS volume path:

    FRFADF Games:ADF/IKPlus.adf SLOT1
    FRFADF Games:ADF/IKPlusDisk2.adf SLOT2

The OSD can now switch the loaded images in DF0 or DF1.
Only 901120-byte DD and 1802240-byte HD ADFs are accepted.

This AmigaOS-side loader is NOT a mechanism to read the Raspberry Pi
FAT32 boot partition directly. It reads any volume/path visible to
AmigaDOS, and passes the image to the bare-metal chipset.

## Reset hotkeys

- Ctrl+LWin+RWin: Amiga guest soft reset.
- Ctrl+LeftAlt+F11: alternative guest soft reset.
- Ctrl+LeftShift+F11: **Pi host reboot**, unchanged.
- Ctrl+LeftAlt+F12: open OSD.
- The reset menu requires two separate activations.

## Verification and limits

Test reset with Workbench and then IK+ or another WHDLoad title.
Confirm return to Kickstart/Workbench without the Pi boot splash.
Confirm ADF images remain inserted across guest reset.

Physical Pi 3 tests are **not yet complete**. Report any black screen,
reset loop, lost USB input, or virtual disk loss and preserve logs.

WHDLoad .ss Save State / Load State remain NO ENGINE (not implemented).
Do not use this experimental kernel as a replacement for a verified
RC8 release without keeping a backup.
