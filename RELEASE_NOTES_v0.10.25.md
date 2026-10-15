# EMU68AGA BARE METAL v0.10.25 — local hardware confirmation

Reported hardware result, 15 October 2026: the USB-enabled v0.10.25
kernel boots using **the boot files from v0.10.22**. Later repackaged
boot files should **not** be used as the baseline.

The locally tested, privately retained kernel image has SHA-256:

```
138bb918681fe2072c783484b11da9a3e4f50889305c0214aefc349d7c10fc1f  kernel8.img
```

## What this confirms

- The current local USB-enabled kernel can boot in the owner's configuration.
- The USB integration is based on *USPi*, not a completed clean-room USB host.
- The v0.10.25 source removes v0.10.16 guest Exec task-list scanning and
  v0.10.19 hardware-trace atomic operations from performance-sensitive paths.
- Broader WHDLoad, Carmageddon, USB gamepad and hotplug compatibility are not
  implied by successful boot.

## Exact boot-file requirements

See [the boot combination guide](docs/BOOT_COMBINATION_v0.10.25.md).
The public v0.10.25 support archive deliberately omits the combined
USB kernel, Kickstart ROM and commercial software. It is not bootable alone.

## Public binary hold

The upstream AGA-PiStorm core is GPL-2.0-only; USPi is GPL-3.0-or-later.
The local kernel statically links both, so the working combined binary
cannot presently be redistributed as a public release under the current
licences. See [USB-LICENSING.md](USB-LICENSING.md).

**No USB-enabled GitHub binary release has been published.**
