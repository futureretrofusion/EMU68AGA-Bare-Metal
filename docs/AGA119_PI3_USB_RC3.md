# AGA-PiStorm 1.119 selective Pi 3 port on GitHub v0.10.43 USB

**Status:** RC3 cross-compiled, **not yet hardware tested**. Do not modify the published `v0.10.43` tag or deploy over a known-working kernel without rollback.

## Provenance

- GitHub repository: `futureretrofusion/EMU68AGA-Bare-Metal`
- Published release: `v0.10.43`, tag commit `e723b2dd572c5b9e4f08957b1c909975104e2992`
- Integration base: `release-v0.10.43`, commit `8f1ff1e2bb6be0c014dd317d87a31785eae3b198`
- 1.119 source: `AGA-Pistorm_universal_v1.119.7z`
- Source modifications are limited to `machine/arm/aga_hvs.c`, `machine/arm/aga_glue.h`, `machine/Emu68/src/aarch64/vectors.c`, and the **test banner only** in `machine/Emu68/src/raspi/start_rpi64.c`.

The original v0.10.43 USB backend, DWC2/TinyUSB, multi-button input, Amiga `usbmass.device`, floppy, virtual AGA and physical RGB paths are **not replaced**.

## What was ported from 1.119

- 1% scanline intensity for ordinary, woven interlaced, and independently-configured NTSC output.
- 1.119 `0x50xx` select and `0x53xx` value protocol.
- HVS-only CRT beam scanlines (shape 4) and top/bottom scanline position, using Pi 3-compatible display hardware.
- Legacy 25% controls and old scanline shapes 0/1/2 remain.

**Not included:** three-page frame blending, fill/beam-fill shapes 3/5, CRT fields mode, Pi 4 GPU curve/glow/afterglow, HDMI PC/TV setup installer changes, standalone HDF/ADF preboot selector. Unsupported selectors are ignored.

## Rebuild

Configure the checked-out source as in `BUILD_PI3_V01043.sh`, retaining:

```text
TARGET=raspi64
VARIANT=pistorm-classic
AGA_PI3=ON
FRF_VIRTUAL_AGA=ON
FRF_USB_HOST=ON
FRF_USB_NATIVE_DWC2=ON
FRF_USB_TINYUSB=ON
FRF_TINYUSB_MSC_WRITE=ON
FRF_USB_DIAG_HOLD=OFF
FRF_USB_PASSIVE_TEST=OFF
FRF_NET_Z2_BRIDGE=OFF
FRF_RTG_HANDOFF=OFF
FRF_WIFI_SD1_ENUM=OFF
```

Compiler: AArch64 big-endian Arm GNU 15.3. The test build completed to 100%, with standard linker RWX load-segment warning.

## USB integrity audit

The **published** v0.10.43 GitHub release asset was downloaded and compared with local copies.

- Published original `kernel8.img` SHA-256: `7ae412dd01b0fa416bb1af06fd94248d5d9ef823e42f8a5a181a7c5601333de8`
- Published original `usbmass.device` SHA-256: `e95989edd12976cc0a54be482ca65f78333c35a6ce30cb4edce5b0bbbc97d707`

The RC3 kernel is a **new build**, not byte-identical to the released kernel. It was linked with `FRF_USB_NATIVE_DWC2=1`, `FRF_USB_TINYUSB=1`, `FRF_TINYUSB_MSC_WRITE=1`; the executable contains `hcd_init`, `tuh_msc_read10`, and `tuh_msc_write10`. These checks prove code inclusion, **not** hardware functionality.

## Hardware acceptance before merging into release branch

1. Back up the original SD `kernel8.img` and retain a working boot card. Test RC3 from a spare/test boot setup.
2. Verify the RC3 POST banner and stable startup, HDMI, actual physical RGB/HAM6, normal AGA and RTG screen transitions.
3. Verify keyboard, mouse, all previously working gamepad buttons, and boot timing.
4. Test native USB storage enumeration, correct partition geometry, and PFS3 **read-only first**.
5. Only with expendable data, test USB PFS3 writes and readback using the published matching `usbmass.device` v0.2.
6. Check scanline shapes 0/1/2/4, legacy intensity steps, fine 1% controls, NTSC, PAL and interlace. Scanlines must default off.
7. Roll back the original kernel immediately if USB or video regress.

**No live Amiga SD card, published release, or production kernel was modified in preparing this branch.**
