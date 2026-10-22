#!/usr/bin/env bash
set -euo pipefail
r="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
t="$r/third_party/tinyusb/src"
i="$r/src/frf_usb/tinyusb"
cc=/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc
"$cc" -std=gnu17 -O2 -static -pthread -DFRF_USB_TINYUSB=1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-error=scalar-storage-order -I "$t" -I "$i" "$r/tests/pi3_dwc2_hcd_model_test.c" "$t/tusb.c" "$t/common/tusb_fifo.c" "$t/host/usbh.c" "$t/host/hub.c" "$r/src/frf_usb/tinyusb/frf_tinyusb_pad.c" "$r/src/frf_usb/tinyusb/frf_tinyusb_xpad_vendor.c" "$t/class/hid/hid_host.c" "$t/class/msc/msc_host.c" "$t/portable/synopsys/dwc2/hcd_dwc2.c" "$t/portable/synopsys/dwc2/dwc2_common.c" -o /tmp/frf_pi3_aarch64_be_model 2>/tmp/frf_be_model_build.log
for m in 0 1 2 3 5 6 7 8; do
 echo "=== Big-endian ARM mode $m ==="
 timeout 11s qemu-aarch64_be /tmp/frf_pi3_aarch64_be_model "$m" | tail -7
done
