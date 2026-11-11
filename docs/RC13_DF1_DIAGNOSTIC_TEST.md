# DF1 diagnosis on Pi 3 — RC13

Back up the known-working kernel8.img before copying the
RC13 BOOT/kernel8.img. Cold boot from power-off; do not simply
reboot the running RC11/12 machine. No repartitioning.

From AmigaShell copy FRFDFTEST into C: and run:

    FRFDFTEST

Capture the output, especially:
- disk.resource cached IDs for DF0/DF1/DF2/DF3
- OpenDevice(trackdisk.device,1) result
- TD_GETDRIVETYPE / TD_GETNUMTRACKS for DF1
- TD_CHANGESTATE before inserting media

Now mount a known AmigaDOS-formatted floppy ADF into DF1:

    FRFADF Games:ADF/WorkbenchDisk.adf DF1
    FRFDFTEST 1

The name is an example: replace it with your real DOS-formatted ADF.
FRFADF $AD81 means the kernel accepted the ADF, NOT Workbench
mount success. TD_CHANGESTATE actual=0 means the drive sees media.
Check whether Workbench shows the DOS volume.

If OpenDevice and TD_CHANGESTATE both pass, OPTIONAL:
    FRFDFTEST 1 READ

CMD_READ reads sector 0 using virtual Paula DMA; don't try it if
drive detection fails or if the game may be using custom disk sectors.
If it hangs, capture the current debug log after reboot.

If Lionheart requires Disk 2 in DF0, swap DF0 rather than mounting
Disk 2 in DF1. Test DF1 separately using a DOS-formatted ADF.
The goal is a working DF1, not just an accepted image buffer.
