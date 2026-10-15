/*
 * Bare-metal absent native-hardware responses for A1200 Gayle/IDE.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * No physical Amiga bus access, sleeps or indefinite completion loops.
 * Values derived from Gayle status/control mapping and UAE absent-IDE
 * detection convention: IDE status 0x7f (never busy) / others floating 0xff.
 *
 * Integration remains opt-in pending real hardware regression.
 */
#include "frf_native_bus.h"
#include <string.h>

#define IDE_START  0x00da0000u
#define IDE_END    0x00da3fffu
#define GAYLE_CS   0x00da8000u
#define GAYLE_IRQ  0x00da9000u
#define GAYLE_INT  0x00daa000u
#define GAYLE_CFG  0x00dab000u
#define IDE_ALT_STATUS_0 (IDE_START + 0x101cu)
#define IDE_ALT_STATUS_1 (IDE_START + 0x101du)
#define IDE_STATUS_0 (IDE_START + 0x001cu)
#define IDE_STATUS_1 (IDE_START + 0x001du)
#define IDE_STATUS_2 (IDE_START + 0x001eu)

static uint8_t irq_enable;
static uint8_t configuration;
static frf_native_backend backend;
static frf_native_bus_stats stats;

static int mapped(uint32_t a)
{
    return (a >= IDE_START && a <= IDE_END) ||
           (a >= GAYLE_CS && a <= GAYLE_CS+3u) ||
           (a >= GAYLE_IRQ && a <= GAYLE_IRQ+3u) ||
           (a >= GAYLE_INT && a <= GAYLE_INT+3u) ||
           (a >= GAYLE_CFG && a <= GAYLE_CFG+3u);
}
static int ide(uint32_t a) { return a >= IDE_START && a <= IDE_END; }
static int ide_status(uint32_t a)
{
    return a == IDE_STATUS_0 || a == IDE_STATUS_1 ||
           a == IDE_STATUS_2 || a == IDE_ALT_STATUS_0 ||
           a == IDE_ALT_STATUS_1;
}
static uint8_t byte_read(uint32_t a)
{
    if (ide(a)) return ide_status(a) ? 0x7fu : 0xffu;
    /* An empty PCMCIA slot must NOT assert card detect or IDE IRQ. */
    if (a == GAYLE_CS || a == GAYLE_IRQ) return 0u;
    if (a == GAYLE_INT) return irq_enable;
    if (a == GAYLE_CFG) return configuration;
    return 0xffu; /* unused bus lanes float high */
}
void frf_native_bus_reset(void)
{
    irq_enable = 0;
    configuration = 0;
    memset(&stats, 0, sizeof(stats));
    /* Do not clear physical backend: reset must not silently lose it. */
}
void frf_native_bus_set_backend(const frf_native_backend *next)
{
    if (next) backend = *next;
    else memset(&backend, 0, sizeof(backend));
}
int frf_native_bus_read(uint32_t a, unsigned size, uint64_t *out)
{
    unsigned i;
    uint64_t v = 0;
    if (!out || (size != 1u && size != 2u && size != 4u &&
                 size != 8u)) return 0;
    if (!mapped(a) || !mapped(a+size-1u)) return 0;
    if (backend.read && backend.read(backend.ctx, a, size, out)) {
        stats.passthrough_reads++;
        return 1;
    }
    for (i=0; i<size; ++i) v = (v << 8) | byte_read(a+i);
    *out = v;
    if (ide(a)) stats.ide_reads++;
    else stats.gayle_reads++;
    return 1;
}
int frf_native_bus_write(uint32_t a, unsigned size, uint64_t value)
{
    unsigned i;
    if (size != 1u && size != 2u && size != 4u &&
        size != 8u) return 0;
    if (!mapped(a) || !mapped(a+size-1u)) return 0;
    if (backend.write && backend.write(backend.ctx,a,size,value)) {
        stats.passthrough_writes++;
        return 1;
    }
    /* No attached storage device. Writes cannot produce an IRQ, DMA,
       phantom disk, false mount or busy-wait. Gayle mask/config latched. */
    for (i=0; i<size; ++i) {
        uint32_t pos=a+i;
        uint8_t b = (uint8_t)(value >> (8u * (size-i-1u)));
        if (pos == GAYLE_INT) irq_enable = b;
        if (pos == GAYLE_CFG) configuration = b & 0x0fu;
    }
    if (ide(a)) stats.ide_writes++;
    else stats.gayle_writes++;
    return 1;
}
frf_native_bus_stats frf_native_bus_get_stats(void) { return stats; }
