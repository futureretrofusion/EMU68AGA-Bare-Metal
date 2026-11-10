# Pi 3 RC12 hardware test: external DF1

Back up the existing working kernel8.img before replacing it with
BOOT/kernel8.img from the RC12 test package. Do not format the SD.

Cold boot to Workbench WITHOUT an ADF inserted in DF1, then from
AmigaShell (with FRFADF installed):

    FRFADF Games:ADF/Lionheart1.adf DF0
    FRFADF Games:ADF/Lionheart2.adf DF1

The FRFADF status $AD81 means image accepted by the virtual floppy
backend. Check separately whether Workbench recognizes DF1.

To test an image mounted after a previous image:

    FRFADF Games:ADF/AnotherDisk.adf DF0

Allow at least one second for the deliberate empty-drive interval.

If Lionheart asks for disk 2 in DF0, swap disk 2 INTO DF0 instead.
The loader chooses which drive to read; mounting a disk in DF1
does not alter game code.

Test DF1 detection with a known Workbench-formatted ADF too, because
a custom game disk might not appear as a Workbench icon.
Keep DF0 IK+.adf boot as regression. Log any failed DF1 selection,
missing icon, disk-change indication or motor behavior.

Guest reset: Ctrl + Left Alt + F11, not Pi host reboot.
This build is not yet verified on physical Pi hardware.
