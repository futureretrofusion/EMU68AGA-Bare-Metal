/*
 * FRF virtual Zorro III Fast RAM
 * 128 MB, normally autoconfigured at $40000000-$47ffffff.
 */
#include <stdint.h>
#include <boards.h>
#include <mmu.h>
#include <support.h>

#define FRF_Z3RAM_BYTES (128u * 1024u * 1024u)

uintptr_t frf_z3ram_phys = 0;
uint32_t  frf_z3ram_size = FRF_Z3RAM_BYTES;

/* Standard nibble-wired Autoconfig representation.
   Logical configuration:
      A3 11 B0 00 6D 73 00 00 00 01 00 00 00 00 00 00
   A3/B0 describe a 128 MB Zorro III memory board.
 */
#define AC_NIB(n) ((uint16_t)(((n) & 0x0fu) << 12))
#define AC_DIR(b) AC_NIB((b) >> 4), AC_NIB(b)
#define AC_INV(b) AC_NIB((~(b)) >> 4), AC_NIB(~(b))

static uint16_t frf_z3rom[32] = {
    AC_DIR(0xA3),
    AC_INV(0x11),
    AC_INV(0xB0),
    AC_INV(0x00),
    AC_INV(0x6D), AC_INV(0x73),
    AC_INV(0x00), AC_INV(0x00), AC_INV(0x00), AC_INV(0x01),
    AC_INV(0x00), AC_INV(0x00), AC_INV(0x00), AC_INV(0x00),
    AC_INV(0x00), AC_INV(0x00)
};

static void frf_z3ram_map(struct ExpansionBoard *board)
{
    if (!frf_z3ram_phys || !frf_z3ram_size) {
        kprintf("[BOARD] FRF ZIII RAM has no backing memory; ignored\\n");
        return;
    }

    kprintf("[BOARD] Mapping FRF ZIII Fast RAM at %08x-%08x (%u MiB), phys %p\\n",
            board->map_base,
            board->map_base + frf_z3ram_size - 1,
            frf_z3ram_size >> 20,
            frf_z3ram_phys);

    mmu_map(frf_z3ram_phys, board->map_base, frf_z3ram_size,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
}

static struct ExpansionBoard frf_z3_board = {
    frf_z3rom,
    FRF_Z3RAM_BYTES,
    0,
    1,
    1,
    frf_z3ram_map
};

static void * __attribute__((used, section(".boards.z3"))) frf_z3_board_ref = &frf_z3_board;
