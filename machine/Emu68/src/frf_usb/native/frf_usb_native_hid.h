/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FRF_USB_NATIVE_HID_H
#define FRF_USB_NATIVE_HID_H
#include <stdint.h>
#include "frf_dwc2_pio.h"
struct frf_native_hid_ep {
 uint8_t ep, interface, protocol, interval, toggle;
 uint8_t device_address, hub_address, hub_port, low_speed;
 uint16_t max_packet;
 uint32_t next_poll_ms;
 uint32_t success, nak, faults;
};
struct frf_native_hid {
 uint8_t address, mps, configured, have_keyboard, have_mouse, root_device_class, root_hub_interface;
 uint8_t hub_address, hub_port, low_speed, root_hub_ports;
 uint32_t ports_seen, port_status, port_change;
 int32_t port_error;
 uint32_t elapsed_ms;
 struct frf_native_hid_ep keyboard, mouse;
 struct frf_dwc2_pio_stats stats;
};
int frf_native_hid_enumerate(const struct frf_dwc2_ops *o,
                             struct frf_native_hid *h, unsigned root_speed);
void frf_native_hid_poll(const struct frf_dwc2_ops *o,
                         struct frf_native_hid *h);
#endif
