/* SPDX-License-Identifier: GPL-2.0-only
 * EMU68AGA native USB TinyUSB host configuration.
 * Separate experimental firmware backend. NO public release until hardware tests.
 */
#pragma once
#define CFG_TUSB_MCU OPT_MCU_BCM2837
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED
#define CFG_TUH_HUB 4
#define CFG_TUH_DEVICE_MAX 16
#define CFG_TUH_HID 8
#define CFG_TUH_MSC 4
#define CFG_TUH_MSC_MAXLUN 1
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(32)))
#define CFG_TUSB_DEBUG 0
#define CFG_TUH_DWC2_DMA_ENABLE 0
#define CFG_TUH_DWC2_SLAVE_ENABLE 1

#define CFG_TUSB_FIFO_HWFIFO_CUSTOM_WRITE 1
#define CFG_TUSB_FIFO_HWFIFO_CUSTOM_READ 1
