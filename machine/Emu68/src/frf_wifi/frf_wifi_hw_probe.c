/*
 * EMU68AGA Bare Metal: Pi 3A+ Wi-Fi SDIO host hardware discovery.
 * Copyright (c) 2026 Future Retro Fusion contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Read-only first stage. Deliberately no SDIO controller register writes:
 * keeps the known-good SD boot partition, AGS RDB and USB input untouched.
 * Enabled only for a confirmed Raspberry Pi 3 Model A Plus.
 * NOT a network driver: no CMD5/CMD52, firmware or WPA connection.
 */
#include <stdint.h>
#include <string.h>
#include "devicetree.h"
#include "support.h"

#define FRF_WIFI_SD1_VIRT_BASE ((uintptr_t)0xF2300000u)
#define FRF_WIFI_SLOTISR_VER  (0xFCu)
#define FRF_WIFI_STATUS       (0x24u)
#define FRF_WIFI_CONTROL0     (0x28u)
#define FRF_WIFI_CONTROL1     (0x2Cu)
#define FRF_WIFI_CAPABILITIES (0x40u)

static const char *property_string(of_node_t *node, const char *name)
{
    of_property_t *p = node ? dt_find_property(node, name) : 0;
    if (!p || !p->op_value || !p->op_length)
        return 0;
    const char *s = (const char *)p->op_value;
    if (s[p->op_length-1] != 0)
        return 0;
    return s;
}

static int compatible(of_node_t *node, const char *want)
{
    of_property_t *p = node ? dt_find_property(node, "compatible") : 0;
    if (!p || !p->op_value || !p->op_length)
        return 0;
    const char *a = (const char *)p->op_value;
    unsigned i = 0;
    while (i < p->op_length) {
        unsigned start=i;
        while (i < p->op_length && a[i]) ++i;
        if (i < p->op_length && strlen(want)==i-start &&
            !memcmp(a+start,want,i-start)) return 1;
        ++i;
    }
    return 0;
}
static void describe(const char *path)
{
    of_node_t *n = dt_find_node(path);
    const char *state = property_string(n, "status");
    const char *hw = property_string(n,"compatible");
    kprintf("[WIFI] DT %s: %s, compatible=%s\n",path,
        n ? (state ? state : "enabled-default") : "missing",
        hw ? hw : "unspecified");
}
static int enabled(of_node_t *n)
{
    const char *p=property_string(n,"status");
    return n && (!p || !strcmp(p,"okay") || !strcmp(p,"ok"));
}
static uint32_t read_sd1(uint32_t offset)
{
    /* Kernel is big-endian while Raspberry Pi MMIO is little-endian. */
    return LE32(*(volatile uint32_t *)(FRF_WIFI_SD1_VIRT_BASE+offset));
}
void frf_wifi_hw_probe(void)
{
    of_node_t *root=dt_find_node("/");
    of_node_t *aliases=dt_find_node("/aliases");
    const char *model=property_string(root, "model");
    const char *mmc=property_string(aliases,"mmc");
    const char *mmc1=property_string(aliases,"mmc1");
    of_node_t *wifi=dt_find_node("/soc/mmcnr@7e300000");
    int pi3a=compatible(root, "raspberrypi,3-model-a-plus");

    kprintf("[WIFI] Probe v0.2 READ ONLY: model=%s pi3a=%d\n",
            model ? model : "unknown",pi3a);
    kprintf("[WIFI] aliases: mmc=%s mmc1=%s\n",
            mmc ? mmc : "(none)", mmc1 ? mmc1 : "(none)");
    describe("/soc/mmcnr@7e300000");
    describe("/soc/mmcnr@7e300000/wifi@1");
    describe("/soc/mmc@7e300000");
    describe("/soc/mmc@7e202000");

    if (!pi3a || !enabled(wifi)) {
        kprintf("[WIFI] SD1 MMIO skipped: unverified board or disabled Wi-Fi node\n");
        return;
    }
    /* Pi 3A+ /soc is mapped at virtual 0xF2000000 in Emu68;
     * the Wi-Fi host at bus 0x7E300000 becomes 0xF2300000.
     * SD0 boot controller is not touched. Read without changing state.
     */
    uint32_t version=read_sd1(FRF_WIFI_SLOTISR_VER);
    uint32_t status=read_sd1(FRF_WIFI_STATUS);
    uint32_t control0=read_sd1(FRF_WIFI_CONTROL0);
    uint32_t control1=read_sd1(FRF_WIFI_CONTROL1);
    uint32_t caps=read_sd1(FRF_WIFI_CAPABILITIES);
    kprintf("[WIFI] SD1 registers: slotver=%08x status=%08x control0=%08x\n",
            version,status,control0);
    kprintf("[WIFI] SD1 control1=%08x capabilities=%08x\n",control1,caps);
    if ((version&0x0000FF00u)==0u || (version&0x0000FF00u)==0xFF00u)
        kprintf("[WIFI] SD1 VERSION unusual; do not issue SDIO commands\n");
    else
        kprintf("[WIFI] SD1 readable. CMD5/CCCR/firmware NOT YET attempted\n");
}
