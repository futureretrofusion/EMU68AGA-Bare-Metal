# EMU68AGA BARE METAL splash

The project artwork is now
[EMU68AGA_Bare_Metal_Splash.png](../assets/EMU68AGA_Bare_Metal_Splash.png).
The previous branded splash asset was removed.

The logo is compiled into the firmware: the source PNG is quantized to
480x270, 192-colour RGB565 with RLE and written to
`machine/Emu68/include/logo/logo_aga_rgb.h`.

Regenerate with:
```sh
python3 tools/build_bare_metal_logo.py
```

The startup text in `machine/Emu68/src/raspi/start_rpi64.c`
reads EMU68AGA BARE METAL.

A locally compiled private test build uses the v0.10.25 USB host and
v0.10.22 proven boot files, changing only kernel8.img. New splash
hardware boot testing has not yet been completed.

The public diagnostic image does not include USB input. The working
private USB host uses GPL-3.0-or-later USPi in a GPL-2.0-only chipset
build and cannot be published as a combined binary under those terms.
Consult `USB-LICENSING.md`.

Some Raspberry Pi hardware names like `bcm2710-rpi-3-b.dtb` are not
branding: these must retain their original names and references so the
boot firmware can locate them.
