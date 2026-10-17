/* SPDX-License-Identifier: GPL-2.0-or-later
 * Original FRF network Zorro-II IO expansion: 64KiB, manufacturer 0x6D73,
 * product 0x68. Explicitly NOT a RAM board. Coherent single physical buffer
 * mapped for guest and host. No hardware activation or SD controller changes.
 */
#include <stdint.h>
#include <boards.h>
#include <tlsf.h>
#include <mmu.h>
#include <A64.h>
#include <support.h>
#include "frf_net_bridge.h"

extern void *tlsf;
volatile struct frf_net_shared *frf_net_shared_host;

#define NIC_SIZE 65536u
#define AC_NIB(n) ((uint16_t)(((n)&15u)<<12))
#define AC_BYTE(b) AC_NIB((b)>>4), AC_NIB(b)
#define AC_INV(b) AC_NIB((~(b))>>4), AC_NIB(~(b))

static const uint16_t nic_rom[32] = {
    AC_BYTE(0xC1),       /* Z2 IO board, 64KiB, not memory-list RAM */
    AC_INV(0x68),        /* product */
    AC_INV(0x00),        /* not 8MB memory */
    AC_INV(0x00),        /* reserved */
    AC_INV(0x6D),AC_INV(0x73), /* Emu68 virtual manufacturer */
    AC_INV(0x46),AC_INV(0x52),AC_INV(0x46),AC_INV(0x4E),/* serial FRFN */
    AC_INV(0x00),AC_INV(0x00),AC_INV(0x00),AC_INV(0x00),
    AC_INV(0x00),AC_INV(0x00)
};

static void frf_net_board_map(struct ExpansionBoard *board)
{
    /* Only allocate after autoconfig has assigned Z2 IO address. */
    void *mem = tlsf_malloc_aligned(tlsf,NIC_SIZE,NIC_SIZE);
    if (!mem) {
        kprintf("[FRFNET] ERROR: Cannot allocate 64KiB NIC transport\n");
        return;
    }
    uintptr_t phys=mmu_virt2phys((uintptr_t)mem);
    if (phys==(uintptr_t)-1) {
        kprintf("[FRFNET] ERROR: NIC phys translation failed\n");
        return;
    }
    /* Clear old cache state before uncached aliases are introduced. */
    arm_flush_cache((uintptr_t)mem,NIC_SIZE);
    mmu_map(phys,(uintptr_t)mem,NIC_SIZE,
            MMU_ACCESS|MMU_ISHARE|MMU_ATTR_UNCACHED,0);
    mmu_map(phys,board->map_base,NIC_SIZE,
            MMU_ACCESS|MMU_ISHARE|MMU_ALLOW_EL0|MMU_ATTR_UNCACHED,0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
    const uint8_t no_mac[6]={0x02,0x68,0x00,0x00,0x00,0x01};
    frf_net_bridge_init((struct frf_net_shared *)mem,no_mac);
    __asm__ volatile("dmb ishst" ::: "memory");
    frf_net_shared_host=(struct frf_net_shared *)mem;
    kprintf("[FRFNET] Z2 virtual NIC: $%08x (%u bytes), phys=%p; RADIO DOWN\n",
            board->map_base,NIC_SIZE,(void *)phys);
}

static struct ExpansionBoard frf_net_board = {
    nic_rom,NIC_SIZE,0,0,1,frf_net_board_map
};
static void * __attribute__((used,section(".boards.z2"))) frf_net_board_ref = &frf_net_board;
