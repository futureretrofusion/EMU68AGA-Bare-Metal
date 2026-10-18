# Option B: independent DWC2 USB host (Phase 1)

Status: **experimental controller-only diagnostic, not production USB**.

## Provenance and licence

New code in `machine/Emu68/src/frf_usb/native/` was authored for Future Retro Fusion
under **SPDX-License-Identifier: GPL-2.0-only**. It uses generic DWC2 register
offsets/bitfield semantics from published hardware-facing materials, not
the USPi library or its implementation. The register-only module uses callback
MMIO so it can be tested without Pi hardware. External reading context:
the Synopsys DWC2 programming model, Raspberry Pi BCM2837 address map and
generic DWC2 register documentation. Engineering and copyright review still
required before making a release-level licensing claim.

**This does not yet remove the copy of USPi from the repository.**
The old backend remains available and is still the default to preserve
functional devices during the migration. In the native configuration the
USPi sources are **not compiled or linked**.

## Code now implemented

- Read-only core identification (GSNPSID, GHWCFG2, GINTSTS, HPRT).
- Exclusive host-mode forcing and bounded core reset; PHY clock ungating.
- Polling-only root-port power and reset with W1C preservation.
- Pi 3 AArch64 big-endian MMIO adapter; CPU1 gate integration.
- Read-only boot diagnostic output.
- Native mode does not publish fake USB input or storage devices.

**Not implemented:** USB setup/control transactions, endpoint packet I/O,
DMA, hub descriptor/port enumeration, split transactions, HID keyboard/mouse
and gamepad decoding, bulk-only mass storage, safe FAT/RDB data access,
hotplug, disconnect recovery or interrupts. A high-speed LAN9514 hub stands
between the Pi 3 DWC2 root port and most external USB devices. All these
operations are necessary before the native backend can replace USPi.

## Build a separate diagnostic image (do not overwrite your tested SD image)

```sh
r=/path/to/EMU68AGA_NATIVE_DWC2_PHASE1
cmake -S "$r/machine/Emu68" -B "$r/build-native-pi3" \
  -DCMAKE_TOOLCHAIN_FILE="$r/machine/Emu68/toolchains/aarch64_be-arm-gnu.cmake" \
  -DVARIANT=pistorm-classic -DAGA_PISTORM="$r/machine" \
  -DAGA_PI3=ON -DFRF_VIRTUAL_AGA=ON -DFRF_USB_HOST=ON \
  -DFRF_USB_NATIVE_DWC2=ON -DEMU68_SKIP_PPC_ROM=ON \
  -DEMU68_SKIP_FIRMWARE=ON
cmake --build "$r/build-native-pi3" --target Emu68.elf -j8
```

Firmware output: `build-native-pi3/Emu68.img.gz` (compressed image). Use on a **Raspberry Pi 3**
only, as a separate, reversible diagnostic test. Do not try on Pi 4: USB there
typically uses xHCI and this fixed Pi 3 MMIO base is not appropriate.

Expected serial diagnostics: `[FRF-NATIVE-USB] Pi3 DWC2 probe ...`, followed
by `exclusive host init`, `root port`, and finally `Phase 1 only`.
`frf_usb_boot_ready` indicates the diagnostic finished, **not** that
USB keyboard/mouse/storage is operational. Use a non-USB means of rebooting.

## Unit test

```sh
cd machine/Emu68
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -o /tmp/native-dwc2-tests tests/native_dwc2_test.c \
  src/frf_usb/native/frf_dwc2.c
/tmp/native-dwc2-tests
```

It tests identity rejection, read-only probe, bounded host initialisation,
root reset, safe W1C handling, absent-root-port behaviour. Does not certify
hardware timing or USB functionality.

## Next milestones in order

1. Pi 3 hardware trace: confirm GSNPSID, mode, port speed/enable and timing.
2. Add generic USB request/endpoint abstractions and DWC2 channel transfers,
   correct big-endian descriptor representation, physical DMA address
   translation and cache maintenance.
3. Enumerate the Pi 3 high-speed upstream hub; reset/enable downstream ports.
4. Split transactions for full/low-speed USB devices behind high-speed hubs.
5. HID keyboard and mouse report support, then gamepads including XInput.
6. Mass-storage BOT/SCSI with safe read/write, disconnect and errors.
7. Validate against the existing tested devices, only then flip default and
   remove USPi-dependent source, includes, licences and packaged artefacts.

No functionality is attributed to a milestone until it is demonstrated
on physical hardware.
