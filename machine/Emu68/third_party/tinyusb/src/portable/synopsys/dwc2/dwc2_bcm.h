/*
 * SPDX-License-Identifier: MIT
 * EMU68AGA adaptation of the MIT TinyUSB DWC2 BCM port.
 * Emu68 maps the Pi3 physical USB controller at virtual 0xF2980000.
 * DWC2 compilation uses -fsso-struct=little-endian; FIFOs have explicit
 * endian conversion. Poll the DWC2 event handler only on USB CPU1.
 * This is experimental and not hardware release validated.
 */
#ifndef TUSB_DWC2_BCM_H_
#define TUSB_DWC2_BCM_H_
#if !defined(FRF_USB_TINYUSB) && !defined(FRF_TINYUSB_COMPILE_PROBE)
#error "EMU68AGA TinyUSB adapter may only build in its native USB backend"
#endif
#define DWC2_EP_MAX 8
static const dwc2_controller_t _dwc2_controller[] = {
 { .reg_base=0xf2980000u, .irqnum=9, .ep_count=DWC2_EP_MAX,
   .ep_in_count=DWC2_EP_MAX, .otg_dfifo_depth=4096 }
};
#define dcache_clean(addr,n) ((void)0)
#define dcache_invalidate(addr,n) ((void)0)
#define dcache_clean_invalidate(addr,n) ((void)0)
static inline void dwc2_clock_init(uint8_t port,tusb_role_t role)
{ (void)port;(void)role; }
static inline void dwc2_int_set(uint8_t port,tusb_role_t role,bool enabled)
{ (void)port;(void)role;(void)enabled; }
static inline void dwc2_remote_wakeup_delay(void) {}
static inline void dwc2_phy_init(dwc2_regs_t*regs,uint8_t phy)
{ (void)regs;(void)phy; }
static inline void dwc2_phy_deinit(dwc2_regs_t*regs,uint8_t phy)
{ (void)regs;(void)phy; }
static inline void dwc2_phy_update(dwc2_regs_t*regs,uint8_t phy)
{ (void)regs;(void)phy; }
#endif
