#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Reproducible host-side USB DWC2 bring-up tests for EMU68AGA Pi3.
# This DOES NOT replace hardware tests on the real Raspberry Pi.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$root/third_party/tinyusb/src"
I="$root/src/frf_usb/tinyusb"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

gcc -std=gnu17 -O2 -pthread \
  -DFRF_USB_TINYUSB=1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
  -I "$T" -I "$I" \
  "$root/tests/pi3_dwc2_hcd_model_test.c" \
  "$T/tusb.c" "$T/common/tusb_fifo.c" \
  "$T/host/usbh.c" "$T/host/hub.c" \
  "$root/src/frf_usb/tinyusb/frf_tinyusb_pad.c" \
  "$I/frf_r19_hid_descriptor.c" \
  "$root/src/frf_usb/tinyusb/frf_tinyusb_xpad_vendor.c" \
  "$root/src/frf_usb/tinyusb/frf_usb_packet_capture.c" \
  "$T/class/hid/hid_host.c" "$T/class/msc/msc_host.c" \
  "$T/portable/synopsys/dwc2/hcd_dwc2.c" \
  "$T/portable/synopsys/dwc2/dwc2_common.c" \
  -o "$out/pi3_dwc2_hcd_model"

for m in 0 1 2 3 5 6 7 8; do
  timeout 7s "$out/pi3_dwc2_hcd_model" "$m" > "$out/mode_$m.txt"
  if ! grep -q '^MODE '"$m"' .* PASS$' "$out/mode_$m.txt"; then
    echo "DWC2 model mode $m FAIL"
    cat "$out/mode_$m.txt"
    exit 1
  fi
  if [[ "$m" == 6 ]]; then
    grep -q '^SIMULATED ROOT ENUMERATION: PASS' "$out/mode_$m.txt"
  fi
  echo "DWC2 model mode $m PASS"
done

be_prefix="/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu"
be_cc="${BE_CC:-${be_prefix}-gcc}"
be_objcopy="${BE_OBJCOPY:-${be_prefix}-objcopy}"
"$be_cc" -std=c17 -O2 -ffreestanding \
    -DFRF_USB_TINYUSB=1 -Wno-error=scalar-storage-order \
    -I "$T" -I "$I" -c "$root/tests/frf_be_register_contract.c" \
    -o "$out/abi.o"
"$be_objcopy" -O binary --only-section=.frfbe "$out/abi.o" "$out/abi.bin"
python3 - "$out/abi.bin" <<'PY'
import pathlib,sys
b=pathlib.Path(sys.argv[1]).read_bytes()
assert bytes.fromhex("12 34 56 78") in b, "Host ABI not native BE"
assert bytes.fromhex("01 00 00 00") in b, "Port register bitfield not LE"
assert bytes.fromhex("80 00 00 00") in b, "Phy register bitfield not LE"
assert bytes.fromhex("00 00 00 02 00 00 00 02") in b, "Host descriptor ABI not BE"
print("AARCH64 BE host/LE DWC2 hardware register ABI PASS")
PY
echo "All software-only Pi3 DWC2 host tests PASS; real physical USB unverified."
