#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
t="$root/third_party/tinyusb/src"
i="$root/src/frf_usb/tinyusb"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
src="$root/tests/dyonder_hid_mount_test.c"
pad="$i/frf_tinyusb_pad.c"
gcc -std=gnu17 -O2 -Wall -Wextra -I "$t" -I "$i" "$src" "$pad" "$i/frf_r19_hid_descriptor.c" "$i/frf_r20_hardware_profile.c" -o "$out/test_x86"
"$out/test_x86"
be="${BE_CC:-/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc}"
"$be" -std=gnu17 -O2 -static -Wall -Wextra -I "$t" -I "$i" "$src" "$pad" "$i/frf_r19_hid_descriptor.c" "$i/frf_r20_hardware_profile.c" -o "$out/test_be"
timeout 12s qemu-aarch64_be "$out/test_be"
echo "DYONDER x86 and ARM64-BE mount/decoder/retry unit tests PASS"
