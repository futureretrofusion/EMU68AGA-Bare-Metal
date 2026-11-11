# R101 GitHub release audit — 2026-11-11

## Verified in source tree

- Pi4 64-bit big-endian source successfully cross-built with
  FRF_RELEASE_QUIET=ON and FRF_BOOT_POST=ON.
- Compact 320x202 boot-only POST is linked, with live USB device counts.
- Existing original RGB565/RLE boot image data is byte-for-byte
  unchanged from R100. Lossless decoded 480x270 artwork is in assets.
- R96 native Nintendo Switch Pro decoder software regression passed.
- USB AGA full-screen debug overlay, UART and in-RAM debug logging
  remain disabled.
- GPLv2, MPL2, USPi GPLv3 (optional source only), original file-level
  UAE/WinUAE attribution and major vendor component licences supplied.
- Candidate zip contains only source and documentation, no ROMs,
  ADF/HDF images, firmware downloaded by the build or local build files.
- Re-extracted GitHub source candidate configured and built successfully
  on Studio Main with the documented flags. Clean-built kernel is
  4,771,872 bytes. The independent build does not match the
  development build byte-for-byte (different SHA); reproducible
  identical-binary builds have not been established.

## Still required before stable release

1. R101 physical cold-boot confirmation on Pi4B, including splash
   appearance, accurate live POST values and clean transition to AGS.
2. Functional keyboard / mouse / Switch Pro regression test after
   adding the boot POST; R101 has NOT been physically tested.
3. The cascaded Huasheng 214b:7260 hub can enumerate then drop out.
   Do not advertise generic external USB-hub support as complete.
4. A maintainer review of third-party attribution, donor
   provenance, firmware distribution and artwork rights before
   public binary releases.
5. A versioned GitHub repository/tag and release notes; this local
   source candidate has NOT been pushed to GitHub.

## Suggested release classification

Public *experimental Pi4 development preview* AFTER passing
the R101 hardware test, clearly marking the hub limitation.
NOT a stable v1.0 or production-quality universal Pi3/Pi4/Pi5 release.

## Build output (kept separate from GitHub source)

Kernel on Studio Main:
  /home/fff2025/Downloads/EMU68AGA_PI4B_R101_ORIGINAL_SPLASH_BOOT_POST/kernel8_R101_PI4_ORIGINAL_SPLASH_BOOT_POST.img
SHA256:
  dd80efbb3f8679bb08658af9d0666bd845c6d5c227d4c359310be28e326c1a34
