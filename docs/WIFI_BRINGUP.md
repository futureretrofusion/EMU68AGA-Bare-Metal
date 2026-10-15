# Wi-Fi bring-up for EMU68AGA Bare Metal (Pi 3A+)

## Goal

Keep the working USB-enabled v0.10.25 kernel, v0.10.22 boot files,
Kickstart, and the SD hard drive untouched while adding wireless networking.

The Pi 3A+ uses a **CYW43455** 2.4/5 GHz radio attached over **SDIO**.
The board's microSD boot/Amiga hard disk uses **SD0**; on-board Wi-Fi uses
the separate **SD1 Arasan host controller**. This is not USB Wi-Fi.

**Do not add `dtoverlay=sdio` to the known-good `config.txt`.**
It can reassign interfaces and break microSD access. Keep
`dtoverlay=sdhc` and the entire working v0.10.22 boot configuration.

## What exists on this branch

`machine/Emu68/src/frf_wifi/frf_sdio_probe.c` implements a controller-
independent, **read-only** SDIO card identification sequence. Host callbacks
handle CMD0, CMD5, CMD3, CMD7 and CMD52; the code checks voltage,
enumerates the card, reads CCCR metadata and has bounded timeouts.

`tests/test_sdio_probe.c` simulates the host; **five unit-test scenarios
pass on a normal Linux compiler**. This is not proof of a real Wi-Fi
connection: there is **no physical SD1 controller adapter yet**.
No code from this branch is automatically linked into the working kernel.

Local test:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  -Imachine/Emu68/src/frf_wifi \
  machine/Emu68/src/frf_wifi/frf_sdio_probe.c \
  tests/test_sdio_probe.c -o /tmp/test_sdio_probe
/tmp/test_sdio_probe
```

## Remaining work and hardware checkpoints

1. **SD1 host driver:** bind SD1 specifically (not SD0) using the Pi 3A+
   device tree; configure pinmux, power, clock and CMD response normalization.
   Report CMD5 OCR, two I/O functions, RCA and CCCR version via UART/screen.
   Preserve USB, HDMI and hard-disk operation.
2. **FullMAC bring-up:** implement CMD52/CMD53 data transfers and Wi-Fi
   backplane, identify CYW43455, load the matching Wi-Fi firmware and NVRAM
   calibration, check the regulatory configuration and obtain a MAC address.
   Do not publish or invent firmware redistributability permissions.
3. **WLAN association:** implement scanning, WPA2-Personal authentication
   and reconnect logic; verify association and DHCP, then ICMP/DNS/TCP.
   Handle all credentials privately, never as committed source or ZIP data.
4. **AmigaOS connectivity:** expose an Amiga-compatible network device,
   likely SANA-II or the target TCP/IP stack's supported interface, so
   Workbench applications can actually use Wi-Fi.
5. **Regression checks:** AGS boots, WHDLoad exits normally, USB HID keeps
   working, microSD RDB partitions remain accessible, and resets still work.

Avoid distributing a statically linked GPLv3 Circle WLAN implementation
inside the existing GPL-2.0-only AGA-PiStorm firmware. OpenBSD's
permissively licensed `bwfm` SDIO and FullMAC sources are a useful
reference, but still require careful porting and keeping copyright notices.

## Sources

- Pi 3A+ on-board radio: https://www.raspberrypi.com/documentation/computers/sbcs.html
- SD0/SD1 board wiring: https://www.raspberrypi.com/documentation/computers/raspberry-pi.html
- OpenBSD FullMAC: https://man.openbsd.org/bwfm.4
- OpenBSD SDIO source: https://github.com/openbsd/src/tree/master/sys/dev/sdmmc
- Circle WLAN overview (reference only, GPLv3): https://circle-rpi.readthedocs.io/en/44.4/subsystems/tcp-ip-networking.html
- Upstream firmware source/licence must be verified before any bundle.

**Status: SDIO protocol tests complete; physical WLAN detection and network
connectivity unimplemented. No public working Wi-Fi kernel exists yet.**
