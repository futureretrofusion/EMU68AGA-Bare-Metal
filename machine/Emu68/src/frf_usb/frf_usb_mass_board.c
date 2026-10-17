/* SPDX-License-Identifier: GPL-2.0-or-later
 * Dedicated Zorro-II 64 KiB I/O board, product 0x69. Not Fast RAM.
 * Board mapping deliberately allocates a single uncached page that CPU1
 * and the emulated 68k can both access. Do not share USPi pointers.
 */
#include <stdint.h>
#include <string.h>
#include <boards.h>
#include <tlsf.h>
#include <mmu.h>
#include <A64.h>
#include <support.h>
#include "frf_usb_mass.h"

extern void *tlsf;
volatile struct frf_um_shared *frf_um_host;

#define AC_NIB(n)   ((uint16_t)(((n)&15u)<<12))
#define AC_BYTE(b)  AC_NIB((b)>>4), AC_NIB(b)
#define AC_INV(b)   AC_NIB((~(b))>>4), AC_NIB(~(b))

static const uint16_t um_rom[32] = {
    AC_BYTE(0xC1), /* 64 KiB Zorro-II I/O, not memory */
    AC_INV(0x69),  /* different from 0x68 network board */
    AC_INV(0x00), AC_INV(0x00),
    AC_INV(0x6d), AC_INV(0x73), /* local virtual manufacturer */
    AC_INV(0x55),AC_INV(0x53),AC_INV(0x42),AC_INV(0x4d), /* USBM */
    AC_INV(0x00),AC_INV(0x00),AC_INV(0x00),AC_INV(0x00),
    AC_INV(0x00),AC_INV(0x00)
};

static void frf_um_board_map(struct ExpansionBoard *board)
{
    void *mem=tlsf_malloc_aligned(tlsf,FRF_UM_BOARD_BYTES,FRF_UM_BOARD_BYTES);
    if (!mem) {
        kprintf("[USBMASS] No memory for virtual board\n");
        return;
    }
    uintptr_t phys=mmu_virt2phys((uintptr_t)mem);
    if (phys==(uintptr_t)-1) {
        kprintf("[USBMASS] No physical translation\n");
        return;
    }
    /* The two aliases must be uncached; do not leave one cacheable.
     * Entire board is a single shared cache-coherent I/O mailbox. */
    arm_flush_cache((uintptr_t)mem,FRF_UM_BOARD_BYTES);
    mmu_map(phys,(uintptr_t)mem,FRF_UM_BOARD_BYTES,
            MMU_ACCESS|MMU_ISHARE|MMU_ATTR_UNCACHED,0);
    mmu_map(phys,board->map_base,FRF_UM_BOARD_BYTES,
            MMU_ACCESS|MMU_ISHARE|MMU_ALLOW_EL0|MMU_ATTR_UNCACHED,0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
    memset(mem,0,FRF_UM_BOARD_BYTES);
    struct frf_um_shared *s=(struct frf_um_shared *)mem;
    s->magic=FRF_UM_MAGIC;
    s->version=FRF_UM_VERSION;
    s->bytes=sizeof(*s);
    s->sector_size=FRF_UM_SECTOR_SIZE;
    s->state=FRF_UM_IDLE;
    __asm__ volatile("dmb ishst" ::: "memory");
    __atomic_store_n(&frf_um_host,s,__ATOMIC_RELEASE);
    kprintf("[USBMASS] Z2 board: 0x%08x phys=%p read-only\n",
            board->map_base,(void*)phys);
}

static struct ExpansionBoard frf_um_board={
    um_rom,FRF_UM_BOARD_BYTES,0,0,1,frf_um_board_map
};
static void * __attribute__((used,section(".boards.z2")))
    frf_um_board_ref=&frf_um_board;
