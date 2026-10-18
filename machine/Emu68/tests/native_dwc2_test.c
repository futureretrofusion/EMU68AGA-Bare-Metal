/* SPDX-License-Identifier: GPL-2.0-only
 * DWC2 MMIO model. This tests register state and W1C safety without hardware.
 */
#include "../src/frf_usb/native/frf_dwc2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { GUSBCFG=0x00c, GRSTCTL=0x010, GINTSTS=0x014,
       GSNPSID=0x040, HPRT=0x440 };
static uint32_t regs[1024];
static unsigned wrote_reset, accidental_w1c, calls;
static uint32_t mock_read(void *ctx, uint32_t offset)
{
    (void)ctx;
    assert(offset/4 < 1024);
    if (offset == GRSTCTL) return (1u << 31);
    return regs[offset/4];
}
static void mock_write(void *ctx, uint32_t offset, uint32_t v)
{
    (void)ctx;
    assert(offset/4 < 1024);
    calls++;
    if (offset == HPRT) {
        if (v & ((1u << 1) | (1u << 3) | (1u << 5))) accidental_w1c++;
        if (v & (1u << 8)) wrote_reset++;
        /* Hardware retains connection, sets enable once reset removed. */
        uint32_t connected = regs[offset/4] & 1u;
        regs[offset/4] = (v & ~(1u << 8)) | connected;
        if (connected && wrote_reset && !(v & (1u << 8)))
            regs[offset/4] |= 4u;
    } else regs[offset/4] = v;
    if (offset == GUSBCFG && (v & (1u << 29)))
        regs[GINTSTS/4] |= 1u;
}
static void mock_delay(void *ctx, unsigned us) { (void)ctx; (void)us; }
int main(void)
{
    struct frf_dwc2_state s;
    struct frf_dwc2_ops o = {mock_read, mock_write, mock_delay, 0};
    assert(frf_dwc2_probe(&o, &s) == -2);
    regs[GSNPSID/4] = 0x4f54280au;
    regs[HPRT/4] = 1u | (1u << 12) | (1u << 1);
    assert(frf_dwc2_probe(&o, &s) == 0);
    assert(!s.host_mode && s.port_speed == 3u && calls == 0);
    assert(frf_dwc2_host_init(&o) == 0);
    assert(frf_dwc2_port_reset(&o, &s) == 0);
    assert(s.port_connected && s.port_enabled && s.port_speed == 0u);
    assert(wrote_reset == 1u && accidental_w1c == 0u);
    regs[HPRT/4] = 0u;
    assert(frf_dwc2_port_reset(&o, &s) == -3);
    assert(!s.port_connected);
    puts("native DWC2 mock tests: PASS");
    return 0;
}
