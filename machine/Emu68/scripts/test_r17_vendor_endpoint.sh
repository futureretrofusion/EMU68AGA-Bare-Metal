#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
t="$root/third_party/tinyusb/src"
i="$root/src/frf_usb/tinyusb"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
src="$root/tests/r17_vendor_endpoint_test.c"
pad="$i/frf_tinyusb_xpad_vendor.c"
gcc -std=gnu17 -O2 -Wall -Wextra -I "$t" -I "$i" "$src" "$pad" -o "$out/ep_x86"
"$out/ep_x86"
be="${BE_CC:-/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc}"
"$be" -std=gnu17 -O2 -static -I "$t" -I "$i" "$src" "$pad" -o "$out/ep_be"
timeout 15s qemu-aarch64_be "$out/ep_be"
echo "R17 vendor interface BULK/INT endpoint class-driver tests PASS"
