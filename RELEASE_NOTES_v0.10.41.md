# EMU68AGA Bare Metal v0.10.41

AGA-PiStorm 1.118 release based directly on the hardware-verified v0.10.40a R40a USB endian-fix lineage.

## What changed
- Updated AGA/PiStorm video, HDMI/HVS, timing, audio and RTG handoff code to the v1.118 integration.
- Preserved the v0.10.40a R40a TinyUSB big-endian USB fix and its one-button-era controller behavior.
- Disabled the large R18/R41 onscreen diagnostic panels at POST.
- Kept the compact SYSTEM POST information box.
- POST version line now reads exactly: `v0.10.41 / AGA 1.118`.
- AGA_DIAG and AGA_PROBES are OFF in the release build.

## Validation
- Pi3 bare-metal ARM64-BE build: PASS.
- R40a raw USB endian source regression: PASS.
- 057E:2009 / HID gamepad regression: PASS.
- PAL frame measurement: PASS at 71051 colour clocks.
- NTSC frame measurement: PASS at 59832/59833 colour clocks.
- HDMI frame-length remeasurement: PASS.
- Large POST diagnostic panel strings absent from the final ELF: PASS.
- Compact release/version POST string present: PASS.

## Controller scope
This release intentionally preserves the v0.10.40a / R40a input lineage. It does not include the later RC8/RC9 independent multi-button experiments.

## Hardware status
The R40a USB endian/input base was physically verified. The combined AGA 1.118 update has passed host/build regressions; full physical AGA1.118 revalidation is still recommended.
