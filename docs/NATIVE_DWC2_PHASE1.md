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

## Keyboard-first follow-up

Added a separate, tested USB HID boot keyboard report decoder and an actual CIA-A raw-key output adapter. Handles make/break, modifiers, rollover, and USB HID usage translation. **No keyboard transport is implemented yet**, so this function is not called by hardware. To obtain real keys requires enumerating the Pi 3 USB hub and scheduling HID interrupt-IN transactions (with split transactions for full/low-speed devices). Research identified TinyUSB MIT host stack with DWC2 hub/split support. Reference checkout: `~/Downloads/FRF_TINYUSB_REFERENCE` (upstream commit 2f675ab). Integration and target support are not yet proven.

## Distribution release gate (20 October 2026)
`FRF_DISTRIBUTION_BUILD=ON` deliberately stops CMake configuration until
independent USB enumeration, HID input and storage are implemented and
hardware validated. Setting `FRF_USB_NATIVE_DWC2=ON` by itself produces
a **diagnostic-only** image. The USB keyboard bridge test still links USPi.
Neither has been verified as an independently licensed, functional USB
distribution image. Do not remove this guard to advertise a release.
TinyUSB reference is upstream MIT licensed, but its Pi 3 standalone
EMU68AGA port is **not implemented**.

## Proven input scheduling requirement
Hardware testing: separating keyboard reads from gamepad reads eliminated typing lag and repeated keys. The replacement host driver MUST dispatch keyboard interrupt-IN completions without waiting for gamepad polling, decoding, retries, or mass storage. Keep bounded separate queues for keyboard, mouse and gamepad. Regression: rapid typing in AmigaOS while holding gamepad buttons must show no lag, repeat or missing release.

## New confirmed symptom: keyboard lag accumulates over time
User retracted prior claim that isolated joypad polling cured typing. Source inspection: machine/Emu68/src/aarch64/vectors.c has 64-entry frf_kbd_q, sends each event only after prior keyboard SP handshake and contains no timeout for frf_kbd_wait_ack. A stalled/missed guest handshake can leave a growing queue with drop-oldest overflow. This is an identified potential cause, NOT a proven root cause. Before release instrument queue occupancy/high water/drop count/ACK age, validate guest handshake and avoid an unchecked indefinite stall. Replacing DWC2 alone will not fix this CIA-side issue. Do not mark distribution ready until sustained typing test passes.

## Native USB host development status (20 October 2026)
The native-only DWC2 build now includes a bounded polling PIO packet engine, a root-port control transfer sequence, device/configuration descriptors, SET_ADDRESS, SET_CONFIGURATION, and Boot HID keyboard/mouse interrupt polling, with keyboard events feeding virtual CIA. This path is for directly connected USB HID devices; it does NOT yet enumerate high-speed hubs or perform translator split transactions required for most Pi3B USB ports. No native gamepads, XInput, hotplug or mass-storage BOT yet. Never publish as a complete distro. Linux mock control-transfer and HID translation tests PASS; Pi3 BE native firmware compiles and links but has not been physically validated. Virtual CIA keyboard now supports optional 250ms lost-ACK recovery (-DFRF_KBD_ACK_RECOVERY=ON) plus high-water, drop, ACK and timeout counters; also unverified on hardware. Previous diagnostic-only description above is historical. Distribution guard deliberately rejects release builds.

## Hardware-test failure and source corrections — 21 October 2026
User reports **no USB devices working at all** with the native DWC2 firmware.
Treat that hardware test as authoritative. Earlier mock tests and successful
compilation did not validate USB operation. The native USB build is withdrawn
from distribution consideration.

Confirmed source defects corrected on this branch:
- DWC2 register base used physical `0x3F980000`, but EMU68Aga uses
  virtual peripheral base `0xF2000000` so native DWC2 must use
  `0xF2980000`; the passive probe was corrected too.
- Native driver omitted the Pi firmware mailbox USB-HCD power-on request
  (VC device 3). A bounded standalone mailbox request is now present.
- Root-port writes previously risked disabling an enabled port because
  HPRT write-one-clear masking omitted the enable and suspend bits.
- Native PIO driver lacked receive FIFO interrupt/status unmasking and
  explicit DMA-off mode. Both have been updated.
- Native boot POST now displays controller stage, return code, register
  detail and auxiliary value on HDMI, with no serial dependency.

**None of these corrections has been rebuilt or hardware tested** because
Studio Main tunnel disconnected before the corrections were made. No new
`kernel8.img` was produced or validated. Rebuild with the pre-existing Pi3
native CMake configuration and inspect HDMI boot POST before making claims.
The normal Pi3B USB path goes through a high-speed internal hub, for which
this independent USB driver still lacks hub enumeration and split
transactions. HID, gamepads and mass storage remain blocked behind it.
The distribution-build safety gate must remain enabled.
