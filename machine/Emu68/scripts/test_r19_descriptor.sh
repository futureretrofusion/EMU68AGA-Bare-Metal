#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
i="$root/src/frf_usb/tinyusb"
t="$root/tests/r19_descriptor_test.c"
o="$(mktemp -d)"
trap 'rm -rf "$o"' EXIT
gcc -O2 -Wall -Wextra -Werror -std=c17 -I "$i" "$t" "$i/frf_r19_hid_descriptor.c" -o "$o/desc_x86"
"$o/desc_x86"
be="${BE_CC:-/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc}"
"$be" -static -O2 -Wall -Wextra -Werror -std=c17 -I "$i" "$t" "$i/frf_r19_hid_descriptor.c" -o "$o/desc_be"
timeout 15s qemu-aarch64_be "$o/desc_be"
