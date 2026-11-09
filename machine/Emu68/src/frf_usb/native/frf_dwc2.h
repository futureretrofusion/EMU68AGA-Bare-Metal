/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Future Retro Fusion.
 * Independent register-level DWC2 USB2 host controller bring-up.
 * No USPi implementation or headers.
 */
#ifndef FRF_DWC2_H
#define FRF_DWC2_H
#include <stdint.h>
struct frf_dwc2_ops {
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void (*delay_us)(void *ctx, unsigned duration);
    void *ctx;
};
struct frf_dwc2_state {
    uint32_t core_id, hardware_config, global_interrupts, port_status;
    unsigned host_mode, port_connected, port_enabled, port_speed;
};
/* Pure observation: no writes or hardware ownership required. */
int frf_dwc2_probe(const struct frf_dwc2_ops *, struct frf_dwc2_state *);
/* Exclusive controller ownership required, with bounded timeouts. */
int frf_dwc2_host_init(const struct frf_dwc2_ops *);
/* Port power/reset, no USB transactions or class-device support yet. */
int frf_dwc2_port_reset(const struct frf_dwc2_ops *, struct frf_dwc2_state *);
#endif
