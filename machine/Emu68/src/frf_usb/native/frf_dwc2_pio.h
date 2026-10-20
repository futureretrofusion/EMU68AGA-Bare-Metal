/* SPDX-License-Identifier: GPL-2.0-only
 * Native DWC2 PIO channel transport, no USPi dependencies.
 * Control transfers on the root link only; hub splits require later support.
 */
#ifndef FRF_DWC2_PIO_H
#define FRF_DWC2_PIO_H
#include <stdint.h>
#include "frf_dwc2.h"
enum { FRF_USB_PID_DATA0=0, FRF_USB_PID_DATA1=2, FRF_USB_PID_SETUP=3 };
struct frf_dwc2_packet {
    uint8_t address, endpoint, in, type, low_speed, pid;
    uint16_t max_packet, length, received;
    uint8_t *buffer;
    /* Zero = root port direct, otherwise high-speed hub TT route. */
    uint8_t hub_address, hub_port;
};
struct frf_dwc2_pio_stats {
    uint32_t attempts, completed, timeouts, naks, stalls, errors, rx_bytes;
    uint32_t last_channel_interrupt, last_global_interrupt;
};
int frf_dwc2_pio_init(const struct frf_dwc2_ops *o);
int frf_dwc2_pio_packet(const struct frf_dwc2_ops *o,
                       struct frf_dwc2_packet *p,
                       struct frf_dwc2_pio_stats *stats);
int frf_dwc2_pio_get_descriptor(const struct frf_dwc2_ops *o,
                                 uint8_t address, uint8_t max_packet,
                                 uint8_t type, uint8_t index,
                                 uint8_t *buffer, uint16_t size,
                                 struct frf_dwc2_pio_stats *stats);
#endif
