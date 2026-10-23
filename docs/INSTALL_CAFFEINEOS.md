# Using EMU68AGA with CaffeineOS

> ## Reset shortcut
>
> **Press `Ctrl + Left Shift + F11` to reset the EMU68AGA system.** Use this instead of power-cycling the Raspberry Pi when you need to reboot the Amiga environment.

This is the easiest route for users who want a complete Amiga environment rather than building an SD card manually.

As of October 2026, **CaffeineOS 9317** is the current full CaffeineOS image referenced by MultibootOS 2.2. CaffeineOS remains a separate project; EMU68AGA does not redistribute it, Kickstart ROMs, AmigaOS files, games or other copyrighted software.

## What this method does

Use a normal CaffeineOS SD-card image as the Amiga filesystem/software base, then replace the Emu68 kernel on its FAT32 boot partition with the current EMU68AGA kernel.

**Do not erase the CaffeineOS Amiga partitions.** They contain the operating system, Workbench setup, applications and data that you want to keep.

The current EMU68AGA v0.10.40a release package contains the hardware-tested `kernel8.img`. You normally keep the Raspberry Pi firmware, CaffeineOS partitions, ROM file and other boot support files from the CaffeineOS card and replace only the Emu68 kernel.

## 1. Prepare CaffeineOS

1. Download the latest CaffeineOS image from the CaffeineOS project. At the time this guide was written, the current full image is CaffeineOS 9317.
2. Write the image to a microSD card with your preferred imaging tool.
3. Reinsert the card in your PC so the FAT32 Emu68 boot partition is visible.
4. **Back up the complete contents of that FAT32 partition before changing anything.**

Depending on the image/tool, the boot partition may appear as `EMU68`, `EMU68BOOT`, or simply as a FAT32 boot volume.

## 2. Install the EMU68AGA kernel

Download the current EMU68AGA release:

https://github.com/futureretrofusion/EMU68AGA-Bare-Metal/releases/tag/v0.10.40a

Use the release asset named `kernel8.img`, or extract the matching R40a ZIP and use:

`NORMAL/kernel8.img`

On the CaffeineOS FAT32 boot partition:

1. Locate the Emu68 kernel currently selected by `config.txt`.
2. Make a backup of that kernel.
3. Copy the EMU68AGA `kernel8.img` to the boot partition.
4. The simplest layout is to keep it in the root of the boot partition and make sure `config.txt` contains:

```ini
kernel=kernel8.img
arm_64bit=1
```

If your CaffeineOS image deliberately uses a different kernel path, either keep that path and replace the selected file in place, or change `kernel=` to point at `kernel8.img`.

**Do not delete the rest of the CaffeineOS boot partition.** In particular, keep its Raspberry Pi firmware files, ROM file, filesystem layout and CaffeineOS Amiga partitions.

## 3. 1280x720p 50 Hz HDMI configuration

Our retained known-good EMU68AGA Pi 3 boot configuration uses **1280x720 progressive at 50 Hz**.

Add or confirm these lines in `config.txt`:

```ini
hdmi_force_hotplug=1
hdmi_drive=2
hdmi_group=1
hdmi_mode=31
```

For Raspberry Pi firmware, CEA group 1 / mode 31 selects **1280x720p50**.

The retained working boot configuration also contains:

```ini
core_freq=250
gpu_mem=32
temp_soft_limit=70
```

A compact tested baseline therefore looks like:

```ini
kernel=kernel8.img
arm_64bit=1
disable_splash=1
enable_gic=0
avoid_warnings=1
enable_uart=1

core_freq=250
gpu_mem=32
temp_soft_limit=70

hdmi_force_hotplug=1
hdmi_drive=2
hdmi_group=1
hdmi_mode=31
```

Keep any additional CaffeineOS-specific settings that your image requires.

## 4. Overclocking

Our development Amiga has also been run with additional Raspberry Pi overclock settings.

**Do not copy guessed overclock values from the internet.** Preserve the overclock lines already proven on your own Pi/card, and change one setting at a time. The exact CPU/voltage values from the project's live Amiga should be treated as machine-specific rather than a mandatory part of EMU68AGA.

The repository's retained boot copy confirms `core_freq=250` and `temp_soft_limit=70`; the later live CPU overclock values are intentionally not documented here until they have been re-read from the active `EMU68:` partition and verified.

If you have a known-good overclock in your existing CaffeineOS `config.txt`, leave those lines in place when replacing the kernel.

## 5. First boot

**Remember: `Ctrl + Left Shift + F11` resets the system.**

1. Safely eject the card.
2. Install it in the Pi 3A+.
3. Connect HDMI, keyboard/mouse and your USB controller.
4. Boot normally.
5. Confirm that Workbench/CaffeineOS loads.
6. Confirm the display reports 1280x720 at 50 Hz.
7. Test keyboard, mouse and joystick/gamepad input before making further changes.

If the machine does not boot, restore the backed-up FAT32 boot partition first. Do not alter the CaffeineOS Amiga partitions while troubleshooting the kernel.

## Updating EMU68AGA later

For later EMU68AGA releases you normally do **not** need to re-image CaffeineOS.

Back up the current Emu68 kernel, replace it with the newer tested `kernel8.img`, keep your existing CaffeineOS partitions/configuration, and reboot.

That is the intended upgrade path: **CaffeineOS supplies the complete Amiga environment; EMU68AGA supplies the standalone bare-metal Emu68/AGA kernel.**
