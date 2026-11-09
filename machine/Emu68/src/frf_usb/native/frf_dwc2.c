/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Future Retro Fusion.
 * Independent first-stage DWC2 controller diagnostics and host-port bring-up.
 * Register offsets/bit positions from published DWC2 programming model.
 */
#include "frf_dwc2.h"
#include <stddef.h>
#include <string.h>

enum {
    GAHBCFG = 0x008, GUSBCFG = 0x00c, GRSTCTL = 0x010,
    GHWCFG4 = 0x050, HCFG=0x400,
    GINTSTS = 0x014, GINTMSK = 0x018, GSNPSID = 0x040,
    GHWCFG2 = 0x048, HPRT = 0x440, PCGCCTL = 0xe00
};
#define BIT(n) (1u << (n))
#define AHB_IDLE BIT(31)
#define CORE_RESET BIT(0)
#define FORCE_HOST BIT(29)
#define FORCE_DEVICE BIT(30)
#define HOST_MODE BIT(0)
#define PRT_CONNECT BIT(0)
#define PRT_CONN_CHANGED BIT(1)
#define PRT_ENABLE BIT(2)
#define PRT_EN_CHANGED BIT(3)
#define PRT_OVERCURRENT_CHANGED BIT(5)
#define PRT_SUSPEND BIT(7)
#define PRT_RESET BIT(8)
#define PRT_POWER BIT(12)
#define PRT_W1C (PRT_CONN_CHANGED | PRT_ENABLE | PRT_EN_CHANGED | PRT_OVERCURRENT_CHANGED | PRT_SUSPEND)

static int valid(const struct frf_dwc2_ops *o)
{
    return o && o->read && o->write && o->delay_us;
}
static uint32_t rd(const struct frf_dwc2_ops *o, uint32_t r)
{
    return o->read(o->ctx, r);
}
static void wr(const struct frf_dwc2_ops *o, uint32_t r, uint32_t v)
{
    o->write(o->ctx, r, v);
}
static int wait_mask(const struct frf_dwc2_ops *o, uint32_t r,
                     uint32_t mask, uint32_t want, unsigned loops)
{
    for (unsigned i = 0; i != loops; ++i) {
        if ((rd(o, r) & mask) == want) return 0;
        o->delay_us(o->ctx, 10);
    }
    return -1;
}

int frf_dwc2_probe(const struct frf_dwc2_ops *o, struct frf_dwc2_state *s)
{
    if (!valid(o) || !s) return -1;
    memset(s, 0, sizeof(*s));
    s->core_id = rd(o, GSNPSID);
    if ((s->core_id & 0xffff0000u) != 0x4f540000u) return -2;
    s->hardware_config = rd(o, GHWCFG2);
    s->global_interrupts = rd(o, GINTSTS);
    s->host_mode = !!(s->global_interrupts & HOST_MODE);
    s->port_speed = 3u;
    if (s->host_mode) {
        s->port_status = rd(o, HPRT);
        s->port_connected = !!(s->port_status & PRT_CONNECT);
        s->port_enabled = !!(s->port_status & PRT_ENABLE);
        s->port_speed = (s->port_status >> 17) & 3u;
    }
    return 0;
}

int frf_dwc2_host_init(const struct frf_dwc2_ops *o)
{
    struct frf_dwc2_state s;
    if (frf_dwc2_probe(o, &s)) return -1;
    /* Standalone DWC2 host, program register-level PHY selection BEFORE
       soft reset, as in the Synopsys core bring-up model. All MMIO is
       explicitly little-endian through frf_dwc2_ops. This fixes the old
       code which forced host first and reset the core with no PHY setup.
       No USPi implementation or headers are used. */
    wr(o, GINTMSK, 0u);
    wr(o, GAHBCFG, rd(o, GAHBCFG) & ~(BIT(0) | BIT(5)));

    const uint32_t hwcfg2=s.hardware_config;
    const unsigned hsphy=(hwcfg2 >> 6) & 3u;
    if (hsphy==0u) return -5; /* not a high-speed host PHY */
    const int use_ulpi=(hsphy==2u);
    const unsigned width=(rd(o, GHWCFG4)>>14)&3u;
    const int utmi16=(!use_ulpi && width!=0u);
    uint32_t cfg=rd(o, GUSBCFG);
    cfg &= ~(BIT(3)|BIT(4)|BIT(6)|BIT(7)|BIT(17)|BIT(19)|
             BIT(20)|BIT(21)|BIT(29)|BIT(30));
    if(use_ulpi) cfg |= BIT(4);
    if(utmi16) cfg |= BIT(3);
    wr(o, GUSBCFG, cfg);

    if (wait_mask(o, GRSTCTL, AHB_IDLE, AHB_IDLE, 10000)) return -2;
    wr(o, GRSTCTL, rd(o, GRSTCTL) | CORE_RESET);
    if (wait_mask(o, GRSTCTL, CORE_RESET, 0u, 10000)) return -3;
    if (wait_mask(o, GRSTCTL, AHB_IDLE, AHB_IDLE, 10000)) return -6;
    o->delay_us(o->ctx, 100);

    cfg=rd(o, GUSBCFG);
    cfg=(cfg & ~((7u<<0)|(15u<<10)))|
        (7u<<0)|((utmi16?5u:9u)<<10);
    wr(o, GUSBCFG, cfg);
    /* Fully ungate high speed PHY; host mode is selected AFTER reset. */
    wr(o, PCGCCTL, rd(o,PCGCCTL) & ~15u);
    wr(o, GUSBCFG, (rd(o,GUSBCFG)|FORCE_HOST) & ~FORCE_DEVICE);
    if (wait_mask(o, GINTSTS, HOST_MODE, HOST_MODE, 15000)) return -4;
    wr(o, HCFG, rd(o,HCFG) & ~7u); /* HS enabled, 30/60MHz host clock */
    wr(o, GAHBCFG, rd(o,GAHBCFG) & ~(BIT(0)|BIT(5)));
    wr(o, GINTSTS, 0xffffffffu); /* clear stale state */
    wr(o, GINTMSK, 0u);
    return 0;
}

int frf_dwc2_port_reset(const struct frf_dwc2_ops *o,
                        struct frf_dwc2_state *s)
{
    if (!valid(o) || !s) return -1;
    if (!(rd(o, GINTSTS) & HOST_MODE)) return -2;
    uint32_t p = rd(o, HPRT);
    /* Never accidentally acknowledge write-one-to-clear status bits. */
    wr(o, HPRT, (p & ~PRT_W1C) | PRT_POWER);
    o->delay_us(o->ctx, 100000);
    p = rd(o, HPRT);
    for (unsigned attempt=0; attempt<40u && !(p & PRT_CONNECT); ++attempt) {
        o->delay_us(o->ctx,10000);
        p=rd(o,HPRT);
    }
    if (!(p & PRT_CONNECT)) {
        (void) frf_dwc2_probe(o, s);
        return -3;
    }
    wr(o, HPRT, (p & ~PRT_W1C) | PRT_POWER | PRT_RESET);
    o->delay_us(o->ctx, 60000);
    p = rd(o, HPRT);
    wr(o, HPRT, (p & ~(PRT_W1C | PRT_RESET)) | PRT_POWER);
    o->delay_us(o->ctx, 20000);
    (void) frf_dwc2_probe(o, s);
    return s->port_enabled ? 0 : -4;
}
