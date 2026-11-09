/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#define DWC2_COMMON_DEBUG   2

#if defined(TUP_USBIP_DWC2) && (CFG_TUH_ENABLED || CFG_TUD_ENABLED)
#include "dwc2_common.h"

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------
/* Firmware USB HCD must never park CPU1 in an infinite PHY-reset spin.
 * All timeouts derive from the ARM generic timer via the TinyUSB port. */
static bool reset_core(dwc2_regs_t* dwc2) {
  uint32_t deadline=tusb_time_millis_api();
  while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) {
    if ((uint32_t)(tusb_time_millis_api()-deadline)>250u) return false;
  }

  const uint32_t gsnpsid = dwc2->gsnpsid; // preload gsnpsid which is not readable while resetting
  dwc2->grstctl |= GRSTCTL_CSRST; // reset core

  if ((gsnpsid & DWC2_CORE_REV_MASK) < (DWC2_CORE_REV_4_20a & DWC2_CORE_REV_MASK)) {
    // prior v4.20a: CSRST is self-clearing, and the core clears this bit after all the necessary logic is reset in
    // the core, which can take several clocks, depending on the current state of the core. Once this bit has been
    // cleared, the software must wait at least 3 PHY clocks before accessing the PHY domain (synchronization delay).
    deadline=tusb_time_millis_api();
    while (dwc2->grstctl & GRSTCTL_CSRST) {
      if ((uint32_t)(tusb_time_millis_api()-deadline)>500u) return false;
    }
  } else {
    // From v4.20a: CSRST bit is write only. The application must clear this bit after checking the bit 29 of this
    // register i.e Core Soft Reset Done CSRT_DONE (w1c)
    deadline=tusb_time_millis_api();
    while (!(dwc2->grstctl & GRSTCTL_CSRST_DONE)) {
      if ((uint32_t)(tusb_time_millis_api()-deadline)>500u) return false;
    }
    dwc2->grstctl = (dwc2->grstctl & ~GRSTCTL_CSRST) | GRSTCTL_CSRST_DONE;
  }

  deadline=tusb_time_millis_api();
  while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) {
    if ((uint32_t)(tusb_time_millis_api()-deadline)>250u) return false;
  }
  return true;
}

// Dedicated FS PHY is internal with a clock 48Mhz.
static bool phy_fs_init(dwc2_regs_t* dwc2) {
  TU_LOG(DWC2_COMMON_DEBUG, "Fullspeed PHY init\r\n");

  uint32_t gusbcfg = dwc2->gusbcfg;

  // Select FS PHY
  gusbcfg |= GUSBCFG_PHYSEL;
  dwc2->gusbcfg = gusbcfg;

  // MCU specific PHY init before reset
  dwc2_phy_init(dwc2, GHWCFG2_HSPHY_NOT_SUPPORTED);

  // Reset core after selecting PHY
  if (!reset_core(dwc2)) return false;

  // USB turnaround time is critical for certification where long cables and 5-Hubs are used.
  // So if you need the AHB to run at less than 30 MHz, and if USB turnaround time is not critical,
  // these bits can be programmed to a larger value. Default is 5
  gusbcfg &= ~GUSBCFG_TRDT_Msk;
  gusbcfg |= 5u << GUSBCFG_TRDT_Pos;
  dwc2->gusbcfg = gusbcfg;

  // MCU specific PHY update post reset
  dwc2_phy_update(dwc2, GHWCFG2_HSPHY_NOT_SUPPORTED);
  return true;
}

/* dwc2 has 2 highspeed PHYs options
 * - UTMI+ is internal highspeed PHY, can be clocked at 30 Mhz (8-bit) or 60 Mhz (16-bit).
 * - ULPI is external highspeed PHY, clocked at 60Mhz with 8-bit interface.
 *
 * In addition, UTMI+/ULPI can be shared to run at fullspeed mode with 48Mhz
 */
static bool phy_hs_init(dwc2_regs_t* dwc2) {
  uint32_t gusbcfg = dwc2->gusbcfg;
  const dwc2_ghwcfg2_t ghwcfg2 = {.value = dwc2->ghwcfg2};
  const dwc2_ghwcfg4_t ghwcfg4 = {.value = dwc2->ghwcfg4};

  uint8_t phy_width;
  if (CFG_TUSB_MCU != OPT_MCU_AT32F402_405 && // at32f402_405 does not support 16-bit
      ghwcfg4.phy_data_width) {
    phy_width = 16; // 16-bit PHY interface if supported
  } else {
    phy_width = 8; // 8-bit PHY interface
  }

  // De-select FS PHY
  gusbcfg &= ~GUSBCFG_PHYSEL;
#ifdef FRF_USB_TINYUSB
  /* Broadcom BCM2837 host quirk, matched to the locally verified USPi
   * DWHCIDeviceInitCore(): select UTMI+ 8-bit even when GHWCFG2 advertises
   * ULPI, disable external VBUS drive and term-select DL pulse.
   * Generic TinyUSB's HS ULPI selection prevents Pi3's root host PHY
   * from ever connecting to the hardwired LAN9514 hub. */
  phy_width=8u;
  gusbcfg &= ~(GUSBCFG_ULPI_UTMI_SEL | GUSBCFG_PHYIF16 |
               GUSBCFG_ULPIEVBUSD | GUSBCFG_ULPIEVBUSI |
               GUSBCFG_DDRSEL | (1u<<22));
#else
  if (ghwcfg2.hs_phy_type == GHWCFG2_HSPHY_ULPI) {
    TU_LOG(DWC2_COMMON_DEBUG, "Highspeed ULPI PHY init\r\n");

    // Select ULPI PHY (external)
    gusbcfg |= GUSBCFG_ULPI_UTMI_SEL;

    // ULPI is always 8-bit interface
    gusbcfg &= ~GUSBCFG_PHYIF16;

    // ULPI select single data rate
    gusbcfg &= ~GUSBCFG_DDRSEL;

    // default internal VBUS Indicator and Drive
    gusbcfg &= ~(GUSBCFG_ULPIEVBUSD | GUSBCFG_ULPIEVBUSI);

    // Disable FS/LS ULPI
    gusbcfg &= ~(GUSBCFG_ULPIFSLS | GUSBCFG_ULPICSM);
  } else {
    TU_LOG(DWC2_COMMON_DEBUG, "Highspeed UTMI+ PHY init\r\n");

    // Select UTMI+ PHY (internal)
    gusbcfg &= ~GUSBCFG_ULPI_UTMI_SEL;

    // Set 16-bit interface if supported
    if (phy_width == 16) {
      gusbcfg |= GUSBCFG_PHYIF16;
    } else {
      gusbcfg &= ~GUSBCFG_PHYIF16;
    }
  }

#endif
#ifdef FRF_USB_TINYUSB
  /* BCM2837: match the previously working bare-metal USPi sequence.
   * First clear external VBUS drive/TERMSEL, then reset the core, settle
   * 100ms, and ONLY THEN select UTMI+ 8 bit. The generic DWC2 driver
   * selected a PHY before resetting; that does not match the Pi3 stack.
   */
  dwc2->gusbcfg &= ~(GUSBCFG_ULPIEVBUSD | (1u<<22));
  if(!reset_core(dwc2)) return false;
  uint32_t reset_wait=tusb_time_millis_api();
  while((uint32_t)(tusb_time_millis_api()-reset_wait)<100u) {}
  dwc2->gusbcfg = gusbcfg;
  dwc2_phy_init(dwc2, ghwcfg2.hs_phy_type);
#else
  // Generic Synopsys DWC2: select PHY, reset core, then configure clocks.
  dwc2->gusbcfg = gusbcfg;
  dwc2_phy_init(dwc2, ghwcfg2.hs_phy_type);
  if (!reset_core(dwc2)) return false;
#endif

  // Set turn-around, must after core reset otherwise it will be clear
  // - 9 if using 8-bit PHY interface
  // - 5 if using 16-bit PHY interface
  gusbcfg &= ~GUSBCFG_TRDT_Msk;
  gusbcfg |= (phy_width == 16 ? 5u : 9u) << GUSBCFG_TRDT_Pos;
#ifdef FRF_USB_TINYUSB
  /* USPi's Pi3 core init preserves ULPI FS/LS clock behaviour for
   * dedicated FS PHY configurations even with UTMI+ selected. */
  gusbcfg &= ~(GUSBCFG_ULPIFSLS | GUSBCFG_ULPICSM);
  if(ghwcfg2.hs_phy_type==GHWCFG2_HSPHY_ULPI &&
     ghwcfg2.fs_phy_type==GHWCFG2_FSPHY_DEDICATED) {
      gusbcfg |= (GUSBCFG_ULPIFSLS | GUSBCFG_ULPICSM);
  }
#endif
  dwc2->gusbcfg = gusbcfg;

  // MCU specific PHY update post reset
  dwc2_phy_update(dwc2, ghwcfg2.hs_phy_type);
  return true;
}

static bool check_dwc2(dwc2_regs_t* dwc2) {
#if CFG_TUSB_DEBUG >= DWC2_COMMON_DEBUG
  // print guid, gsnpsid, ghwcfg1, ghwcfg2, ghwcfg3, ghwcfg4
  // Run 'python dwc2_info.py' and check dwc2_info.md for bit-field value and comparison with other ports
  volatile uint32_t const* p = (volatile uint32_t const*) &dwc2->guid;
  TU_LOG1("guid, gsnpsid, ghwcfg1, ghwcfg2, ghwcfg3, ghwcfg4\r\n");
  for (size_t i = 0; i < 5; i++) {
    TU_LOG1("0x%08" PRIX32 ", ", p[i]);
  }
  TU_LOG1("0x%08" PRIX32 "\r\n", p[5]);
#endif

  // For some reason: GD32VF103 gsnpsid and all hwcfg register are always zero (skip it)
  (void)dwc2;
#if !TU_CHECK_MCU(OPT_MCU_GD32VF103)
  enum { GSNPSID_ID_MASK = TU_GENMASK(31, 16) };
  const uint32_t gsnpsid = dwc2->gsnpsid & GSNPSID_ID_MASK;
  TU_ASSERT(gsnpsid == DWC2_OTG_ID || gsnpsid == DWC2_FS_IOT_ID || gsnpsid == DWC2_HS_IOT_ID);
#endif

  return true;
}

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------
bool dwc2_core_is_highspeed_phy(dwc2_regs_t* dwc2, bool prefer_hs_phy) {
  const dwc2_ghwcfg2_t ghwcfg2    = {.value = dwc2->ghwcfg2};
  const bool           has_hs_phy = (ghwcfg2.hs_phy_type != GHWCFG2_HSPHY_NOT_SUPPORTED);

  if (prefer_hs_phy) {
    return has_hs_phy;
  } else {
    const bool has_fs_phy = (ghwcfg2.fs_phy_type != GHWCFG2_FSPHY_NOT_SUPPORTED);
    // false if has fs phy, otherwise true since hs phy is the only available phy
    return !has_fs_phy && has_hs_phy;
  }
}

bool dwc2_core_init(uint8_t rhport, bool is_hs_phy, bool is_dma) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  // Check Synopsys ID register, failed if controller clock/power is not enabled
  TU_ASSERT(check_dwc2(dwc2));

  // disable global interrupt
  dwc2->gahbcfg &= ~GAHBCFG_GINT;

  if (is_hs_phy) {
    TU_ASSERT(phy_hs_init(dwc2));
  } else {
    TU_ASSERT(phy_fs_init(dwc2));
  }

  /* Set HS/FS Timeout Calibration to 7 (max available value).
   * The number of PHY clocks that the application programs in
   * this field is added to the high/full speed interpacket timeout
   * duration in the core to account for any additional delays
   * introduced by the PHY. This can be required, because the delay
   * introduced by the PHY in generating the linestate condition
   * can vary from one PHY to another. */
  dwc2->gusbcfg |= (7ul << GUSBCFG_TOCAL_Pos);

  // Enable PHY clock TODO stop/gate clock when suspended mode
#ifdef FRF_USB_TINYUSB
  /* USPi DWHCIDeviceInitHost writes zero to BCM ARM_USB_POWER (0xE00)
   * rather than masking the four generic low-power bits. Restore that
   * proven Pi3 behaviour so PHY clock is completely ungated. */
  dwc2->pcgcctl=0u;
  dwc2->gusbcfg &= ~(GUSBCFG_HNPCAP | GUSBCFG_SRPCAP);
  dwc2->gahbcfg |= (1u<<4); /* BCM2837 WAIT_AXI_WRITES */
#else
  dwc2->pcgcctl &= ~(PCGCCTL_STOPPCLK | PCGCCTL_GATEHCLK | PCGCCTL_PWRCLMP | PCGCCTL_RSTPDWNMODULE);
#endif

  TU_ASSERT(dfifo_flush_tx(dwc2, 0x10)); // all tx FIFO
  TU_ASSERT(dfifo_flush_rx(dwc2));

  // Clear pending and disable all interrupts
  dwc2->gintsts = 0xFFFFFFFFU;
  dwc2->gotgint = 0xFFFFFFFFU;
  dwc2->gintmsk = 0;

  TU_LOG(DWC2_COMMON_DEBUG, "DMA = %u\r\n", is_dma);

  if (is_dma) {
    // DMA seems to be only settable after a core reset, and not possible to switch on-the-fly
#ifdef FRF_USB_TINYUSB
    /* Pi 3 BCM2837: working USPi explicitly clears the BCM-specific
     * MAX_AXI_BURST bits [2:1] and sets WAIT_AXI_WRITES (bit 4).
     * TinyUSB's generic INCR4 value (GAHBCFG_HBSTLEN_2 = 0x6)
     * turns those bits back on and can change DMA bus behaviour.
     * Preserve the known Pi3 controller programming, not generic DWC2.
     */
    dwc2->gahbcfg=(dwc2->gahbcfg | GAHBCFG_DMAEN | (1u<<4)) &
                   ~(3u<<1);
#else
    dwc2->gahbcfg |= GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_2;
#endif
  } else {
    dwc2->gintmsk |= GINTSTS_RXFLVL;
  }

  return true;
}

void dwc2_core_deinit(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  // Disable global interrupt
  dwc2->gahbcfg &= ~GAHBCFG_GINT;

  // Reset core: this also flushes FIFOs and clears all interrupt registers
  (void)reset_core(dwc2);

  // Stop PHY clock and gate HCLK for power saving (per databook chapter 14)
  dwc2->pcgcctl |= PCGCCTL_STOPPCLK | PCGCCTL_GATEHCLK;

  // MCU-specific PHY deinit (disable PHY power)
  const dwc2_ghwcfg2_t ghwcfg2 = {.value = dwc2->ghwcfg2};
  const uint8_t hs_phy_type = (dwc2->gusbcfg & GUSBCFG_PHYSEL) ? GHWCFG2_HSPHY_NOT_SUPPORTED : ghwcfg2.hs_phy_type;
  dwc2_phy_deinit(dwc2, hs_phy_type);
}

// void dwc2_core_handle_common_irq(uint8_t rhport, bool in_isr) {
//   (void) in_isr;
//   dwc2_regs_t * const dwc2 = DWC2_REG(rhport);
//   const uint32_t int_mask = dwc2->gintmsk;
//   const uint32_t int_status = dwc2->gintsts & int_mask;
//
//   // Device disconnect
//   if (int_status & GINTSTS_DISCINT) {
//     dwc2->gintsts = GINTSTS_DISCINT;
//   }
//
// }

#endif
