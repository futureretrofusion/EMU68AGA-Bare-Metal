# Exact boot combination: v0.10.22 support files + v0.10.25 kernel

## Hardware confirmation

On 15 October 2026 the project owner confirmed that **v0.10.25's
USB-enabled `kernel8.img` boots with the v0.10.22 boot directory**.
The newer independently packaged v0.10.25 and v0.10.26 boot folders
had failed. This is an important configuration regression, not
evidence that the SD-card partition contents were changed.

## Known matching local files

- All `config.txt`, `cmdline.txt`, `floppy.txt`, Pi firmware
  and `overlays/` files originate from
  `v0.10.22-keyboard-pal50/boot` and remain byte-for-byte unchanged.
- The **only** changed file in the validated local combination is
  `kernel8.img`, obtained from v0.10.25.
- SHA-256 of kernel:
  `138bb918681fe2072c783484b11da9a3e4f50889305c0214aefc349d7c10fc1f`.
- SHA-256 of working `config.txt`:
  `9811f19fdafde1d9c05a6f9c3c4e981c1e35f129a095cbc98abb293e12d8a4f8`.
- `config.txt` includes both `dtoverlay=sdhc` and
  `dtoverlay=diagnostic,hud` (enabled, as in v0.10.22).
- `floppy.txt` includes `initramfs kick31.rom followkernel`.

## Boot directory layout

```text
FAT32_ROOT/
  kernel8.img           <- user-provided compatible kernel
  kick31.rom            <- user's legally obtained Kickstart ROM
  config.txt            <- exact v0.10.22 config
  cmdline.txt
  floppy.txt
  bootcode.bin
  start.elf
  fixup.dat
  start4.elf
  fixup4.dat
  LICENCE.broadcom
  bcm2710-*.dtb
  bcm2711-*.dtb
  overlays/
    sdhc.dtbo
    diagnostic.dtbo
```

The existing second MBR partition, type 0x76, must contain an installed
bootable AGS/AmigaOS system; the FAT32 boot folder alone is insufficient.

The [public boot-support ZIP](../distribution/EMU68AGA_BCM_v0.10.25_BOOT_SUPPORT_ROM_FREE.zip)
contains the original matching boot configuration and Raspberry Pi firmware,
but **no kernel, Kickstart ROM, AGS, AmigaOS or game images**. Its purpose
is to prevent future packaging errors while the USB-linked kernel's licence
issue is resolved. It cannot boot by itself.

## Hardware-test interpretation

A successful boot verifies that the combination starts in that setup.
It does **not** yet establish that USB device reconnection, every gamepad,
Carmageddon or all WHDLoad games work, or that a new independent USB stack
has been completed.

## Distribution

The private working kernel combines AGA-PiStorm GPL-2.0-only with
USPi GPL-3.0-or-later. Public redistribution of the combined binary is on
hold until licensing is addressed.
