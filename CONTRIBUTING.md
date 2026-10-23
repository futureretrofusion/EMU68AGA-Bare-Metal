# Contributing to EMU68AGA

Thanks for helping improve EMU68AGA.

## Reporting problems

Use the GitHub issue templates so reports include enough information to reproduce the problem.

For hardware-related bugs, the most useful reports include:

- exact EMU68AGA release/tag/commit;
- Raspberry Pi model;
- CaffeineOS/AGS/custom Amiga environment;
- connected USB devices;
- exact reproduction steps;
- what you expected;
- what actually happened;
- screenshots, logs or diagnostics where available.

For USB controller issues, include the controller name and USB VID:PID if known.

## Pull requests

Pull requests are welcome, but hardware behaviour is authoritative.

A successful build or host-side unit test is not enough to prove that a change is safe on the Raspberry Pi or inside the Amiga environment. Changes affecting USB, timing, reset handling, HDMI/video, SD/storage or chipset emulation should be tested on physical hardware whenever possible.

Keep pull requests focused. Avoid combining unrelated fixes.

Please include:

- the exact base commit or tag;
- what changed;
- why it changed;
- software test results;
- physical hardware test results;
- any known regressions or limitations.

## USB and joypad changes

USB/HID changes should document:

- VID:PID;
- descriptor/report-length assumptions;
- axis/button mappings;
- keyboard/mouse isolation;
- whether multiple controllers or hubs were tested.

## Compatibility reports

Game/demo/application compatibility reports are useful even when no code change is proposed. Include launch method, WHDLoad slave/version if relevant, graphics mode, sound/input behaviour and whether the result is repeatable.

## Release policy

Experimental changes should not be treated as release-ready until they have been verified on real hardware. The current public release remains the reference point unless a newer release has been explicitly marked hardware-verified.
