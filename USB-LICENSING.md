# USB prototype licensing status

The local hardware-test branch currently contains an experimental bare-metal USB host/HID implementation based on **USPi**.

USPi is GPL-3.0-or-later. The AGA-PiStorm donor tree carries licensing terms that make publishing that statically linked combination something we should not do casually.

For that reason the USPi-derived subtree and FRF USB glue are intentionally omitted from the public main branch for now.

Public builds should use `FRF_USB_HOST=OFF`.

The USB implementation can be published later after replacing it with a license-compatible implementation, separating it appropriately, or obtaining compatible relicensing permission.
