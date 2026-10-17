/*
 * EMU68AGA Pi 3A+ WiFiPi compatibility bridge.
 * Copyright (c) 2026 Future Retro Fusion contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NO hardware reinitialization, no new network device, and no SDIO writes.
 * Firmware may load bcm2710-rpi-3-b-plus.dtb as fallback for physical 3A+;
 * WiFiPi 0.3.2 uses the FIRST /compatible string for selecting its 43455
 * firmware and otherwise rejects an unrecognised Pi model.
 *
 * Correct the guest-facing compatible property ONLY when the VideoCore
 * mailbox confirms physical board type 0x0E = Pi 3 Model A+.
 */
#include <stdint.h>
#include <string.h>
#include "devicetree.h"
#include "support.h"
extern uint32_t frf_get_board_revision(void);
extern void kprintf(const char *fmt, ...);

void frf_wifi_guest_compat(void)
{
    const uint32_t rev = frf_get_board_revision();
    const unsigned model = (rev >> 4) & 0xffu;
    const int new_scheme = (rev & (1u << 23)) != 0;
    kprintf("[WIFI] physical board revision=%08x model=%02x new=%d\n",
            rev, model, new_scheme);
    if (!new_scheme || model != 0x0eu) {
        kprintf("[WIFI] no Pi 3A+ guest DT compatibility changes\n");
        return;
    }
    of_node_t *root = dt_find_node("/");
    if (!root) {
        kprintf("[WIFI] no root DT node, cannot prepare WiFiPi firmware match\n");
        return;
    }
    const char new_compat[] = "raspberrypi,3-model-a-plus\0brcm,bcm2837";
    of_property_t *old = dt_find_property(root, "compatible");
    if (old && old->op_value && old->op_length >= sizeof("raspberrypi,3-model-a-plus") &&
        !strcmp((const char*)old->op_value,"raspberrypi,3-model-a-plus")) {
        kprintf("[WIFI] Pi 3A+ compatible already matches WiFiPi\n");
    } else {
        dt_add_property(root, "compatible", new_compat, sizeof(new_compat));
        kprintf("[WIFI] corrected fallback DT compatible for Pi 3A+ WiFiPi firmware matching\n");
    }
    const char model_name[]="Raspberry Pi 3 Model A+";
    dt_add_property(root, "model", model_name, sizeof(model_name));
    of_node_t *aliases = dt_find_node("/aliases");
    const of_property_t *mmc = dt_find_property(aliases, "mmc");
    const of_property_t *mmc1 = dt_find_property(aliases, "mmc1");
    kprintf("[WIFI] guest mmc alias=%s mmc1=%s (not modified)\n",
            mmc && mmc->op_value ? (const char*)mmc->op_value : "missing",
            mmc1 && mmc1->op_value ? (const char*)mmc1->op_value : "missing");
}
