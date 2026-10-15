# Installing FRF Emu68 Standalone AGA with AGS 3.1 Classic

**Target:** Raspberry Pi 3A+ bare-metal standalone AGA, HDMI monitor, USB input, microSD card. You do **not** need a physical Amiga or PiStorm board.

**Important:** The downloadable public v0.10.23 diagnostic kernel has USB disabled; it is not yet an interactively usable distribution. The local USB-enabled test kernel is a different build. The instructions here describe the target full configuration and our actual AGS setup; never assume the public diagnostic package has the same features.

## 1. Know which image you have

Our development machine contains two different image types:

| Image | What it is | How to use |
|---|---|---|
| \`EMU68-AGS3.1.img\` | **Entire SD-card image**, 56.37 GiB | Flash the whole image to a microSD card (it overwrites that card). This is our existing AGS test setup, **not distributed** by this repository. |
| \`AGS_Classic_AGA.img\` | 28.5 GiB **Amiga RDB hard-disk image**, with an RDB and a separate FAT partition | **Not** a complete Pi boot SD image. It must be imported/migrated with RDB-aware tooling; do not flash it expecting a normal Emu68 boot card. |

The existing full SD image has this verified MBR layout:

| Partition | MBR type | Size | Purpose |
|---|---|---|---|
| 1 | \`0x0B\` FAT32, bootable | 1 GiB | Raspberry Pi firmware, \`kernel8.img\`, configuration, user-provided Kickstart |
| 2 | \`0x76\` | 30 GiB | Emu68/Amiga hard disk containing AGS / AmigaOS |
| 3 | \`0x0B\` FAT32 | 25.4 GiB | Extra file/data partition |

**Do not format partition 2 as FAT32, ext4 or NTFS.** On the Linux host, a \`0x76\` partition will often appear with no recognisable filesystem. This is normal for the Amiga disk layout.

## 2. Prepare an SD card

**Option A — reproduce our existing AGS 3.1 configuration:** If you legally possess the full \`EMU68-AGS3.1.img\`, flash that entire image using Raspberry Pi Imager's custom-image option, or a verified imaging tool. Select the correct removable card and verify capacity; writing an image destroys existing card contents. A card of at least the source image's 60,525,903,872 bytes is required; a nominal 64 GB card generally fits, but check the actual capacity. The resulting boot partition should contain the Raspberry Pi boot files. Replace only the emulator's boot files if upgrading; keep a backup.

**Option B — build your own:** Start with a microSD card using an MBR partition table. Make partition 1 FAT32 (around 1 GiB), and partition 2 type \`0x76\` for an Emu68-compatible Amiga boot volume. Use an Amiga/RDB-aware setup or imaging method to install an actually bootable AmigaOS/AGS system on that partition; merely setting the MBR type will not install or format AmigaOS. A third FAT32 storage partition is optional. This process requires legitimate AmigaOS/AGS sources and is not fully automated by the current release.

## 3. Copy the firmware to FAT32

Extract the **boot archive**, open its \`boot/\` directory, and copy its **contents** (not the enclosing folder) into partition 1's root. Its layout should include:

\`\`\`text
FAT32 root/
  bootcode.bin
  start.elf
  fixup.dat
  bcm2710-rpi-3-b.dtb       (and other included DTBs)
  overlays/
  config.txt
  cmdline.txt
  floppy.txt
  kernel8.img
  kick31.rom               <-- YOU supply this file
\`\`\`

The exact ROM name is **\`kick31.rom\`**, not \`kick.rom\`, because the supplied \`floppy.txt\` contains:

\`\`\`ini
initramfs kick31.rom followkernel
\`\`\`

Use a legally obtained, compatible Kickstart 3.1 ROM. **No ROM is provided with FRF**. Do not use an AmigaOS disk image as \`kick31.rom\`.

## 4. Critical configuration

\`config.txt\` must have \`kernel=kernel8.img\`, \`arm_64bit=1\`, \`include floppy.txt\`, and \`dtoverlay=sdhc\`. The line \`disable_splash=1\` disables the Raspberry Pi firmware rainbow screen, **not** the embedded EMU68AGA artwork. Leave \`dtoverlay=diagnostic,hud\` commented to keep the diagnostics HUD off.

\`cmdline.txt\` contains \`frf_virtual_aga=1\` for this setup.

Check that **all three files exist**: \`config.txt\`, \`floppy.txt\`, and \`cmdline.txt\`. An earlier alpha mistakenly omitted two of them.

## 5. Starting AGS

With a bootable Amiga partition and correct Kickstart, the system should enter AmigaOS/AGS. On our existing AGS 3.1 installation, old startup scripts caused apparent lockups when they attempted legacy networking or other environment-specific commands. We restored normal AGS and WHDLoad game return by commenting out problematic early startup commands.

**Do not automatically re-enable** Miami/AmiTCP \`startnet\` commands or add them to \`S:Startup-Sequence\` / \`S:User-Startup\`. Wi-Fi host integration is still experimental and these classic Amiga networking commands are **not** required for local game play.

If AGS does not reach the menu, use the local firmware's **Ctrl+Shift+F12** Early Startup shortcut and try booting without Startup-Sequence, where supported. Back up Amiga startup scripts before altering them.

## 6. USB, joystick, reset

Our working **local** v0.10.21 has USB keyboard and mouse. The experimental v0.10.23 candidate broadens generic USB gamepad mapping (port 2, axes/hat and primary fire). USB hotplug and the Ctrl+both Windows-logo keys guest-reset shortcut are **not yet verified**. Full host reboot currently uses Ctrl+Shift+F11 in the local input build. **The public ROM-free v0.10.23 diagnostic ZIP has USB disabled**; do not use it to evaluate input features.

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| Blank screen before EMU68AGA splash | Correct Raspberry Pi 3-compatible firmware, \`kernel8.img\`, monitor HDMI mode, SD FAT32 contents |
| Splash appears but Kickstart fails | \`kick31.rom\` exact name, \`floppy.txt\` \`initramfs\` line, ROM compatibility |
| Kickstart boots but no hard disk | Bootable Amiga partition, correct \`0x76\` MBR type, SD controller overlay, Emu68 autoconfig |
| AGS stops on loading or returning from a game | Check \`S:Startup-Sequence\`, \`S:User-Startup\`, old network start commands; do not assume hardware failure |
| Keyboard/mouse absent | Check whether you downloaded the **public USB-disabled diagnostic** rather than the local USB-enabled test build |
| Some OCS demos glitch | Capture demo title and reproduction details; Copper, blitter, sprite and DMA timing remain under investigation |

## 8. Distribution boundaries

We distribute **only** the firmware and FRF code/assets for which redistribution is being reviewed. Every user provides their own legally acquired Kickstart ROM, AmigaOS/AGS software, games and disks. Non-commercial status does not eliminate the duties attached to upstream open-source licences.

The full \`EMU68-AGS3.1.img\` and \`AGS_Classic_AGA.img\` are local reference/test assets, **not GitHub downloads**.
