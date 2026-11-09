# Third-party notices and acknowledgements

Future Retro Fusion claims authorship only over its own contributions.

## Emu68

Emu68 is by **Michal Schulz and contributors**.

Upstream: https://github.com/michalsc/Emu68

The Emu68 source in this project retains its MPL-2.0 notices and license.

## AGA-PiStorm

AGA-PiStorm by **Astair86 and contributors** is the primary virtual AGA donor used by this project.

Project page: https://astair86.itch.io/aga-pistorm-10-experimental-prototype

Its original license/copyright notices are preserved.

## PiStorm

PiStorm was created by **Claude Schwarz (`captain-amygdala`)** and developed by many contributors. PiStorm-facing support remains part of the lineage of this project.

## UAE / WinUAE

The AGA-PiStorm source distribution credits chipset code derived/adapted from UAE/WinUAE. Its attribution notice is retained at `machine/package/UAE-WinUAE-notice.txt`.

Important retained files include `machine/LICENSE`, `machine/COPYING`, `machine/MPL-2.0.txt`, `machine/Emu68/LICENSE`, and source-level copyright/license headers.

FRF does not claim ownership of these upstream works.

## AGA-Storm v1.121 contributors (2026 integration)

The FRF v1.121 integration is derived from **AGA-Storm**, previously
**AGA-PiStorm**, by **Astair86** and contributors. The project is not an
independent reimplementation of the original AGA chipset.

Upstream v1.121 expressly acknowledges the following contributors and
sources. These credits are retained and extended here:

- **UAE / WinUAE:** Bernd Schmidt, Alessandro Bissacco, Toni Wilen and
  the other UAE authors for blitter, Copper, playfield, sprite, collision
  and custom-register foundations. **Richard Drummond** for E-UAE.
- **Emu68:** Michal Schulz and contributors CrypticalCode0, MBeijer,
  mras0, rondoval, captain-amygdala, ronaegis, apolkosnik, mdpye and
  TomCat666. Emu68's source files carry MPL-2.0 notices.
- **PiStorm:** Claude Schwarz (captain-amygdala), plus the PiStorm/PiStorm600
  communities and **LemaruX**.
- **WHDLoad:** Wepl, whose kick31.asm was the basis for upstream
  whdslave examples.
- **AGA-Storm testing:** katzazakis, FraBro and Angelclaww.
- **USB assistance credited by upstream:** Lemaru, for dwc2usb.device
  and work helping enable Pi 3 USB pad support. The FRF RC8 binary does
  **not** compile PiStormAGA's USB drivers.
- **FRF testing:** Gennaro gtv3000 for kernel and joypad testing.
- **TinyUSB:** Ha Thach and contributors; MIT-licensed upstream USB
  host sources used in FRF's independent DWC2 implementation.

Source references:
- AGA-Storm upstream v1.121 README and bundled Credits/Licenses
- https://github.com/michalsc/Emu68
- https://github.com/tonioni/WinUAE
- https://github.com/hathach/tinyusb

The AGA-Storm chipset source retains its original GPL-2.0-only headers;
Emu68 remains under its file-level MPL-2.0 terms and TinyUSB under MIT.
Any required copying of source, notices and licences must accompany
redistribution. Future Retro Fusion claims only its own modifications.
