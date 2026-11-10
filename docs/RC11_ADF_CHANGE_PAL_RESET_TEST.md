# RC11 Pi 3 hardware test checklist

Backup the known-good RC10 or RC8 kernel8.img before trying this
developer kernel. Replace ONLY kernel8.img on the known-good boot
partition. Do not format the SD card.

## Disk change

1. Boot Workbench from the existing hard-disk setup and confirm it
   remains operable. Launch AmigaShell.
2. Using the included FRFADF tool, load one valid DD ADF into DF0:
       FRFADF Games:ADF/Disk1.adf DF0
3. Wait at least one second (RC11 deliberately stages swap media),
   and check that the floppy icon/label appears on Workbench.
4. Load a DIFFERENT DD ADF:
       FRFADF Games:ADF/Disk2.adf DF0
5. Wait at least one second. The original disk should be removed and
   the new disk icon/label detected WITHOUT restarting the Pi.
6. Eject DF0, then repeat with DF1. Test rapid reinsert only after
   verifying the normal interval, and do not discard dirty images.
7. OSD ADF slots 1–4 now use the same delayed replacement path.

## Lionheart

Load the exact Lionheart disk 1 ADF, wait one second for the swap
to finish, and request an Amiga soft reset (not a Pi reboot).
Record whether the bootblock is read, whether the Amiga PAL screen
appears, and the first failed or stalled track read. Supply the ADF
SHA1/size and logs when possible; RC11 is NOT confirmed to boot
Lionheart. A known-booting IK+.adf should still boot as regression.

## Guest reset PAL

Boot an RTG Workbench, then press:
  Ctrl + Left Alt + F11
or:
  Ctrl + Left Win + Right Win
Expect the Pi not to reboot; the old P96 display disappears, and
the Amiga's native PAL output takes over while Kickstart runs.
After a new RTG screen is activated, VideoCore.card/P96 may
take HDMI ownership again. Repeated resets should not leave the
last RTG framebuffer on screen.

Ctrl + Left Shift + F11 is still a Pi reboot; avoid during tests.

NOTE: The kernel resets PAL *AGA composition*, not necessarily HDMI's
physical 50 Hz video mode. This hardware mode switch is separate.

## Build reproducibility

Configure the distribution build as in docs/BUILD_1.121_RC8.md but add:
  -DFRF_RTG_HANDOFF=ON

## Other unchanged components

- WHDLoad state-slot UI exists; .ss Save/Load engine is NOT yet enabled.
- No PiStormAGA USB additions; FRF TinyUSB/DWC2 remains in service.
