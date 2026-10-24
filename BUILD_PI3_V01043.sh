#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cmake -S "$ROOT/machine/Emu68" -B "$ROOT/build-v01043" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake" \
  -DTARGET=raspi64 -DVARIANT=pistorm-classic -DAGA_PISTORM="$ROOT/machine" \
  -DAGA_PI3=ON -DAGA_DIAG=OFF -DAGA_PROBES=OFF \
  -DFRF_USB_HOST=ON -DFRF_USB_NATIVE_DWC2=ON -DFRF_USB_TINYUSB=ON \
  -DFRF_TINYUSB_MSC_WRITE=ON -DFRF_VIRTUAL_AGA=ON \
  -DFRF_USB_DIAG_HOLD=OFF -DFRF_USB_PASSIVE_TEST=OFF \
  -DFRF_NET_Z2_BRIDGE=OFF -DFRF_RTG_HANDOFF=OFF -DFRF_WIFI_SD1_ENUM=OFF
cmake --build "$ROOT/build-v01043" -j8
gzip -dc "$ROOT/build-v01043/Emu68.img.gz" > "$ROOT/kernel8.img"
mkdir -p "$ROOT/DEVS"
m68k-amigaos-gcc -Os -m68020 -fomit-frame-pointer -nostdlib \
  -Wl,-e,_doNotExecute -I "$ROOT/tools/usbmass" \
  "$ROOT/tools/usbmass/usbmass.device.c" -o "$ROOT/DEVS/usbmass.device"
sha256sum "$ROOT/kernel8.img" "$ROOT/DEVS/usbmass.device"
