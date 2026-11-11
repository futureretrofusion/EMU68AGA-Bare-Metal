#ifndef FRF_XHCI_PROBE_H
#define FRF_XHCI_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only physical xHCI proof. No controller state is changed in V0.3.2. */
enum {
    FRFXHCI_PROBE_OK = 0,
    FRFXHCI_PROBE_NOT_RUN = 1,
    FRFXHCI_PROBE_NO_DT_NODE = 2,
    FRFXHCI_PROBE_DT_DISABLED = 3,
    FRFXHCI_PROBE_BAD_REG = 4,
    FRFXHCI_PROBE_BAD_ADDRESS = 5,
    FRFXHCI_PROBE_BAD_CAPS = 6,
    FRFXHCI_PROBE_NO_DEVICE_MAP = 7
};

struct FRFXHCIProbeInfo {
    uint32_t state;
    uint32_t status;
    uint32_t bus_base;
    uint32_t phys_base;
    uint32_t reg_bytes;
    uint32_t irq_spi;
    uint32_t gic_id;
    uint32_t capbase;
    uint32_t hcsparams1;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t portsc1;
    uint32_t max_ports;
    uint32_t max_slots;
    uint32_t max_intrs;
    uint32_t hci_version;
    uint32_t access_mode;
    uint32_t virt_base;
    uint32_t probes;
};

uint32_t frf_xhci_probe_run(void);
const volatile struct FRFXHCIProbeInfo *frf_xhci_probe_info(void);

#ifdef __cplusplus
}
#endif

#endif
