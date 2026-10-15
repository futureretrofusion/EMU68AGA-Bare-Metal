/* EMU68AGA Bare Metal: virtual optional native hardware bus.
 * Copyright (c) 2026 Future Retro Fusion contributors.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Unattached Gayle IDE/PCMCIA emulation for an A1200-compatible guest.
 * A probe MUST terminate without waiting for a physical drive or PC card.
 * The backend seam permits a future physical bridge or virtual IDE device.
 *
 * Enabled only with FRF_VIRTUAL_GAYLE_ABSENT and FRF_VIRTUAL_AGA.
 */
#ifndef FRF_NATIVE_BUS_H
#define FRF_NATIVE_BUS_H
#include <stdint.h>

enum frf_native_device {
    FRF_NATIVE_GAYLE = 1
};
typedef struct {
    void *ctx;
    /* Return 1 when provided, 0 to use absent-device emulation. No blocking. */
    int (*read)(void *ctx, uint32_t address, unsigned size, uint64_t *result);
    int (*write)(void *ctx, uint32_t address, unsigned size, uint64_t value);
} frf_native_backend;

typedef struct {
    uint32_t ide_reads;
    uint32_t gayle_reads;
    uint32_t ide_writes;
    uint32_t gayle_writes;
    uint32_t passthrough_reads;
    uint32_t passthrough_writes;
} frf_native_bus_stats;

void frf_native_bus_reset(void);
/* Installation requires an externally synchronized/quiescent guest.
 * A real backend must implement its own finite deadlines and errors. */
void frf_native_bus_set_backend(const frf_native_backend *backend);
int frf_native_bus_read(uint32_t address, unsigned size, uint64_t *result);
int frf_native_bus_write(uint32_t address, unsigned size, uint64_t value);
frf_native_bus_stats frf_native_bus_get_stats(void);
#endif
