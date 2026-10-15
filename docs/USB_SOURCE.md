# FRF USB integration: source-only publication

The three files in `machine/Emu68/src/frf_usb/` are the FRF-maintained
integration layer from the **v0.10.25 local USB-enabled test kernel**:

- `frf_usb.c` — USB HID keyboard/mouse/gamepad state mapping into the virtual Amiga.
- `frf_usb.h` — input events/interface.
- `frf_uspi_env.c` — Pi 3 CPU1 host environment, DMA addressing,
  timer callbacks, USB IRQ adapter and VideoCore power mailbox.

The source is shared to support inspection and independent work. These
files **are not themselves a complete USB host implementation**.
The GPL-3.0-or-later **USPi library is not bundled**; the 3 files are
built against its external API and need it for USB-enabled local builds.

The hardware-tested *local-only* v0.10.25 binary statically links
USPi with the GPL-2.0-only AGA-PiStorm chipset, a licence conflict for
public distribution. As a result, do not build and distribute a combined
firmware binary using this source without resolving the conflict.

`FRF_USB_HOST=OFF` remains the public build default. Source availability
is not a declaration of licence compatibility for a combined binary.

The experimental clean-room USB state-machine prototype is a separate
unreleased effort; it is **not** included here and has not replaced USPi.
For user-observed boot behaviour and boot file checksums see
`docs/BOOT_COMBINATION_v0.10.25.md`.
