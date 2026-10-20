#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Compilation-only TinyUSB 0.21 host+DWC2 reference for EMU68AGA.
# Deliberately does not link kernel8: MMIO IRQ and clock runtime not ported.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/third_party/tinyusb/src"
cfg="$root/src/frf_usb/tinyusb"
out="$root/build-tinyusb-port-objects"
cc="/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc"
ar="/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-ar"
mkdir -p "$out"
sources=(tusb.c host/usbh.c host/hub.c class/hid/hid_host.c class/msc/msc_host.c portable/synopsys/dwc2/hcd_dwc2.c portable/synopsys/dwc2/dwc2_common.c)
objects=()
for f in "${sources[@]}"; do
  obj="$out/$(basename "$f").o"
  extra=(); if [[ "$f" == portable/synopsys/dwc2/* ]]; then extra=(-fsso-struct=little-endian); fi
  "$cc" -std=gnu17 -O2 -ffreestanding "${extra[@]}" \
    -DFRF_TINYUSB_COMPILE_PROBE=1 -I "$src" -I "$cfg" -c "$src/$f" -o "$obj"
  objects+=("$obj")
done
"$ar" rcs "$out/libtinyusb_port_compile_probe.a" "${objects[@]}"
echo "Compile-only TinyUSB host+hub+HID+MSC+DWC2: PASS"
echo "Runtime port: BLOCKED until big-endian adapter + real IRQ implementation is hardware tested"
