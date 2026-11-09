/*
 * SPDX-License-Identifier: MIT
 * EMU68AGA adaptation of the MIT TinyUSB DWC2 BCM port.
 * Emu68 maps the Pi3 physical USB controller at virtual 0xF2980000.
 * Only DWC2 MMIO register types use LE scalar storage. Internal DMA
 * uses the Pi3 GPU bus address and explicit cache maintenance on CPU1.
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
/* DMA cache hooks run on physical CPU pointers, not GPU bus addresses. */
#if CFG_TUH_MEM_DCACHE_ENABLE
extern void arm_flush_cache(uintptr_t addr, uint32_t len);
extern void arm_dcache_invalidate(uintptr_t addr, uint32_t len);
static inline bool dwc2_dcache_clean(const void *addr, uint32_t len) {
  if (!addr || !len) return true;
  arm_flush_cache((uintptr_t)addr,len);
  return true;
}
static inline bool dwc2_dcache_invalidate(const void *addr, uint32_t len) {
  if (!addr || !len) return true;
  arm_dcache_invalidate((uintptr_t)addr,len);
  return true;
}
static inline bool dwc2_dcache_clean_invalidate(const void *addr, uint32_t len) {
  if (!addr || !len) return true;
  arm_flush_cache((uintptr_t)addr,len);
  return true;
}
#endif
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
