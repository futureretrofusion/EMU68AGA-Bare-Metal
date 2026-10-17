# Build EMU68AGA Version 0.1-Alpha

Build on Linux with the AArch64 big-endian GNU toolchain and CMake. From the repository root:

```sh
cmake -S machine/Emu68 -B build-pi3 -DCMAKE_TOOLCHAIN_FILE="$PWD/machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake" -DTARGET=raspi64 -DVARIANT=pistorm-classic -DAGA_PISTORM="$PWD/machine" -DFRF_USB_HOST=ON -DFRF_VIRTUAL_AGA=ON -DFRF_RTG_HANDOFF=ON -DFRF_NET_Z2_BRIDGE=ON -DFRF_WIFI_SD1_ENUM=ON -DAGA_PI3=ON -DAGA_DIAG=OFF -DAGA_PROBES=OFF
cmake --build build-pi3 -j8
gzip -dc build-pi3/Emu68.img.gz > kernel8.img
```

Experimental reference. Toolchain, ROM and dependencies are not included.
