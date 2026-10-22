#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
t="$root/tests/r23_keyboard_guard_test.c"
k="$root/src/frf_usb/native/frf_native_keyboard.c"
gcc -O2 -Wall -Wextra -Werror -std=c17 "$t" "$k" -o "$out/kbd_x86"
"$out/kbd_x86"
be="${BE_CC:-/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc}"
"$be" -static -O2 -Wall -Wextra -Werror -std=c17 "$t" "$k" -o "$out/kbd_be"
timeout 15s qemu-aarch64_be "$out/kbd_be"
echo "R23 keyboard/gamepad firewall regression PASS x86+ARM64-BE"
