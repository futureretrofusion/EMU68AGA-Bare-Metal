# EMU68AGA Bare Metal — Pi4 R101 experimental source candidate

## Status

Experimental. R101 cross-compiles successfully but has not yet been
booted on Raspberry Pi 4B. The original 480x270 circuit-board
EMU68AGA BARE METAL splash (192-colour RGB565/RLE) is unchanged,
with a boot-only compact POST display showing live USB device counts.
The Pi3-style large live debug HUD, serial logging and AGA debug log
remain disabled.

R96 was confirmed with Nintendo Switch Pro gamepad input on Pi4.
Cascaded Huasheng 214b:7260 USB2 hubs enumerate inconsistently and
may drop out. General hub hotplug, USB audio and hub mass storage
are not supported by this build. Use R96 as the stable rollback.

## Build (tested configuration)

Requires cmake, native compiler and Arm GNU Toolchain 15.3.rel2
AArch64 BIG-ENDIAN cross-compiler. The firmware-fetch part of the
CMake build requires internet access. From source release root:

~~~sh
cmake -S machine/Emu68 -B build   -DVARIANT=pistorm-classic -DTARGET=raspi64   -DCMAKE_TOOLCHAIN_FILE="$PWD/machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake"   -DAGA_PISTORM="$PWD/machine"   -DFRF_PI4_XHCI_HID=ON -DFRF_VIRTUAL_AGA=ON   -DFRF_USB_HOST=OFF -DFRF_RTG_HANDOFF=ON   -DFRF_NET_Z2_BRIDGE=ON -DAGA_PI3=OFF   -DAGA_DIAG=OFF -DAGA_PROBES=OFF   -DFRF_RELEASE_QUIET=ON -DFRF_BOOT_POST=ON   -DCMAKE_BUILD_TYPE=Release   '-DCMAKE_C_FLAGS=-DNDEBUG=1 -D_FORTIFY_SOURCE=3 -DFRF_PI4_R75_NO_Z3_RAM=1 -DFRF_PI4_NATIVE_USB_GUEST_PCIE_OFF=1'   '-DCMAKE_CXX_FLAGS=-DNDEBUG=1 -D_FORTIFY_SOURCE=3 -DFRF_PI4_R75_NO_Z3_RAM=1 -DFRF_PI4_NATIVE_USB_GUEST_PCIE_OFF=1'
cmake --build build -j8
gzip -dc build/Emu68.img.gz > kernel8_R101_PI4_ORIGINAL_SPLASH_BOOT_POST.img
~~~

## Installation

Back up the Pi4 SD boot FAT partition configuration and current
kernel. Copy the new kernel to the FAT volume, select it in the
[pi4] kernel= stanza only, and leave [pi3], [all] and Amiga RDB
partitions unchanged. First cold-boot should be observed on
hardware before calling R101 a public release.

## Licensing

See machine/LICENSE, machine/COPYING (GPL-2.0-only AGA core),
machine/MPL-2.0.txt (MPL-2.0 Emu68),
machine/package/UAE-WinUAE-notice.txt and
THIRD_PARTY_NOTICES.md. USB Pi4 backend does NOT link legacy USPi.
No Amiga Kickstart ROMs, Workbench or commercial software are
included in the source package. Provide lawfully obtained media.
