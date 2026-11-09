# Build FRF EMU68AGA v1.121 RC8 — Pi 3 bare metal

The checked-in `machine/Emu68`, `machine/arm` and `machine/core`
are the directly compiled sources of this experimental RC8 kernel.

The v1.120 and v1.121 upstream source distributions were compared
against a shared Emu68 1.106 base before FRF's three-way source merge.
The legacy `machine/arm/emu68-aga.patch` is a historical reference;
for RC8 build the **in-tree sources**, not that patch.

## Required components

- AArch64 **big-endian** GCC toolchain, with the supplied
  `machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake`
- Emu68's normal build prerequisites, including PPC ROM build
  dependencies and Raspberry Pi firmware files
- TinyUSB `src` and MIT notice under
  `machine/Emu68/third_party/tinyusb/`

## Configure and build

From the repository root (replace `$PWD` with an absolute path if needed):

```sh
cmake -S machine/Emu68 -B build-rc8 \
 -DCMAKE_TOOLCHAIN_FILE="$PWD/machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake" \
 -DVARIANT=pistorm-classic -DAGA_PI3=ON \
 -DAGA_PISTORM="$PWD/machine" -DFRF_VIRTUAL_AGA=ON \
 -DFRF_USB_HOST=ON -DFRF_USB_NATIVE_DWC2=ON \
 -DFRF_USB_TINYUSB=ON -DFRF_TINYUSB_MSC_WRITE=ON \
 -DFRF_DISTRIBUTION_BUILD=ON
cmake --build build-rc8 -j8
gzip -dc build-rc8/Emu68.img.gz > kernel8.img
```

No USB additions from PiStormAGA are compiled; FRF's existing
TinyUSB/DWC2 stack remains the only enabled USB host.

The release binary was built in the separate integration tree with
these options using the same merged source. It is hardware **untested
for the OSD repair**.

## Expected code paths

- HID key recognition:
  `machine/Emu68/src/frf_usb/native/frf_native_keyboard.c`
- TinyUSB USB host:
  `machine/Emu68/src/frf_usb/tinyusb/`
- Guest CPU keyboard / OSD handoff:
  `machine/Emu68/src/aarch64/vectors.c`
- OSD keyboard-active state, drawing and frame composition:
  `machine/arm/aga_hvs.c`, `machine/arm/aga_glue.h`
- Boot identifier:
  `machine/Emu68/src/raspi/start_rpi64.c`

Full upstream attribution: `THIRD_PARTY_NOTICES.md`.
