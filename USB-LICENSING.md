# USB redistribution status (15 October 2026)

The *local*, hardware-tested v0.10.25 kernel uses the original USPi-based
USB implementation and v0.10.24 CPU1 startup synchronization. It is **not**
a completed independent DWC2 USB driver.

## Why the kernel is not a public release

- AGA-PiStorm's source tree declares the virtual chipset core under
  **GPL-2.0-only** (see `machine/LICENSE`, `machine/arm/aga_glue.c`,
  `machine/core/src/custom.c`).
- The used USPi library declares **GPL-3.0-or-later**
  (USPi upstream, R. Stange, licence headers in `uspios.h`).
- The local firmware compiles both into **one statically linked kernel**.
- GPL-2.0-only and GPL-3.0-or-later are not compatible for publicly
  conveying that combined work under these licences.

FSF FAQ: https://www.gnu.org/licenses/gpl-faq.html#v2v3Compatibility

The ability to use this combination **privately** does not grant
permission to publish the combined executable. This is not an objection
to USPi being open source: it is a conflict between the licence
requirements of **two different open-source components**.

No claim that an output is 'licence-compliant' should be made merely
by changing a NOTICE file, excluding ROM files, or calling the result
a hobby build.

## Routes to a public USB-enabled release

1. Implement and test an independently authored USB host solution under
   a licence compatible with AGA-PiStorm's GPL-2.0-only terms.
2. Obtain sufficient copyright-holder permission or licence exceptions
   to distribute the combined USPi + chipset kernel.

Until one of those is completed, do not push the working combined kernel
or a ZIP containing it to GitHub Releases. The FRF integration source is published separately in
`machine/Emu68/src/frf_usb/`; the USPi library source itself is not
bundled, and its presence as a separate dependency would not legalize
redistribution of a statically linked combined kernel. The public support package
contains *only* the original v0.10.22 boot support files and excludes
the kernel, Kickstart, AGS and commercial game content.

Public-source builds should retain `FRF_USB_HOST=OFF` by default.
