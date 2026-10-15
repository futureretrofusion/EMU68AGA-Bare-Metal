/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#ifdef AGA_PISTORM
#include "aga_glue.h"
/* v19-v26 carry the 68k's SR, stack pointers, CACR and FPU control registers
   (M68k.h, REG_*_VN), reserved with -ffixed-vN. GCC 14 ignores that in
   auto-vectorised loops: 1.110's brightness table put all eight to work
   inside the trap and killed the 68k on the spot (2026-09-30). So AGA code
   running in a trap sits between a save and a restore: AGA_TRAP_GUARD()
   copies them, and the cleanup attribute puts them back on every way out of
   the block - even after a return's call has run. */
struct aga_vsave { uint64_t v[16]; } __attribute__((aligned(16)));
static inline void aga_vsave(struct aga_vsave *s)
{
    __asm__ volatile("st1 {v19.2d-v22.2d}, [%0]\n\tst1 {v23.2d-v26.2d}, [%1]"
                     : : "r"(&s->v[0]), "r"(&s->v[8]) : "memory");
}
static inline void aga_vrestore(struct aga_vsave *s)
{
    __asm__ volatile("ld1 {v19.2d-v22.2d}, [%0]\n\tld1 {v23.2d-v26.2d}, [%1]"
                     : : "r"(&s->v[0]), "r"(&s->v[8]) : "memory");
}
#define AGA_TRAP_GUARD() \
    struct aga_vsave aga_vsaved __attribute__((cleanup(aga_vrestore))); \
    aga_vsave(&aga_vsaved)
#endif
#include "A64.h"
#include "config.h"
#include "support.h"
#if defined(FRF_VIRTUAL_AGA) && defined(FRF_VIRTUAL_GAYLE_ABSENT)
#include "frf_native_bus.h"
#endif
#include "mmu.h"
#include "tlsf.h"
#include "M68k.h"
#include "cache.h"
#include "intc.h"

#define FULL_CONTEXT 0

static const int REGMAP[32] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
    18, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, 19, -1
};

#define SAVE_SHORT_CONTEXT \
    "       stp x0, x1, [sp, -10*16]!         \n" \
    "       stp x2, x3, [sp, #1*16]         \n" \
    "       stp x4, x5, [sp, #2*16]         \n" \
    "       stp x6, x7, [sp, #3*16]         \n" \
    "       stp x8, x9, [sp, #4*16]         \n" \
    "       stp x10, x11, [sp, #5*16]       \n" \
    "       stp x12, x13, [sp, #6*16]       \n" \
    "       stp x14, x15, [sp, #7*16]       \n" \
    "       stp x16, x17, [sp, #8*16]       \n" \
    "       stp x18, x30, [sp, #9*16]       \n"

#define SAVE_FULL_CONTEXT \
    "       stp x0, x1, [sp, -256]!         \n" \
    "       stp x2, x3, [sp, #1*16]         \n" \
    "       stp x4, x5, [sp, #2*16]         \n" \
    "       stp x6, x7, [sp, #3*16]         \n" \
    "       stp x8, x9, [sp, #4*16]         \n" \
    "       stp x10, x11, [sp, #5*16]       \n" \
    "       stp x12, x13, [sp, #6*16]       \n" \
    "       stp x14, x15, [sp, #7*16]       \n" \
    "       stp x16, x17, [sp, #8*16]       \n" \
    "       stp x18, x19, [sp, #9*16]       \n" \
    "       stp x20, x21, [sp, #10*16]      \n" \
    "       stp x22, x23, [sp, #11*16]      \n" \
    "       stp x24, x25, [sp, #12*16]      \n" \
    "       stp x26, x27, [sp, #13*16]      \n" \
    "       stp x28, x29, [sp, #14*16]      \n" \
    "       mov x0, sp                      \n" \
    "       stp x30, x0, [sp, #15*16]       \n"

#define LOAD_FULL_CONTEXT \
    "       ldp x2, x3, [sp, #1*16]         \n" \
    "       ldp x4, x5, [sp, #2*16]         \n" \
    "       ldp x6, x7, [sp, #3*16]         \n" \
    "       ldp x8, x9, [sp, #4*16]         \n" \
    "       ldp x10, x11, [sp, #5*16]       \n" \
    "       ldp x12, x13, [sp, #6*16]       \n" \
    "       ldp x14, x15, [sp, #7*16]       \n" \
    "       ldp x16, x17, [sp, #8*16]       \n" \
    "       ldp x18, x19, [sp, #9*16]       \n" \
    "       ldp x20, x21, [sp, #10*16]      \n" \
    "       ldp x22, x23, [sp, #11*16]      \n" \
    "       ldp x24, x25, [sp, #12*16]      \n" \
    "       ldp x26, x27, [sp, #13*16]      \n" \
    "       ldp x28, x29, [sp, #14*16]      \n" \
    "       ldr x30, [sp, #15*16]           \n" \
    "       ldp x0, x1, [sp], #256          \n"

#define LOAD_SHORT_CONTEXT \
    "       ldp x2, x3, [sp, #1*16]         \n" \
    "       ldp x4, x5, [sp, #2*16]         \n" \
    "       ldp x6, x7, [sp, #3*16]         \n" \
    "       ldp x8, x9, [sp, #4*16]         \n" \
    "       ldp x10, x11, [sp, #5*16]       \n" \
    "       ldp x12, x13, [sp, #6*16]       \n" \
    "       ldp x14, x15, [sp, #7*16]       \n" \
    "       ldp x16, x17, [sp, #8*16]       \n" \
    "       ldp x18, x30, [sp, #9*16]       \n" \
    "       ldp x0, x1, [sp], #10*16          \n"

#if FULL_CONTEXT
#define SAVE_CONTEXT    SAVE_FULL_CONTEXT
#define LOAD_CONTEXT    LOAD_FULL_CONTEXT
#else
#define SAVE_CONTEXT    SAVE_SHORT_CONTEXT
#define LOAD_CONTEXT    LOAD_SHORT_CONTEXT
#endif

struct INT_shadow {
    uint16_t INTENA;
    uint16_t INTREQ;
    uint8_t  ARMPending;
} INT_shadow;

void  __attribute__((used)) __stub_vectors()
{ __asm__ volatile(
"       .section .vectors               \n"
"       .balign 0x800                   \n"
"curr_el_sp0_sync:                      \n" // The exception handler for a synchronous 
        SAVE_CONTEXT                        // exception from the current EL using SP0.
"       mov x0, #0                      \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_sp0_irq:                       \n" // The exception handler for an IRQ exception
        SAVE_CONTEXT                        // from the current EL using SP0.
"       mov x0, #0x80                   \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_sp0_fiq:                       \n" // The exception handler for an FIQ exception
        SAVE_CONTEXT                        // from the current EL using SP0.
"       mov x0, #0x100                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_sp0_serror:                    \n" // The exception handler for a System Error 
        SAVE_CONTEXT                        // exception from the current EL using SP0.
"       mov x0, #0x180                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_spx_sync:                      \n" // The exception handler for a synchrous 
        SAVE_CONTEXT                        // exception from the current EL using the
"       mov x0, #0x200                  \n" // current SP.
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_spx_irq:                       \n" // The exception handler for an IRQ exception from 
"       stp x0, x1, [sp, -16]!          \n" // the current EL using the current SP.
"       mrs x0, MPIDR_EL1               \n" // Check CPU core id
"       tst x0, #3                      \n" // if Core ID != 0
"       b.ne 2f                         \n" // Go to SysHandler
"       mrs x0, SPSR_EL1                \n" // Get SPSR
"       orr x0, x0, #0x080              \n" // Disable IRQ interrupt so that we are not disturbed on return
"       msr SPSR_EL1, x0                \n"
"       adrp x1, INT_shadow             \n" // Load INTENA shadow
"       add x1, x1, :lo12:INT_shadow    \n"
"       ldrh w0, [x1, #%[intena]]       \n"
"       and w0, w0, #0x6000             \n" // Check if INTEN and EXTER are active
"       cmp w0, #0x6000                 \n"
"       mov w0, #1                      \n" // Set ARM int pending so that we can fake INTREQ
"       strb w0, [x1, #%[armpend]]      \n"
"       b.ne 1f                         \n" // Skip setting pint register if interrupt was not enabled
"       mov x1, "CTX_POINTER_ASM"       \n" // Load CPU context
"       mov w0, #6                      \n" // Set level 6 IRQ
"       strb w0, [x1, #%[pint]]         \n"
"1:     ldp x0, x1, [sp], #16           \n" // Restore scratch registers
"       eret                            \n"
"2:     ldp x0, x1, [sp], #16           \n"
"       b IRQonOtherCores               \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_spx_fiq:                       \n" // The exception handler for an FIQ from 
"       stp x0, x1, [sp, -16]!          \n" // the current EL using the current SP.
"       mrs x0, MPIDR_EL1               \n" // Check CPU core id
"       tst x0, #3                      \n" // if Core ID != 0
"       b.ne 2b                         \n" // Go to SysHandler
"       mrs x0, SPSR_EL1                \n" // Get SPSR
"       orr x0, x0, #0x0c0              \n" // Disable IRQ and FIQ interrupts so that we are not disturbed on return
"       msr SPSR_EL1, x0                \n"
"       adrp x1, INT_shadow             \n" // Load INTENA shadow
"       add x1, x1, :lo12:INT_shadow    \n"
"       ldrh w0, [x1, #%[intena]]       \n"
"       and w0, w0, #0x6000             \n" // Check if INTEN and EXTER are active
"       cmp w0, #0x6000                 \n"
"       mov w0, #1                      \n" // Set ARM int pending so that we can fake INTREQ
"       strb w0, [x1, #%[armpend]]      \n"
"       b.ne 1f                         \n" // Skip setting pint register if interrupt was not enabled
"       mov x1, "CTX_POINTER_ASM"       \n" // Load CPU context
"       mov w0, #6                      \n" // Set level 6 IRQ
"       strb w0, [x1, #%[pint]]         \n"
"1:     ldp x0, x1, [sp], #16           \n" // Restore scratch registers
"       eret                            \n"
"                                       \n"
"       .balign 0x80                    \n"
"curr_el_spx_serror:                    \n" // The exception handler for a System Error 
"       stp x0, x1, [sp, -16]!          \n" // exception from the current EL using the
"       mrs x0, SPSR_EL1                \n" // Get SPSR
"       orr x0, x0, #0x1c0              \n" // Disable SError, IRQ and FIQ interrupts so that we are not disturbed on return
"       msr SPSR_EL1, x0                \n"
"       mov x1, "CTX_POINTER_ASM"       \n" // Load CPU context
"       mov w0, #7                      \n" // Set level 7 IRQ
"       strb w0, [x1, #%[perr]]         \n"
"       ldp x0, x1, [sp], #16           \n" // Restore scratch registers
"       eret                            \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch64_sync:                 \n" // The exception handler for a synchronous 
        SAVE_CONTEXT                        // exception from a lower EL (AArch64).
"       mov x0, #0x400                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch64_irq:                  \n" // The exception handler for an IRQ from a lower EL
        SAVE_CONTEXT                        // (AArch64).
"       mov x0, #0x480                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch64_fiq:                  \n" // The exception handler for an FIQ from a lower EL
        SAVE_CONTEXT                        // (AArch64).
"       mov x0, #0x500                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch64_serror:               \n" // The exception handler for a System Error 
        SAVE_CONTEXT                        // exception from a lower EL(AArch64).
"       mov x0, #0x580                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch32_sync:                 \n" // The exception handler for a synchronous 
        SAVE_CONTEXT                        // exception from a lower EL (AArch32).
"       mov x0, #0x600                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch32_irq:                  \n" // The exception handler for an IRQ from a lower EL
        SAVE_CONTEXT                        // (AArch32).
"       mov x0, #0x680                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch32_fiq:                  \n" // The exception handler for an FIQ from a lower EL
        SAVE_CONTEXT                        // (AArch32).
"       mov x0, #0x700                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"       .balign 0x80                    \n"
"lower_el_aarch32_serror:               \n" // The exception handler for a System Error 
        SAVE_CONTEXT                        // exception from a lower EL(AArch32).
"       mov x0, #0x780                  \n"
"       mov x1, sp                      \n"
"       bl SYSHandler                   \n"
"       b ExceptionExit                 \n"
"                                       \n"
"IRQonOtherCores:                       \n"
        SAVE_CONTEXT                        // exception from a lower EL(AArch32).
"       mov x0, #0xffff                 \n"
"       mov x1, sp                      \n"
"       bl IRQHandler                   \n"
        // Fallback to exit
"ExceptionExit:                         \n"
        LOAD_CONTEXT
"       eret                            \n"
"                                       \n"
"       .section .text                  \n"
:
:[pint]"i"(__builtin_offsetof(struct M68KState, INTF.ARM)),
 [perr]"i"(__builtin_offsetof(struct M68KState, INTF.ARM_err)),
 [intena]"i"(__builtin_offsetof(struct INT_shadow, INTENA)),
 [armpend]"i"(__builtin_offsetof(struct INT_shadow, ARMPending))

);}

static int getOPsize(uint32_t opcode)
{
    int size = 0;
    switch (opcode & 0xc0000000)
    {
        case 0x00000000:
            size = 1;
            break;
        case 0x40000000:
            size = 2;
            break;
        case 0x80000000:
            size = 4;
            break;
        case 0xc0000000:
            size = 8;
            break;
    }
    return size;
}

#undef D
#define D(x) /* x */

#ifdef PISTORM_ANY_MODEL

#include "ps_protocol.h"

#include <boards.h>

int board_idx;
struct ExpansionBoard **board;

uint32_t rom_mapped = 0;
uint32_t overlay = 1;
uint32_t z2_ram_autoconf = 1;
uint64_t z2_ram_base = 0;

uint32_t swap_df0_with_dfx = 0;
uint32_t spoof_df0_id = 0;

uint32_t move_slow_to_chip = 0;

// Amiga specific registers
enum
{
    CIAAPRA = 0xBFE001,
    CIABPRB = 0xBFD100,

    VPOSR   = 0xDFF004,
    INTENA  = 0xDFF09A,
    INTENAR = 0xDFF01C,
    INTREQ  = 0xDFF09C,
    INTREQR = 0xDFF01E,
};

int block_c0;
extern int zorro_disable;
extern int disable_scsi;
extern int beamcon0_pal_clear;
extern int beamcon0_pal_set;

#ifdef FRF_VIRTUAL_AGA
extern uintptr_t aga_chip_base;
extern uint32_t aga_chip_size;

#define FRF_INTF_PORTS 0x0008u
#define FRF_INTF_EXTER 0x2000u
#define FRF_CIA_ECLOCK_HZ 709379u
#define FRF_CIAA_TOD_HZ 50u
#define FRF_CIAB_TOD_HZ 15600u

struct frf_vcia {
    uint8_t pra, prb, ddra, ddrb;
    uint16_t ta, tb, la, lb;
    uint32_t tod, alarm, tod_latch;
    uint8_t sdr, icr, imask, cra, crb;
    uint8_t tod_latched, tod_running;
};

static struct frf_vcia frf_ciaa = {
    .pra = 0x00, .prb = 0xff, .ta = 0xffff, .tb = 0xffff,
    .la = 0xffff, .lb = 0xffff, .cra = 0x04, .crb = 0x04
};
static struct frf_vcia frf_ciab = {
    .pra = 0x8c, .prb = 0xff, .ta = 0xffff, .tb = 0xffff,
    .la = 0xffff, .lb = 0xffff, .cra = 0x04, .crb = 0x04
};

static inline void frf_vcia_lock(void);
static inline void frf_vcia_unlock(void);
static volatile char frf_vcia_lock_v __attribute__((aligned(64)));
static uint64_t frf_vcia_last_tick, frf_vcia_e_frac;
static uint64_t frf_vcia_a_tod_frac, frf_vcia_b_tod_frac;

#define FRF_KBD_QN 64u
static uint8_t frf_kbd_q[FRF_KBD_QN];
static uint8_t frf_kbd_qr, frf_kbd_qw;
static uint32_t frf_kbd_drop;

/* Real Amiga keyboard protocol state.
   One byte may be outstanding. The computer acknowledges it by switching
   CIA-A CRA bit 6 (SPMODE) to output, holding the SP handshake, then returning
   it to input. The official keyboard protocol requires an 85 us pulse. */
static uint8_t  frf_kbd_wait_ack;
static uint8_t  frf_kbd_ack_active;
static uint64_t frf_kbd_ack_start;
static uint64_t frf_kbd_next_tick;

void frf_vcia_keyboard_raw(uint8_t raw)
{
    frf_vcia_lock();
    uint8_t n = (uint8_t)((frf_kbd_qw + 1u) & (FRF_KBD_QN - 1u));
    if (n == frf_kbd_qr) {
        frf_kbd_qr = (uint8_t)((frf_kbd_qr + 1u) & (FRF_KBD_QN - 1u));
        frf_kbd_drop++;
    }
    frf_kbd_q[frf_kbd_qw] = raw;
    frf_kbd_qw = n;
    frf_vcia_unlock();
}

static inline uint64_t frf_cntvct(void)
{
    uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v;
}
static inline uint64_t frf_cntfrq(void)
{
    uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v;
}
static inline void frf_vcia_lock(void)
{
    if (!__atomic_test_and_set(&frf_vcia_lock_v, __ATOMIC_ACQUIRE)) return;
    __asm__ volatile("sevl");
    while (__atomic_test_and_set(&frf_vcia_lock_v, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
}
static inline void frf_vcia_unlock(void)
{
    __atomic_clear(&frf_vcia_lock_v, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
}

static void frf_vcia_overlay_recalc_locked(void)
{
    uint8_t effective = (uint8_t)(frf_ciaa.pra | (uint8_t)~frf_ciaa.ddra);
    uint32_t next = (effective & 1u) ? 1u : 0u;
    if (next == overlay) return;
    overlay = next;
    if (next) {
        mmu_map(0x00f80000u, 0x000000u, 0x00080000u,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
        kprintf("[JIT:SYS] FRF OVL -> ROM\n");
    } else if (aga_chip_base && aga_chip_size) {
        mmu_map(aga_chip_base, 0x000000u, aga_chip_size,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                MMU_ATTR_CACHED, 0);
        kprintf("[JIT:SYS] FRF OVL -> CHIP (%u KB)\n", aga_chip_size >> 10);
    }
    __asm__ volatile("dsb ish; isb" ::: "memory");
}

static unsigned frf_vcia_timer_advance(uint16_t *counter, uint16_t latch,
                                       uint8_t *cr, uint64_t ticks)
{
    unsigned underflows = 0;
    if (((*cr) & 0x21u) != 0x01u || !ticks) return 0;
    while (ticks) {
        uint64_t span = (uint64_t)(*counter) + 1u;
        if (ticks < span) {
            *counter = (uint16_t)((uint64_t)(*counter) - ticks);
            break;
        }
        ticks -= span;
        underflows++;
        *counter = latch;
        if ((*cr) & 0x08u) {
            *cr &= (uint8_t)~0x01u;
            break;
        }
        if (underflows > 1024u) break;
    }
    return underflows;
}

static int frf_vcia_irq_active(const struct frf_vcia *c)
{
    return (c->icr & c->imask & 0x1fu) != 0u;
}

static void frf_vcia_publish_irq_locked(struct frf_vcia *c, int is_a)
{
    int active = frf_vcia_irq_active(c);
    if (active) c->icr |= 0x80u;
    else        c->icr &= 0x7fu;
    aga_virtual_cia_irq(is_a ? FRF_INTF_PORTS : FRF_INTF_EXTER, active);
}

static void frf_vcia_keyboard_service_locked(void)
{
    uint64_t now = frf_cntvct();

    /* Reading ICR is not the keyboard acknowledgement. Do not present another
       byte until AmigaOS has performed the CIA-A SP handshake. */
    if (frf_kbd_wait_ack || frf_kbd_qr == frf_kbd_qw)
        return;

    if (frf_kbd_next_tick && (int64_t)(now - frf_kbd_next_tick) < 0)
        return;

    uint8_t raw = frf_kbd_q[frf_kbd_qr];
    frf_kbd_qr = (uint8_t)((frf_kbd_qr + 1u) & (FRF_KBD_QN - 1u));

    /* Amiga wire format: rotate key state so release bit is transmitted last,
       then active-low invert into CIA-A SDR. */
    uint8_t wire = (uint8_t)(((raw & 0x7fu) << 1) | ((raw >> 7) & 1u));
    frf_ciaa.sdr = (uint8_t)~wire;
    frf_ciaa.icr |= 0x08u;
    frf_kbd_wait_ack = 1;
    frf_kbd_ack_active = 0;
    frf_vcia_publish_irq_locked(&frf_ciaa, 1);
}

static void frf_vcia_update_chip_locked(struct frf_vcia *c, int is_a, uint64_t ticks)
{
    unsigned au = frf_vcia_timer_advance(&c->ta, c->la, &c->cra, ticks);
    unsigned bu = 0;
    if (au) {
        c->icr |= 0x01u;
        if ((c->crb & 0x61u) == 0x41u || (c->crb & 0x61u) == 0x61u) {
            for (unsigned n = 0; n < au; ++n) {
                if (!(c->crb & 1u)) break;
                if (c->tb == 0u) {
                    c->tb = c->lb; bu++;
                    if (c->crb & 0x08u) { c->crb &= (uint8_t)~1u; break; }
                } else c->tb--;
            }
        }
    }
    if ((c->crb & 0x61u) == 0x01u)
        bu += frf_vcia_timer_advance(&c->tb, c->lb, &c->crb, ticks);
    if (bu) c->icr |= 0x02u;
    if (au || bu) frf_vcia_publish_irq_locked(c, is_a);
}

static void frf_vcia_tod_step_locked(struct frf_vcia *c, int is_a, uint64_t n)
{
    while (n--) {
        c->tod = (c->tod + 1u) & 0x00ffffffu;
        if (c->alarm && c->tod == c->alarm) {
            c->icr |= 0x04u;
            frf_vcia_publish_irq_locked(c, is_a);
        }
    }
}

void frf_vcia_service(void)
{
    uint64_t now = frf_cntvct(), freq = frf_cntfrq();
    frf_vcia_lock();
    if (!frf_vcia_last_tick) {
        frf_vcia_last_tick = now;
        frf_vcia_unlock();
        return;
    }
    uint64_t dt = now - frf_vcia_last_tick;
    frf_vcia_last_tick = now;

    uint64_t q = frf_vcia_e_frac + dt * (uint64_t)FRF_CIA_ECLOCK_HZ;
    uint64_t eticks = q / freq;
    frf_vcia_e_frac = q % freq;
    if (eticks) {
        frf_vcia_update_chip_locked(&frf_ciaa, 1, eticks);
        frf_vcia_update_chip_locked(&frf_ciab, 0, eticks);
    }

    if (frf_ciaa.tod_running) {
        q = frf_vcia_a_tod_frac + dt * (uint64_t)FRF_CIAA_TOD_HZ;
        uint64_t n = q / freq; frf_vcia_a_tod_frac = q % freq;
        if (n) frf_vcia_tod_step_locked(&frf_ciaa, 1, n);
    }
    if (frf_ciab.tod_running) {
        q = frf_vcia_b_tod_frac + dt * (uint64_t)FRF_CIAB_TOD_HZ;
        uint64_t n = q / freq; frf_vcia_b_tod_frac = q % freq;
        if (n) frf_vcia_tod_step_locked(&frf_ciab, 0, n);
    }
    frf_vcia_keyboard_service_locked();
    frf_vcia_unlock();
}

static uint8_t frf_vcia_port_read_locked(struct frf_vcia *c, int is_a, unsigned port)
{
    if (is_a && port == 0u) {
        uint8_t tmp = 0x3cu;
        uint8_t disk_bits;
        tmp |= (uint8_t)((c->pra | (c->ddra ^ 3u)) & 0x03u);
        if (aga_virtual_disk_pra(&disk_bits))
            tmp = (uint8_t)((tmp & (uint8_t)~0x3cu) | (disk_bits & 0x3cu));
        uint8_t ext_buttons = aga_virtual_ciaa_buttons();
        if (c->ddra & 0x40u) tmp = (uint8_t)((tmp & ~0x40u) | (c->pra & 0x40u));
        else tmp = (uint8_t)((tmp & ~0x40u) | (ext_buttons & 0x40u));
        if (c->ddra & 0x80u) tmp = (uint8_t)((tmp & ~0x80u) | (c->pra & 0x80u));
        else tmp = (uint8_t)((tmp & ~0x80u) | (ext_buttons & 0x80u));
        return tmp;
    }
    if (!is_a && port == 0u) return 0u;
    {
        uint8_t data = port ? c->prb : c->pra;
        uint8_t dir  = port ? c->ddrb : c->ddra;
        return (uint8_t)((data & dir) | (0xffu & (uint8_t)~dir));
    }
}

static uint8_t frf_vcia_read_one_locked(int is_a, unsigned r)
{
    struct frf_vcia *c = is_a ? &frf_ciaa : &frf_ciab;
    r &= 15u;
    switch (r) {
    case 0: return frf_vcia_port_read_locked(c, is_a, 0u);
    case 1: return frf_vcia_port_read_locked(c, is_a, 1u);
    case 2: return c->ddra;
    case 3: return c->ddrb;
    case 4: return (uint8_t)c->ta;
    case 5: return (uint8_t)(c->ta >> 8);
    case 6: return (uint8_t)c->tb;
    case 7: return (uint8_t)(c->tb >> 8);
    case 8:
        if (c->tod_latched) { c->tod_latched = 0; return (uint8_t)c->tod_latch; }
        return (uint8_t)c->tod;
    case 9: return (uint8_t)((c->tod_latched ? c->tod_latch : c->tod) >> 8);
    case 10:
        if (!c->tod_latched) { c->tod_latch = c->tod; c->tod_latched = 1; }
        return (uint8_t)(c->tod_latch >> 16);
    case 12: return c->sdr;
    case 13: {
        uint8_t v = c->icr;
        c->icr = 0;
        frf_vcia_publish_irq_locked(c, is_a);
        return v;
    }
    case 14: return c->cra;
    case 15: return c->crb;
    default: return 0;
    }
}

static void frf_vcia_mask_write_locked(struct frf_vcia *c, uint8_t v)
{
    if (v & 0x80u) c->imask |= (uint8_t)(v & 0x1fu);
    else           c->imask &= (uint8_t)~(v & 0x1fu);
}

static void frf_vcia_write_one_locked(int is_a, unsigned r, uint8_t v)
{
    struct frf_vcia *c = is_a ? &frf_ciaa : &frf_ciab;
    r &= 15u;
    switch (r) {
    case 0:
        if (is_a) {
            c->pra = (uint8_t)((c->pra & (uint8_t)~0xc3u) | (v & 0xc3u));
            frf_vcia_overlay_recalc_locked();
        } else c->pra = v;
        break;
    case 1:
        c->prb = v;
        if (!is_a) aga_virtual_disk_prb(v);
        break;
    case 2: c->ddra = v; if (is_a) frf_vcia_overlay_recalc_locked(); break;
    case 3: c->ddrb = v; break;
    case 4: c->la = (uint16_t)((c->la & 0xff00u) | v); break;
    case 5:
        c->la = (uint16_t)((c->la & 0x00ffu) | ((uint16_t)v << 8));
        if (!(c->cra & 1u)) c->ta = c->la;
        if (c->cra & 0x08u) { c->ta = c->la; c->cra |= 1u; }
        break;
    case 6: c->lb = (uint16_t)((c->lb & 0xff00u) | v); break;
    case 7:
        c->lb = (uint16_t)((c->lb & 0x00ffu) | ((uint16_t)v << 8));
        if (!(c->crb & 1u)) c->tb = c->lb;
        if (c->crb & 0x08u) { c->tb = c->lb; c->crb |= 1u; }
        break;
    case 8:
        if (c->crb & 0x80u) c->alarm = (c->alarm & 0xffff00u) | v;
        else { c->tod = (c->tod & 0xffff00u) | v; c->tod_running = 1u; }
        break;
    case 9:
        if (c->crb & 0x80u) c->alarm = (c->alarm & 0xff00ffu) | ((uint32_t)v << 8);
        else c->tod = (c->tod & 0xff00ffu) | ((uint32_t)v << 8);
        break;
    case 10:
        if (c->crb & 0x80u) c->alarm = (c->alarm & 0x00ffffu) | ((uint32_t)v << 16);
        else { c->tod = (c->tod & 0x00ffffu) | ((uint32_t)v << 16); c->tod_running = 0; }
        break;
    case 12: c->sdr = v; break;
    case 13:
        frf_vcia_mask_write_locked(c, v);
        frf_vcia_publish_irq_locked(c, is_a);
        break;
    case 14: {
        uint8_t oldcra = c->cra;
        uint8_t newcra = (uint8_t)(v & 0x7fu);

        if (is_a && frf_kbd_wait_ack) {
            int old_out = (oldcra & 0x40u) != 0;
            int new_out = (newcra & 0x40u) != 0;

            if (!old_out && new_out) {
                /* Host begins the keyboard SP handshake. */
                frf_kbd_ack_active = 1;
                frf_kbd_ack_start = frf_cntvct();
            } else if (old_out && !new_out && frf_kbd_ack_active) {
                /* Handshake complete. Real hardware requires the line low for
                   85 us. Emu68 can execute the guest's delay faster than wall
                   clock, so accept the semantic transition but keep the
                   keyboard unavailable until the 85 us boundary has elapsed. */
                uint64_t freq = frf_cntfrq();
                uint64_t min_ticks = (freq * 85u + 999999u) / 1000000u;
                frf_kbd_next_tick = frf_kbd_ack_start + min_ticks;
                frf_kbd_wait_ack = 0;
                frf_kbd_ack_active = 0;
            }
        }

        c->cra = newcra;
        if (c->cra & 0x10u) { c->cra &= (uint8_t)~0x10u; c->ta = c->la; }
        break;
    }
    case 15:
        c->crb = v;
        if (c->crb & 0x10u) { c->crb &= (uint8_t)~0x10u; c->tb = c->lb; }
        break;
    default: break;
    }
}

static int frf_vcia_decode(uint64_t far, int *which, unsigned *reg)
{
    if ((far & 0xfff000u) == 0xbfe000u) {
        *which = 0; *reg = (unsigned)((far >> 8) & 15u); return 1;
    }
    if ((far & 0xfff000u) == 0xbfd000u) {
        *which = 1; *reg = (unsigned)((far >> 8) & 15u); return 1;
    }
    return 0;
}

static int frf_vcia_write(uint64_t far, int size, uint64_t value)
{
    int which; unsigned reg; uint8_t v;
    if (!frf_vcia_decode(far, &which, &reg)) return 0;
    if (size == 1) v = (uint8_t)value;
    else if (size == 2) v = which ? (uint8_t)(value >> 8) : (uint8_t)value;
    else return 1;
    frf_vcia_lock();
    frf_vcia_write_one_locked(which ? 0 : 1, reg, v);
    frf_vcia_unlock();
    return 1;
}

static int frf_vcia_read(uint64_t far, int size, uint64_t *value)
{
    int which; unsigned reg; uint8_t v;
    if (!frf_vcia_decode(far, &which, &reg)) return 0;
    frf_vcia_lock();
    v = frf_vcia_read_one_locked(which ? 0 : 1, reg);
    frf_vcia_unlock();
    if (size == 1) *value = v;
    else if (size == 2) *value = which ? (((uint16_t)v << 8) | 0x00ffu)
                                       : (0xff00u | v);
    else *value = 0xffffffffffffffffULL;
    return 1;
}
#endif

int SYSWriteValToAddr(uint64_t value, uint64_t value2, int size, uint64_t far)
{
    D(kprintf("[JIT:SYS] SYSWriteValToAddr(0x%x, %d, %p)\n", value, size, far));

    /*
        Allow single wrap around the address space. This provides mirror areas for
        simplified aarch64 pointer arithmetic
    */
    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff) {
        far &= 0xffffffff;
    }

    if ((far >> 32) != 0) {
        kprintf("Write in high memory with far %p, size %d, value %08x\n", far, size, value);
        return 0;
    }
    if (disable_scsi) {
        if (far >= 0x00da0000 && far <= 0x00da7fff) {
            return 1;
        }
    }

    if (far == INTENA) {
        if (value & 0x8000) {
            INT_shadow.INTENA |= value & 0x7fff;
        }
        else {
            INT_shadow.INTENA &= ~(value & 0x7fff);
        }
        if (INT_shadow.ARMPending) {
            struct M68KState *ctx;
            __asm__ volatile("mov %0, "CTX_POINTER_ASM"\n":"=r"(ctx));
            ctx->INTF.ARM = (INT_shadow.INTENA & 0x6000) == 0x6000 ? 0x01 : 0;
        }
    }

    if (far == INTREQ) {
        if (value & 0x8000) {
            INT_shadow.INTREQ |= value & 0x3fff;
        }
        else {
            INT_shadow.INTREQ &= ~(value & 0x3fff);
        }
        if ((value & 0xa000) == 0x2000) {
            struct M68KState *ctx;
            __asm__ volatile("mov %0, "CTX_POINTER_ASM"\n":"=r"(ctx));
            ctx->INTF.ARM = 0;
            INT_shadow.ARMPending = 0;
        }
    }

#ifdef AGA_PISTORM
    /* chip RAM first - the hottest path here by far, a game's memcpy is one of these per longword. The magic registers below all live above it */
    if (aga_chip_write(far, size, value, value2)) return 1;  /* no-MMU: chip RAM to chip_va (sandbox, or $5C0N test) */
    {   AGA_TRAP_GUARD();               /* from here on the 68k's v19-v26 are safe (top of file) */
#ifdef FRF_VIRTUAL_AGA
    if (frf_vcia_write(far, size, value)) return 1;
#ifdef FRF_VIRTUAL_GAYLE_ABSENT
    if (frf_native_bus_write((uint32_t)far, (unsigned)size, value)) return 1;
#endif
#endif
    /* AGA/ECS mode switch request from the agaboot tool, valid in both modes */
    if (far == 0xDFF1F2 && size == 2 && ((value & 0xFFFF) == 0xA6A1 || (value & 0xFFFF) == 0xEC50)) {
        aga_reboot((value & 0xFFFF) == 0xA6A1);
        return 1;
    }
    if (far == 0xDFF1E8 && size == 2) { aga_diag_select((uint16_t)value); return 1; }
    /* agaboot hands over an AllocMem(MEMF_CHIP) buffer for the Paula DMA rings.
       Only before sandbox entry, so it can never be confused with a game
       writing sprite 7's pointer once the virtual chipset is live. */
    if (far == 0xDFF1EA && size == 4 && !aga_sandbox_active) { aga_audio_set_ring((uint32_t)value); return 1; }
    /* agaboot BUSTEST lends a chip RAM buffer; outside the sandbox only, like the ring */
    if (far == 0xDFF1EC && size == 4 && !aga_sandbox_active) { aga_bustest_set_buffer((uint32_t)value); return 1; }
    /* agaboot lends the chip RAM for HAM6 on the RGB port (0 = off); outside the sandbox only, like the ring */
    if (far == 0xDFF1F4 && size == 4 && !aga_sandbox_active) { aga_rgbout_set_block((uint32_t)value); return 1; }
    /* ...and graphics.library's copinit: in a WHDLoad game the RGB port is black while the OS's own display runs from it.
       Long and outside the sandbox only: inside it $1F8 is the virtual chipset's window width (agawindow, a word) */
    if (far == 0xDFF1F8 && size == 4 && !aga_sandbox_active) { aga_rgbout_set_os_list((uint32_t)value); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0B) { aga_program_mode = 0; aga_game_mode = 0; aga_sandbox_enter(); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0C) { aga_program_mode = 0; aga_game_mode = 1; aga_sandbox_enter(); return 1; }
    /* FRF PROGRAM/RUN: sandbox alive before the child, HAM6 deferred to child Copper. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0D) { aga_game_mode = 0; aga_program_mode = 1; if (!aga_sandbox_enter()) aga_program_mode = 0; return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5A10) { aga_program_rtg_surface((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A12) { aga_program_seed_display(); return 1; }
    /* $5B0N: leave bisect level at RUNTIME. As a cmdline option every rung
       cost a boot - card out of the Amiga, into the PC - five damn trips to
       bisect one function. From here, only the rungs that actually kill the
       machine cost one. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5B00) { aga_leave_stop = (int)(value & 0xF); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0F) { aga_sandbox_leave(); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5C00) { aga_redirect_test(value & 0xF); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5D00) { aga_audio_mask(value & 0xF); return 1; }
    /* $5E00 AGA / $5E01 ECS Denise / $5E03 ECS Denise the user forced: which
       chipset the next game gets, from the WHDLoad slave's own AGA flag or the
       user's setting. Sent before the enter word. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5E00 && (value & 3) != 2) { aga_chipset_ecs((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5F00) { aga_overlay_idle = value & 1; aga_video_overlay_enable((value >> 1) & 1); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0xAD00) { aga_disk_command(value & 0xFF); return 1; }
    /* $590N: how much of the frame the full-screen picture shows (bits 0-2) and
       sharp scaling (bit 3) - agaboot ZOOM */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5800) { aga_blit_mode((int)(value & 3)); return 1; }   /* agaboot BLITTIMING */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5810) { aga_beam_mode = (int)(value & 3); return 1; }   /* agaboot BEAM */
    /* $5821 / $5820: the emulated chipset's sound on HDMI as well, on / off (agaboot SOUND, aga_hdmi.c) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5820) { aga_hdmi_enable((int)(value & 1)); return 1; }
    /* $5830 off / $5831 A500 / $5834 A1200: the filters on HDMI sound (agaboot SOUND FILTER, aga_hdmi_filter.h).
       Bits 1 and 15 stay clear: a kernel without this line stores the word as the core's aga_ctrl,
       where those two mean "windowed" and "use the register" */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x5830 && ((value & 7) <= 1 || (value & 7) == 4)) { aga_hdmi_filter((value & 4) ? 2 : (int)(value & 1)); return 1; }
    /* the Sound window's mix: $5840 + n HDMI volume, $5850 + n stereo width, n = bit 0 | bits 2-3 >> 1
       (bit 1 clear, as above); $5860 / $5861 left and right as they are / swapped */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5840) { aga_hdmi_volume((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5850) { aga_audio_stereo((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5860) { aga_audio_swap((int)(value & 1)); return 1; }
    /* the custom crop (arm/aga_crop.h, agaboot RTG Image - Crop...): $58C0 + E5(d) five bits of an edge,
       $5870 + E3(s) it into edge s & 3 of PAL (s < 4) or NTSC, $5880 + E3(s) the crop screen with that
       edge marked; bits 1 and 15 clear, as above */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFC2) == 0x58C0) { aga_video_cut_data((int)((value & 1) | ((value >> 1) & 0x1E))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5870) { aga_video_cut_store((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5880) { aga_video_crop_screen((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    /* AHI (aga-hdmi.audio): $58A0 + E4(n) four more bits of its ring's address, n = bit 0 | bits 2-4 >> 1
       (bit 1 clear, as above); $5895 the ring from that address on, $5894 off */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFE2) == 0x58A0) { aga_hdmi_ahi_nibble((int)((value & 1) | ((value >> 1) & 0xE))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5894) { aga_hdmi_ahi((int)(value & 1)); return 1; }
    /* Paula on HDMI: $5899 band-limited, $5898 classic (agaboot SOUND PAULA) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5898) { aga_hdmi_paula((int)(value & 1)); return 1; }
    /* HDMI sound rate: $5868 + E2(m) (agaboot SOUND RATE) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFA) == 0x5868) { aga_hdmi_rate_mode((int)((value & 1) | ((value >> 1) & 2))); return 1; }
    /* $5891 / $5890: HDMI sync on / off - the HDMI frame follows the emulated one (agaboot RTG Image) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5890) { aga_video_hdmi_sync((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5900) { aga_video_set_zoom((int)(value & 0x7)); aga_video_set_sharp((int)((value >> 3) & 1)); return 1; }
    /* $592L: the look, after $590N - 0 sharp, 1 smooth, 2 TV (blends across).
       $57nn / $56nn: the picture right / down by a signed byte of hires pixels
       / lines. $5931 / $5930: the Position screen on / off (desktop only).
       All agaboot RTG Image; an older kernel ignores them and keeps $590N's. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5920) { aga_video_set_look((int)(value & 3)); return 1; }
    /* $597S: scale, 0 4:3 / 1 integer / 2 fill; $598L: scanlines, 0 off to 4 - agaboot RTG Image */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5970) { aga_video_set_scale((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x5980) { aga_video_set_scanlines((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0x5700) { aga_video_set_pos_x((int8_t)(value & 0xFF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0x5600) { aga_video_set_pos_y((int8_t)(value & 0xFF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5930) { aga_video_calibrate((int)(value & 1)); return 1; }
    /* $5941 / $5940: chip RAM code read past Emu68's software cache from the next game on / off (agaboot CODEDIRECT, A/B) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5940) { aga_set_code_direct((int)(value & 1)); return 1; }
    /* $596N: one timed run of the real bus over that buffer (agaboot BUSTEST) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5960) { aga_bustest_run((int)(value & 3)); return 1; }
    /* $5951 / $5950: the next game on the RGB port only, HDMI black / HDMI as usual (agaboot RGBPORT) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5950) { aga_rgbout_set_hdmi_off((int)(value & 1)); return 1; }
    /* $5953 / $5952: agaboot's RGB port Position frame on / off; $54nn / $55nn: that picture right / down */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5952) { aga_rgbout_calibrate((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFE00) == 0x5400) { aga_rgbout_set_offset((int)((value >> 8) & 1), (int8_t)(value & 0xFF)); return 1; }
    /* $599S: the RGB port picture's size; $59A1 / $59A0: a steady 25 fps, or as many as get through */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5990) { aga_rgbout_set_size((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A0) { aga_rgbout_set_fps25((int)(value & 1)); return 1; }
    /* FRF $59A3/$59A2: DOUBLE/SINGLE physical HAM buffer policy. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A2) { aga_rgbout_set_doublebuf((int)(value & 1)); return 1; }
    /* AGA 1.115: adaptive HAM frame rate; FRF renderer retains dynamic memory allocation. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A8) { aga_rgbout_set_auto((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59AC) { aga_rgbout_set_c32((int)(value & 1)); return 1; }
    /* $59A5 / $59A4: the RGB port's colours dithered into 4 bits (smooth) or rounded (exact) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A4) { aga_rgbout_set_dither((int)(value & 1)); return 1; }
    /* RTG Image, 1.110: $59Bn / $59Cn the TV border across / down (0-10%), $59Dn the flicker
       fixer, $59En scanlines for interlaced screens, $59Fn brightness, $591n PAL / NTSC for the next game */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x59B0) { aga_video_set_border(0, (int)(value & 0xF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x59C0) { aga_video_set_border(1, (int)(value & 0xF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59D0) { aga_flickerfix((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x59E0) { aga_video_set_laced_scanlines((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x59F0) { aga_brightness((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5910) { aga_video_standard((int)(value & 1)); return 1; }
    if (aga_enabled && far >= 0xBFD100 && far < 0xBFD102 && aga_cia_write(far, size, value)) return 1;
    if (aga_sandbox_active && far >= 0xBF0000 && far < 0xC00000) {
        aga_diag_cia((uint32_t)far, 1);
        if (size == 1) aga_diag_cia_value((uint32_t)far, 1, (uint8_t)value);
    }
    if (aga_enabled) {
        if (far >= 0xDFF000 && far < 0xE00000)
            return aga_trap_write(far, size, value, value2);
        if (far >= 0xC00000 && far < 0xD80000)
            return 1;                       /* no trapdoor/slow RAM in AGA mode */
    }
    }   /* AGA_TRAP_GUARD */
#endif
    if (far == 0xdeadbeef && size == 1) {
        kprintf("%c", value);
        return 1;
    }

    if (far >= 0xff000000) {
        kprintf("Z3 write access with far %08x, size %d, value %08x\n", far, size, value);
    }

    if (far >= 0x1000000) {
        return 1; // Unmapped Z3 address
    }

    if (far == CIAAPRA && size == 1) {
        if ((value & 1) != overlay) {
            kprintf("[JIT:SYS] OVL bit changing to %d\n", value & 1);
            overlay = value & 1;
#ifndef FRF_VIRTUAL_AGA
            extern int fast_page0;
#endif

            /* If fast_page_zero is active either map to ROM or to physical ARM RAM at address 0 */
#ifdef FRF_VIRTUAL_AGA
            if (overlay)
                mmu_map(0xf80000, 0x0, 0x80000, MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
            else if (aga_chip_base && aga_chip_size)
                mmu_map(aga_chip_base, 0x0, aga_chip_size, MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
#else
            if (fast_page0)
            {
                if (overlay)
                    mmu_map(0xf80000, 0x0, 4096, MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
                else
                    mmu_map(0x0, 0x0, 4096, MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
            }
#endif
        }
    }

    if (far == CIABPRB) {
        if (swap_df0_with_dfx) {
            const int SEL0_BITNUM = 3;

            if ((value & ((1 << (SEL0_BITNUM + swap_df0_with_dfx)) | 0x80)) == 0x80) {
              // If drive selected but motor off, Amiga is reading drive ID.
              spoof_df0_id = 1;
            } else {
              spoof_df0_id = 0;
            }

            // If the value for SEL0/SELx differ
            if (((value >> SEL0_BITNUM) & 1) != ((value >> (SEL0_BITNUM + swap_df0_with_dfx)) & 1)) {
              // Invert both bits to swap them around
              value ^= ((1 << SEL0_BITNUM) | (1 << (SEL0_BITNUM + swap_df0_with_dfx)));
            }
        }
    }

    if (!zorro_disable && far >= 0xe80000 && far <= 0xe8ffff && board[board_idx])
    {
        if (board[board_idx]->is_z3)
        {
            if (far == 0xe80044) {
                board[board_idx]->map_base = (value & 0xffff) << 16;
                board[board_idx]->map(board[board_idx]);
                board_idx++;
            }
        }
        else
        {
            if (far == 0xe80048) {
                board[board_idx]->map_base = (value & 0xff) << 16;
                board[board_idx]->map(board[board_idx]);
                board_idx++;
            }
        }
        
        if (far == 0xe8004c || far == 0xe8004e) {
            board_idx++;
        }
 
        return 1;
    }

    if (move_slow_to_chip) {
        if (far >= 0x080000 && far <= 0x0FFFFF) {
            // A500 JP2 connects Agnus' A19 input to A23 instead of A19 by default, and decodes trapdoor memory at 0xC00000 instead of 0x080000.
            // We can move the trapdoor to chipram simply by rewriting the address.
            far += 0xB80000;
        } else if (far >= 0xC00000 && far <= 0xC7FFFF) {
            // Block accesses through to trapdoor at slow ram address, otherwise it will be detected at 0x080000 and 0xC00000.
            return 1;
        }
    }

    if ((far >= 0xc00000 && far <= 0xc7ffff) && block_c0)
    {
        return 1;
    }

#ifdef FRF_VIRTUAL_AGA
    return 1;
#endif
    switch(size)
    {
        case 1:
            ps_write_8(far, value);
            break;
        case 2:
            ps_write_16(far, value);
            break;
        case 4:
            ps_write_32(far, value);
            break;
        case 8:
            ps_write_64(far, value);
            break;
        case 16:
        {
            uint128_t val;
            val.hi = value;
            val.lo = value2;
            ps_write_128(far, val);
            break;
        }
    }
    return 1;
}

int SYSReadValFromAddr(uint64_t *value, uint64_t *value2, int size, uint64_t far)
{  
    D(kprintf("[JIT:SYS] SYSReadValFromAddr(%d, %p)\n", size, far));

    /*
        Allow single wrap around the address space. This provides mirror areas for
        simplified aarch64 pointer arithmetic
    */
    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff) {
        far &= 0xffffffff;
    }
#ifdef AGA_PISTORM
    if (aga_chip_read(far, size, value, value2)) return 1;   /* no-MMU: chip RAM from chip_va (sandbox, or $5C0N test) */
    {   AGA_TRAP_GUARD();               /* from here on the 68k's v19-v26 are safe (top of file) */
#ifdef FRF_VIRTUAL_AGA
    if (frf_vcia_read(far, size, value)) return 1;
#ifdef FRF_VIRTUAL_GAYLE_ABSENT
    if (frf_native_bus_read((uint32_t)far, (unsigned)size, value)) return 1;
#endif
#endif
    if (far == 0xDFF1EA && size == 2) { *value = aga_diag_read_word(); return 1; }
    if (aga_enabled && (far == 0xBFE001 || far == 0xBFE000 || far == 0xBFEC01)
        && aga_cia_read(far, size, value)) return 1;
    if (aga_sandbox_active && far >= 0xBF0000 && far < 0xC00000) {
        aga_diag_cia((uint32_t)far, 0);
        if (size == 1) {
            uint64_t v = ps_read_8((unsigned)far);
            aga_diag_cia_value((uint32_t)far, 0, (uint8_t)v);
            *value = v;
            return 1;
        }
    }
    if (aga_enabled) {
        if (far >= 0xDFF000 && far < 0xE00000) {
            /* A real floppy transfer for the sandbox is over when the real
               DSKBLK is up: the track comes in before the program looks at
               INTREQR or DSKBYTR (aga_glue.c, real drives in System wide). */
            if (aga_dskb_pending && ((far & ~1) == INTREQR || (far & ~1) == 0xDFF01A))
                aga_disk_bridge_poll();
            int r = aga_trap_read(far, size, value, value2);
            if ((far & ~1) == INTREQR) {
                /* CIA interrupts stay REAL in the sandbox: the real Paula
                   raises IPL 2/6 and the 68k takes the exception down the
                   normal PiStorm path. But then the handler reads THIS
                   register, and the virtual INTREQ knows nothing about the
                   CIAs - it saw nothing pending, never ran the CIA server,
                   never acknowledged the CIA, and the level stuck: ~1200
                   level-2 exceptions during entry, then not a damn thing for
                   the rest of the session - keyboard and microhz timer dead.
                   (The chipset
                   loop's mirror never fired: on the classic PiStorm INTF.IPL
                   is a 0/1 flag, never the level.) Fold the real PORTS/EXTER
                   bits in here, one bus read per INTREQR read. */
                /* game mode: PORTS only. An EXTER bit a WHDLoad game never
                   enabled makes its handler ack a source that isn't its own. */
#ifdef FRF_VIRTUAL_AGA
                uint16_t real = 0;   /* no real Paula/CIA interrupt source */
#else
                uint16_t real = ps_read_16(0xDFF01E) & 0x2008;   /* EXTER | PORTS */
#endif
                if (aga_sandbox_active) {
                    if (real & 0x2000) aga_diag_inc(AGA_D_EXTER_REAL);
                    if (INT_shadow.ARMPending) aga_diag_inc(AGA_D_EXTER_ARM);
                }
                if (INT_shadow.ARMPending) real |= 0x2000;
                if (size == 2) *value |= real;
                else if (size == 1) *value |= (far & 1) ? (real & 0xFF) : (real >> 8);
                if (aga_sandbox_active) aga_diag_intreqr_val((uint32_t)*value);
            }
            return r;
        }
        if (far >= 0xC00000 && far < 0xD80000) {
            *value = 0;
            return 1;
        }
    }
    }   /* AGA_TRAP_GUARD */
#endif

    if ((far >> 32) != 0) {
        kprintf("Read from high memory with far %p, size %d\n", far, size);
        return 0;
    }

    if (disable_scsi) {
        if (far >= 0x00da0000 && far <= 0x00da7fff) {
            if (value) { *value = 0xffffffff; }
            if (value2) { *value2 = 0xffffffff; }
            return 1;
        }
    }

    if (far >= 0x1000000) {
        // Unmapped Z3 address

        static const uint64_t u64[5] = {
            0xBAD00BAD00BAD00BULL, 0xAD00BAD00BAD00BAULL,
            0x00BAD00BAD00BAD0ULL, 0x0BAD00BAD00BAD00ULL
        };
        uintptr_t p64 = (uintptr_t)&u64[2] - (15 - (far & 15));

        switch (size)
        {
            case 1:
                *value = *(uint8_t*)p64;
                break;
            case 2:
                *value = *(uint16_t*)p64;
                break;
            case 4:
                *value = *(uint32_t*)p64;
                break;
            case 8:
                *value = *(uint64_t*)p64;
                break;
            case 16:
                *value = *(uint64_t*)p64;
                *value2 = *(uint64_t*)(p64 + 8);
                break;
        }

        return 1;
    }

    if ((far >= 0xc00000 && far <= 0xc7ffff) && block_c0)
    {
        *value = 0;
        return 1;
    }

    if (!zorro_disable && far >= 0xe80000 && far <= 0xe8ffff && size == 1)
    {
        while(board[board_idx] && !board[board_idx]->enabled) {
            board_idx++;
        }

        if (board[board_idx])
        {
            
            const uint8_t *rom = board[board_idx]->rom_file;
            *value = rom[far - 0xe80000];

            return 1;
        }
    }

    if (rom_mapped && overlay)
    {
        if (far < 0x80000)
        {
            switch (size)
            {
                case 1:
                    *value = *(uint8_t*)(0xffffff9000e00000 + far);
                    break;
                case 2:
                    *value = *(uint16_t*)(0xffffff9000e00000 + far);
                    break;
                case 4:
                    *value = *(uint32_t*)(0xffffff9000e00000 + far);
                    break;
                case 8:
                    *value = *(uint64_t*)(0xffffff9000e00000 + far);
                    break;
            }

            return 1;
        }
    }

    if (move_slow_to_chip) {
        if (far >= 0x080000 && far <= 0x0FFFFF) {
            // A500 JP2 connects Agnus' A19 input to A23 instead of A19 by default, and decodes trapdoor memory at 0xC00000 instead of 0x080000.
            // We can move the trapdoor to chipram simply by rewriting the address.
            far += 0xB80000;
        } else if (far >= 0xC00000 && far <= 0xC7FFFF) {
            // Block accesses through to trapdoor at slow ram address, otherwise it will be detected at 0x080000 and 0xC00000.
            *value = 0;
            return 1;
        }
    }

#ifdef FRF_VIRTUAL_AGA
    if (value) {
        if (size == 1) *value = 0xffu;
        else if (size == 2) *value = 0xffffu;
        else if (size == 4) *value = 0xffffffffu;
        else *value = 0xffffffffffffffffULL;
    }
    if (value2) *value2 = 0xffffffffffffffffULL;
    return 1;
#endif
    switch(size)
    {
        case 1:
            *value = ps_read_8(far);
            break;
        case 2:
            *value = ps_read_16(far);
            break;
        case 4:
            *value = ps_read_32(far);
            break;
        case 8:
            *value = ps_read_64(far);
            break;
        case 16:
        {
            uint128_t v = ps_read_128(far);
            *value = v.hi;
            *value2 = v.lo;
        }
    }

    if ((far & ~1) == INTENAR) {
        if (size == 2)
            INT_shadow.INTENA = *value;
        else {
            if (far & 1) {
                INT_shadow.INTENA = (INT_shadow.INTENA & 0xff00) | (*value & 0xff);
            }
            else {
                INT_shadow.INTENA = (INT_shadow.INTENA & 0x00ff) | ((*value & 0xff) << 8);
            }
        }
    }

    if ((far & ~1) == INTREQR) {
        if (size == 2)
        {
            INT_shadow.INTREQ = *value;
            if (INT_shadow.ARMPending)
                *value |= 0x2000;
        }
        else {
            if (far & 1) {
                INT_shadow.INTREQ = (INT_shadow.INTREQ & 0xff00) | (*value & 0xff);
            }
            else {
                INT_shadow.INTREQ = (INT_shadow.INTREQ & 0x00ff) | ((*value & 0xff) << 8);
                if (INT_shadow.ARMPending)
                    *value |= 0x20;
            }
        }
    }

    if (far == CIAAPRA) {
        if (swap_df0_with_dfx && spoof_df0_id) {
            // DF0 doesn't emit a drive type ID on RDY pin
            // If swapping DF0 with DF1-3 we need to provide this ID so that DF0 continues to function.
            *value = (*value & 0xDF); // Spoof drive id for swapped DF0 by setting RDY low
        }
    }

    if (far == CIABPRB) {
        if (swap_df0_with_dfx) {
            const int SEL0_BITNUM = 3;

            // SEL0 = 0x80, SEL1 = 0x10, SEL2 = 0x20, SEL3 = 0x40
            // If the value for SEL0/SELx differ
            if (((*value >> SEL0_BITNUM) & 1) != ((*value >> (SEL0_BITNUM + swap_df0_with_dfx)) & 1)) {
              // Invert both bits to swap them around
              *value ^= ((1 << SEL0_BITNUM) | (1 << (SEL0_BITNUM + swap_df0_with_dfx)));
            }
        }
    }

    if (far == VPOSR && size == 2)
    {
        if (beamcon0_pal_set) {
            *value &= 0xef00;
        }
        else if (beamcon0_pal_clear) {
            *value |= 0x1000;
        }
    } 

    return 1;
}

#else

int SYSWriteValToAddr(uint64_t value, uint64_t value2, int size, uint64_t far)
{
    D(kprintf("[JIT:SYS] SYSWriteValToAddr(0x%x, %d, %p)\n", value, size, far));
    
#ifdef AGA_PISTORM
    {   AGA_TRAP_GUARD();               /* from here on the 68k's v19-v26 are safe (top of file) */
#ifdef FRF_VIRTUAL_AGA
    if (frf_vcia_write(far, size, value)) return 1;
#ifdef FRF_VIRTUAL_GAYLE_ABSENT
    if (frf_native_bus_write((uint32_t)far, (unsigned)size, value)) return 1;
#endif
#endif
    /* AGA/ECS mode switch request from the agaboot tool, valid in both modes */
    if (far == 0xDFF1F2 && size == 2 && ((value & 0xFFFF) == 0xA6A1 || (value & 0xFFFF) == 0xEC50)) {
        aga_reboot((value & 0xFFFF) == 0xA6A1);
        return 1;
    }
    if (far == 0xDFF1E8 && size == 2) { aga_diag_select((uint16_t)value); return 1; }
    /* agaboot hands over an AllocMem(MEMF_CHIP) buffer for the Paula DMA rings.
       Only before sandbox entry, so it can never be confused with a game
       writing sprite 7's pointer once the virtual chipset is live. */
    if (far == 0xDFF1EA && size == 4 && !aga_sandbox_active) { aga_audio_set_ring((uint32_t)value); return 1; }
    /* agaboot BUSTEST lends a chip RAM buffer; outside the sandbox only, like the ring */
    if (far == 0xDFF1EC && size == 4 && !aga_sandbox_active) { aga_bustest_set_buffer((uint32_t)value); return 1; }
    /* agaboot lends the chip RAM for HAM6 on the RGB port (0 = off); outside the sandbox only, like the ring */
    if (far == 0xDFF1F4 && size == 4 && !aga_sandbox_active) { aga_rgbout_set_block((uint32_t)value); return 1; }
    /* ...and graphics.library's copinit: in a WHDLoad game the RGB port is black while the OS's own display runs from it.
       Long and outside the sandbox only: inside it $1F8 is the virtual chipset's window width (agawindow, a word) */
    if (far == 0xDFF1F8 && size == 4 && !aga_sandbox_active) { aga_rgbout_set_os_list((uint32_t)value); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0B) { aga_program_mode = 0; aga_game_mode = 0; aga_sandbox_enter(); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0C) { aga_program_mode = 0; aga_game_mode = 1; aga_sandbox_enter(); return 1; }
    /* FRF PROGRAM/RUN: sandbox alive before the child, HAM6 deferred to child Copper. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0D) { aga_game_mode = 0; aga_program_mode = 1; if (!aga_sandbox_enter()) aga_program_mode = 0; return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5A10) { aga_program_rtg_surface((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A12) { aga_program_seed_display(); return 1; }
    /* $5B0N: leave bisect level at RUNTIME. As a cmdline option every rung
       cost a boot - card out of the Amiga, into the PC - five damn trips to
       bisect one function. From here, only the rungs that actually kill the
       machine cost one. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5B00) { aga_leave_stop = (int)(value & 0xF); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFF) == 0x5A0F) { aga_sandbox_leave(); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5C00) { aga_redirect_test(value & 0xF); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5D00) { aga_audio_mask(value & 0xF); return 1; }
    /* $5E00 AGA / $5E01 ECS Denise / $5E03 ECS Denise the user forced: which
       chipset the next game gets, from the WHDLoad slave's own AGA flag or the
       user's setting. Sent before the enter word. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5E00 && (value & 3) != 2) { aga_chipset_ecs((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5F00) { aga_overlay_idle = value & 1; aga_video_overlay_enable((value >> 1) & 1); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0xAD00) { aga_disk_command(value & 0xFF); return 1; }
    /* $590N: how much of the frame the full-screen picture shows (bits 0-2) and
       sharp scaling (bit 3) - agaboot ZOOM */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5800) { aga_blit_mode((int)(value & 3)); return 1; }   /* agaboot BLITTIMING */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5810) { aga_beam_mode = (int)(value & 3); return 1; }   /* agaboot BEAM */
    /* $5821 / $5820: the emulated chipset's sound on HDMI as well, on / off (agaboot SOUND, aga_hdmi.c) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5820) { aga_hdmi_enable((int)(value & 1)); return 1; }
    /* $5830 off / $5831 A500 / $5834 A1200: the filters on HDMI sound (agaboot SOUND FILTER, aga_hdmi_filter.h).
       Bits 1 and 15 stay clear: a kernel without this line stores the word as the core's aga_ctrl,
       where those two mean "windowed" and "use the register" */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x5830 && ((value & 7) <= 1 || (value & 7) == 4)) { aga_hdmi_filter((value & 4) ? 2 : (int)(value & 1)); return 1; }
    /* the Sound window's mix: $5840 + n HDMI volume, $5850 + n stereo width, n = bit 0 | bits 2-3 >> 1
       (bit 1 clear, as above); $5860 / $5861 left and right as they are / swapped */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5840) { aga_hdmi_volume((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5850) { aga_audio_stereo((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5860) { aga_audio_swap((int)(value & 1)); return 1; }
    /* the custom crop (arm/aga_crop.h, agaboot RTG Image - Crop...): $58C0 + E5(d) five bits of an edge,
       $5870 + E3(s) it into edge s & 3 of PAL (s < 4) or NTSC, $5880 + E3(s) the crop screen with that
       edge marked; bits 1 and 15 clear, as above */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFC2) == 0x58C0) { aga_video_cut_data((int)((value & 1) | ((value >> 1) & 0x1E))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5870) { aga_video_cut_store((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF2) == 0x5880) { aga_video_crop_screen((int)((value & 1) | ((value >> 1) & 6))); return 1; }
    /* AHI (aga-hdmi.audio): $58A0 + E4(n) four more bits of its ring's address, n = bit 0 | bits 2-4 >> 1
       (bit 1 clear, as above); $5895 the ring from that address on, $5894 off */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFE2) == 0x58A0) { aga_hdmi_ahi_nibble((int)((value & 1) | ((value >> 1) & 0xE))); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5894) { aga_hdmi_ahi((int)(value & 1)); return 1; }
    /* Paula on HDMI: $5899 band-limited, $5898 classic (agaboot SOUND PAULA) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5898) { aga_hdmi_paula((int)(value & 1)); return 1; }
    /* HDMI sound rate: $5868 + E2(m) (agaboot SOUND RATE) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFA) == 0x5868) { aga_hdmi_rate_mode((int)((value & 1) | ((value >> 1) & 2))); return 1; }
    /* $5891 / $5890: HDMI sync on / off - the HDMI frame follows the emulated one (agaboot RTG Image) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5890) { aga_video_hdmi_sync((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x5900) { aga_video_set_zoom((int)(value & 0x7)); aga_video_set_sharp((int)((value >> 3) & 1)); return 1; }
    /* $592L: the look, after $590N - 0 sharp, 1 smooth, 2 TV (blends across).
       $57nn / $56nn: the picture right / down by a signed byte of hires pixels
       / lines. $5931 / $5930: the Position screen on / off (desktop only).
       All agaboot RTG Image; an older kernel ignores them and keeps $590N's. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5920) { aga_video_set_look((int)(value & 3)); return 1; }
    /* $597S: scale, 0 4:3 / 1 integer / 2 fill; $598L: scanlines, 0 off to 4 - agaboot RTG Image */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5970) { aga_video_set_scale((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x5980) { aga_video_set_scanlines((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0x5700) { aga_video_set_pos_x((int8_t)(value & 0xFF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFF00) == 0x5600) { aga_video_set_pos_y((int8_t)(value & 0xFF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5930) { aga_video_calibrate((int)(value & 1)); return 1; }
    /* $5941 / $5940: chip RAM code read past Emu68's software cache from the next game on / off (agaboot CODEDIRECT, A/B) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5940) { aga_set_code_direct((int)(value & 1)); return 1; }
    /* $596N: one timed run of the real bus over that buffer (agaboot BUSTEST) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5960) { aga_bustest_run((int)(value & 3)); return 1; }
    /* $5951 / $5950: the next game on the RGB port only, HDMI black / HDMI as usual (agaboot RGBPORT) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5950) { aga_rgbout_set_hdmi_off((int)(value & 1)); return 1; }
    /* $5953 / $5952: agaboot's RGB port Position frame on / off; $54nn / $55nn: that picture right / down */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5952) { aga_rgbout_calibrate((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFE00) == 0x5400) { aga_rgbout_set_offset((int)((value >> 8) & 1), (int8_t)(value & 0xFF)); return 1; }
    /* $599S: the RGB port picture's size; $59A1 / $59A0: a steady 25 fps, or as many as get through */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFC) == 0x5990) { aga_rgbout_set_size((int)(value & 3)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A0) { aga_rgbout_set_fps25((int)(value & 1)); return 1; }
    /* FRF $59A3/$59A2: DOUBLE/SINGLE physical HAM buffer policy. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A2) { aga_rgbout_set_doublebuf((int)(value & 1)); return 1; }
    /* AGA 1.115: adaptive HAM frame rate; FRF renderer retains dynamic memory allocation. */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A8) { aga_rgbout_set_auto((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59AC) { aga_rgbout_set_c32((int)(value & 1)); return 1; }
    /* $59A5 / $59A4: the RGB port's colours dithered into 4 bits (smooth) or rounded (exact) */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59A4) { aga_rgbout_set_dither((int)(value & 1)); return 1; }
    /* RTG Image, 1.110: $59Bn / $59Cn the TV border across / down (0-10%), $59Dn the flicker
       fixer, $59En scanlines for interlaced screens, $59Fn brightness, $591n PAL / NTSC for the next game */
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x59B0) { aga_video_set_border(0, (int)(value & 0xF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF0) == 0x59C0) { aga_video_set_border(1, (int)(value & 0xF)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x59D0) { aga_flickerfix((int)(value & 1)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x59E0) { aga_video_set_laced_scanlines((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFF8) == 0x59F0) { aga_brightness((int)(value & 7)); return 1; }
    if (far == 0xDFF1F2 && size == 2 && (value & 0xFFFE) == 0x5910) { aga_video_standard((int)(value & 1)); return 1; }
    if (aga_enabled && far >= 0xBFD100 && far < 0xBFD102 && aga_cia_write(far, size, value)) return 1;
    if (aga_sandbox_active && far >= 0xBF0000 && far < 0xC00000) {
        aga_diag_cia((uint32_t)far, 1);
        if (size == 1) aga_diag_cia_value((uint32_t)far, 1, (uint8_t)value);
    }
    if (aga_enabled) {
        if (far >= 0xDFF000 && far < 0xE00000)
            return aga_trap_write(far, size, value, value2);
        if (far >= 0xC00000 && far < 0xD80000)
            return 1;                       /* no trapdoor/slow RAM in AGA mode */
    }
    }   /* AGA_TRAP_GUARD */
#endif
    if (far == 0xdeadbeef && size == 1) {
        kprintf("%c", value);
        return 1;
    }

    switch(size)
    {
        case 1:
            *(uint8_t*)(far + 0xffffff9000000000) = value;
            break;
        case 2:
            *(uint16_t*)(far + 0xffffff9000000000) = value;
            break;
        case 4:
            *(uint32_t*)(far + 0xffffff9000000000) = value;
            break;
        case 8:
            *(uint64_t*)(far + 0xffffff9000000000) = value;
            break;
        case 16:
            *(uint64_t*)(far + 0xffffff9000000000) = value;
            *(uint64_t*)(far + 0xffffff9000000008) = value2;
            break;
    }

    return 1;
}

int SYSReadValFromAddr(uint64_t *value, uint64_t *value2, int size, uint64_t far)
{
    D(kprintf("[JIT:SYS] SYSReadValFromAddr(%d, %p)\n", size, far));
    
    switch(size)
    {
        case 1:
            *value = *(uint8_t*)(far + 0xffffff9000000000);
            break;
        case 2:
            *value = *(uint16_t*)(far + 0xffffff9000000000);
            break;
        case 4:
            *value = *(uint32_t*)(far + 0xffffff9000000000);
            break;
        case 8:
            *value = *(uint64_t*)(far + 0xffffff9000000000);
            break;
        case 16:
            *value = *(uint64_t*)(far + 0xffffff9000000000);
            *value2 = *(uint64_t*)(far + 0xffffff9000000008);
            break;
    }

    return 1;
}
#endif

#undef D
#define D(x) /* x  */

#define DWARN(x) /* x */

int SYSValidateUnit(uint32_t vector, uint64_t *ctx, uint64_t elr, uint64_t spsr, uint64_t esr, uint64_t far)
{
    (void)vector;
    (void)ctx;
    (void)elr;
    (void)spsr;
    (void)esr;
    struct M68KTranslationUnit *unit;
    uint16_t *m68k_pc;
    uintptr_t corrected_far = far | (0xff00000000000000ULL);    // Fix the topmost bits
    uintptr_t unit_far = corrected_far & ~0x0000001000000000;   // Clear the executable region bit

    /* Adjust far of the M68KTranslationUnit */
    unit_far = unit_far - __builtin_offsetof(struct M68KTranslationUnit, mt_ARMCode);

    unit = (void*)unit_far;

    m68k_pc = (void*)(uintptr_t)unit->mt_M68kAddress;

    /* Check the unit. The function will free entry if unit was wrong */
    unit = M68K_VerifyUnit(unit);

    if (unit)
    {
        unit->mt_ARMEntryPoint = (void*)corrected_far;
        elr = corrected_far;
        __asm__ volatile("msr ELR_EL1, %0"::"r"(elr));
        return 1;
    }
    else
    {
        unit = M68K_GetTranslationUnit(m68k_pc);
        
        if (unit)
        {
            // Put simple return function to elr
            __asm__ volatile("msr ELR_EL1, %0"::"r"(unit->mt_ARMEntryPoint));

            // Avoid short loop path by invalidating the "last m68k PC" counter. That should trigger full search and translation
            __asm__ volatile("mov "CTX_LAST_PC_ASM", %w0"::"r"(0xffffffff));
            return 1;
        }
    }

    return 0;
}

uint32_t get_fpn_as_single(int fpn) {
    uint32_t ret = 0;

    switch (fpn) {
        case 0:
            __asm__ volatile("mov %w0, v0.s[0]":"=r"(ret));
            break;

        case 1:
            __asm__ volatile("mov %w0, v1.s[0]":"=r"(ret));
            break;

        case 2:
            __asm__ volatile("mov %w0, v2.s[0]":"=r"(ret));
            break;

        case 3:
            __asm__ volatile("mov %w0, v3.s[0]":"=r"(ret));
            break;

        case 4:
            __asm__ volatile("mov %w0, v4.s[0]":"=r"(ret));
            break;

        case 5:
            __asm__ volatile("mov %w0, v5.s[0]":"=r"(ret));
            break;

        case 6:
            __asm__ volatile("mov %w0, v6.s[0]":"=r"(ret));
            break;

        case 7:
            __asm__ volatile("mov %w0, v7.s[0]":"=r"(ret));
            break;

        case 8:
            __asm__ volatile("mov %w0, v8.s[0]":"=r"(ret));
            break;

        case 9:
            __asm__ volatile("mov %w0, v9.s[0]":"=r"(ret));
            break;

        case 10:
            __asm__ volatile("mov %w0, v10.s[0]":"=r"(ret));
            break;

        case 11:
            __asm__ volatile("mov %w0, v11.s[0]":"=r"(ret));
            break;

        case 12:
            __asm__ volatile("mov %w0, v12.s[0]":"=r"(ret));
            break;

        case 13:
            __asm__ volatile("mov %w0, v13.s[0]":"=r"(ret));
            break;

        case 14:
            __asm__ volatile("mov %w0, v14.s[0]":"=r"(ret));
            break;

        case 15:
            __asm__ volatile("mov %w0, v15.s[0]":"=r"(ret));
            break;

        default:
            break;
    }

    return ret;
}

uint64_t get_fpn_as_double(int fpn) {
    uint64_t ret = 0;

    switch (fpn) {
        case 0:
            __asm__ volatile("mov %0, v0.d[0]":"=r"(ret));
            break;

        case 1:
            __asm__ volatile("mov %0, v1.d[0]":"=r"(ret));
            break;

        case 2:
            __asm__ volatile("mov %0, v2.d[0]":"=r"(ret));
            break;

        case 3:
            __asm__ volatile("mov %0, v3.d[0]":"=r"(ret));
            break;

        case 4:
            __asm__ volatile("mov %0, v4.d[0]":"=r"(ret));
            break;

        case 5:
            __asm__ volatile("mov %0, v5.d[0]":"=r"(ret));
            break;

        case 6:
            __asm__ volatile("mov %0, v6.d[0]":"=r"(ret));
            break;

        case 7:
            __asm__ volatile("mov %0, v7.d[0]":"=r"(ret));
            break;

        case 8:
            __asm__ volatile("mov %0, v8.d[0]":"=r"(ret));
            break;

        case 9:
            __asm__ volatile("mov %0, v9.d[0]":"=r"(ret));
            break;

        case 10:
            __asm__ volatile("mov %0, v10.d[0]":"=r"(ret));
            break;

        case 11:
            __asm__ volatile("mov %0, v11.d[0]":"=r"(ret));
            break;

        case 12:
            __asm__ volatile("mov %0, v12.d[0]":"=r"(ret));
            break;

        case 13:
            __asm__ volatile("mov %0, v13.d[0]":"=r"(ret));
            break;

        case 14:
            __asm__ volatile("mov %0, v14.d[0]":"=r"(ret));
            break;

        case 15:
            __asm__ volatile("mov %0, v15.d[0]":"=r"(ret));
            break;

        default:
            break;
    }

    return ret;
}

void set_fpn_as_single(int fpn, uint32_t value) {

    switch (fpn) {
        case 0:
            __asm__ volatile("mov v0.s[0], %w0"::"r"(value));
            break;

        case 1:
            __asm__ volatile("mov v1.s[0], %w0"::"r"(value));
            break;

        case 2:
            __asm__ volatile("mov v2.s[0], %w0"::"r"(value));
            break;

        case 3:
            __asm__ volatile("mov v3.s[0], %w0"::"r"(value));
            break;

        case 4:
            __asm__ volatile("mov v4.s[0], %w0"::"r"(value));
            break;

        case 5:
            __asm__ volatile("mov v5.s[0], %w0"::"r"(value));
            break;

        case 6:
            __asm__ volatile("mov v6.s[0], %w0"::"r"(value));
            break;

        case 7:
            __asm__ volatile("mov v7.s[0], %w0"::"r"(value));
            break;

        case 8:
            __asm__ volatile("mov v8.s[0], %w0"::"r"(value));
            break;

        case 9:
            __asm__ volatile("mov v9.s[0], %w0"::"r"(value));
            break;

        case 10:
            __asm__ volatile("mov v10.s[0], %w0"::"r"(value));
            break;

        case 11:
            __asm__ volatile("mov v11.s[0], %w0"::"r"(value));
            break;

        case 12:
            __asm__ volatile("mov v12.s[0], %w0"::"r"(value));
            break;

        case 13:
            __asm__ volatile("mov v13.s[0], %w0"::"r"(value));
            break;

        case 14:
            __asm__ volatile("mov v14.s[0], %w0"::"r"(value));
            break;

        case 15:
            __asm__ volatile("mov v15.s[0], %w0"::"r"(value));
            break;

        default:
            break;
    }
}

void set_fpn_as_double(int fpn, uint64_t value) {

    switch (fpn) {
        case 0:
            __asm__ volatile("mov v0.d[0], %0"::"r"(value));
            break;

        case 1:
            __asm__ volatile("mov v1.d[0], %0"::"r"(value));
            break;

        case 2:
            __asm__ volatile("mov v2.d[0], %0"::"r"(value));
            break;

        case 3:
            __asm__ volatile("mov v3.d[0], %0"::"r"(value));
            break;

        case 4:
            __asm__ volatile("mov v4.d[0], %0"::"r"(value));
            break;

        case 5:
            __asm__ volatile("mov v5.d[0], %0"::"r"(value));
            break;

        case 6:
            __asm__ volatile("mov v6.d[0], %0"::"r"(value));
            break;

        case 7:
            __asm__ volatile("mov v7.d[0], %0"::"r"(value));
            break;

        case 8:
            __asm__ volatile("mov v8.d[0], %0"::"r"(value));
            break;

        case 9:
            __asm__ volatile("mov v9.d[0], %0"::"r"(value));
            break;

        case 10:
            __asm__ volatile("mov v10.d[0], %0"::"r"(value));
            break;

        case 11:
            __asm__ volatile("mov v11.d[0], %0"::"r"(value));
            break;

        case 12:
            __asm__ volatile("mov v12.d[0], %0"::"r"(value));
            break;

        case 13:
            __asm__ volatile("mov v13.d[0], %0"::"r"(value));
            break;

        case 14:
            __asm__ volatile("mov v14.d[0], %0"::"r"(value));
            break;

        case 15:
            __asm__ volatile("mov v15.d[0], %0"::"r"(value));
            break;

        default:
            break;
    }
}


static inline void SYSPutValueToReg(uint64_t value, uint8_t regNr, uint64_t *ctx)
{
#if FULL_CONTEXT
    ctx[regNr] = value;
#else
    switch(regNr)
    {
        case 19:
            __asm__ volatile("mov x19, %0"::"r"(value));
            break;
        case 20:
            __asm__ volatile("mov x20, %0"::"r"(value));
            break;
        case 21:
            __asm__ volatile("mov x21, %0"::"r"(value));
            break;
        case 22:
            __asm__ volatile("mov x22, %0"::"r"(value));
            break;
        case 23:
            __asm__ volatile("mov x23, %0"::"r"(value));
            break;
        case 24:
            __asm__ volatile("mov x24, %0"::"r"(value));
            break;
        case 25:
            __asm__ volatile("mov x25, %0"::"r"(value));
            break;
        case 26:
            __asm__ volatile("mov x26, %0"::"r"(value));
            break;
        case 27:
            __asm__ volatile("mov x27, %0"::"r"(value));
            break;
        case 28:
            __asm__ volatile("mov x28, %0"::"r"(value));
            break;
        case 29:
            __asm__ volatile("mov x29, %0"::"r"(value));
            break;
        default:
            ctx[REGMAP[regNr]] = value;
            break;
    }
#endif
}

static inline uint64_t SYSGetValueFromReg(uint8_t regNr, uint64_t *ctx)
{
    uint64_t value;
#if FULL_CONTEXT
    value = ctx[regNr];
#else
    switch(regNr)
    {
        case 19:
            __asm__ volatile("mov %0, x19":"=r"(value));
            break;
        case 20:
            __asm__ volatile("mov %0, x20":"=r"(value));
            break;
        case 21:
            __asm__ volatile("mov %0, x21":"=r"(value));
            break;
        case 22:
            __asm__ volatile("mov %0, x22":"=r"(value));
            break;
        case 23:
            __asm__ volatile("mov %0, x23":"=r"(value));
            break;
        case 24:
            __asm__ volatile("mov %0, x24":"=r"(value));
            break;
        case 25:
            __asm__ volatile("mov %0, x25":"=r"(value));
            break;
        case 26:
            __asm__ volatile("mov %0, x26":"=r"(value));
            break;
        case 27:
            __asm__ volatile("mov %0, x27":"=r"(value));
            break;
        case 28:
            __asm__ volatile("mov %0, x28":"=r"(value));
            break;
        case 29:
            __asm__ volatile("mov %0, x29":"=r"(value));
            break;
        default:
            value = ctx[REGMAP[regNr]];
            break;
    }
#endif
    return value;
}

int SYSPageFaultWriteHandler(uint32_t vector, uint64_t *ctx, uint64_t elr, uint64_t spsr, uint64_t esr, uint64_t far)
{
    int handled = 0;
    int size = 0;
    uint64_t value = 0;
    uint32_t opcode = LE32(*(uint32_t *)elr);
    (void)vector;
    (void)spsr;

    if (esr & (1 << 24))
    {
        kprintf("PageFault with valid instruction syndrome: %08x\n", esr);
    }

    size = getOPsize(opcode);

    D(kprintf("[JIT:SYS] Fage fault: opcode %08x, %s %p size %d\n", opcode, "write to", far, size));

    /**** MISC ****/
    if ((opcode & 0xffffffe0) == 0xd50b7e20)
    {
        /* Cache flushes on PiStorm-mapped region, ignore */
        handled = 1;
    }
    /**** Floating point stores ****/
    /* STUR */
    if ((opcode & 0xbfe00c00) == 0xbc000000)
    {
        if (size == 8)
        {
            value = get_fpn_as_double(opcode & 31);
        }
        else
        {
            value = get_fpn_as_single(opcode & 31);
        }
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((int16_t)(opcode >> 5)) >> 7;

        handled = SYSWriteValToAddr(value, 0, size, ptr);
    }
    /* STS/STD post- and pre-index*/
    else if ((opcode & 0xbfe00400) == 0xbc000400)
    {
        int pre_index = opcode & 0x800;
        if (size == 8)
        {
            value = get_fpn_as_double(opcode & 31);
        }
        else
        {
            value = get_fpn_as_single(opcode & 31);
        }
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;

        // Pre-index?
        if (pre_index) ptr += offset;
        
        handled = SYSWriteValToAddr(value, 0, size, ptr);

        if (!pre_index) ptr += offset;

        SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
    }
    /* STS/STD unsigned offset */
    else if ((opcode & 0xbfc00000) == 0xbd000000)
    {
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        if (size == 8)
        {
            value = get_fpn_as_double(opcode & 31);
        }
        else
        {
            value = get_fpn_as_single(opcode & 31);
        }

        ptr += ((opcode >> 10) & 0xfff) * size;

        handled = SYSWriteValToAddr(value, 0, size, ptr);
    }
    /* STS/STD reg */
    else if ((opcode & 0xbfe04c00) == 0xbc204800)
    {
        int option = (opcode >> 13) & 7;
        int s = (opcode & 0x1000);

        if (size == 8)
        {
            value = get_fpn_as_double(opcode & 31);
        }
        else
        {
            value = get_fpn_as_single(opcode & 31);
        }

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        uint64_t rm = SYSGetValueFromReg((opcode >> 16) & 31, ctx);

        switch (option)
        {
            case 0b010: // UXTW
                rm &= 0xffffffffULL;
                break;
            case 0b110: // SXTW
                if (rm & 0x80000000)
                    rm |= 0xffffffff00000000ULL;
                else
                    rm &= 0x00000000ffffffffULL;
                break;
        }

        if (s)
        {
            if (size == 4)
                rm <<= 2;
            else
                rm <<= 3;
        }

        handled = SYSWriteValToAddr(value, 0, size, ptr + rm);
    }
    /**** Integer stores ****/
    /* STUR */
    else if ((opcode & 0x3fe00c00) == 0x38000000)
    {   
        if ((opcode & 31) == 31)
            value = 0;
        else
            value = SYSGetValueFromReg(opcode & 31, ctx);

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((int16_t)(opcode >> 5)) >> 7;

        handled = SYSWriteValToAddr(value, 0, size, ptr);
    }
    /* STR immediate post-/pre-index */
    else if ((opcode & 0x3fe00400) == 0x38000400)
    {
        int pre_index = opcode & 0x800;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;

        // Pre-index?
        if (pre_index) 
        {
            ptr += offset;
        }

        if ((opcode & 31) == 31)
            value = 0;
        else
            value = SYSGetValueFromReg(opcode & 31, ctx);
        
        handled = SYSWriteValToAddr(value, 0, size, ptr);
        
        if (!pre_index) 
        {
            ptr += offset;
        }

        SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
    }
    /* STR unsigned offset */
    else if ((opcode & 0x3fc00000) == 0x39000000)
    {
        if ((opcode & 31) == 31)
            value = 0;
        else
            value = SYSGetValueFromReg(opcode & 31, ctx);

        uint16_t offset = ((opcode >> 10) & 0xfff) * size;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        handled = SYSWriteValToAddr(value, 0, size, ptr + offset);
    }
    /* STR register */
    else if ((opcode & 0x3fe00c00) == 0x38200800)
    {
        if ((opcode & 31) == 31)
            value = 0;
        else
            value = SYSGetValueFromReg(opcode & 31, ctx);

        int option = (opcode >> 13) & 7;
        int s = (opcode & 0x1000);

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        uint64_t rm = SYSGetValueFromReg((opcode >> 16) & 31, ctx);

        switch (option)
        {
            case 0b010: // UXTW
                rm &= 0xffffffffULL;
                break;
            case 0b110: // SXTW
                if (rm & 0x80000000)
                    rm |= 0xffffffff00000000ULL;
                else
                    rm &= 0x00000000ffffffffULL;
                break;
        }

        if (s)
        {
            switch(size)
            {
                case 2:
                    rm <<= 1;
                    break;
                case 4:
                    rm <<= 2;
                    break;
                case 8:
                    rm <<= 3;
                    break;
            }
        }

        handled = SYSWriteValToAddr(value, 0, size, ptr + rm);
    }
    /* ST(L)XR register - no exclusive in this case!!! But m68k bus does not support it anyway */
    else if ((opcode & 0x3fe07c00) == 0x08007c00)
    {
        if ((opcode & 31) == 31)
            value = 0;
        else
            value = SYSGetValueFromReg(opcode & 31, ctx);

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        // Mark the store as successful
        SYSPutValueToReg(0, (opcode >> 16) & 31, ctx);

        handled = SYSWriteValToAddr(value, 0, size, ptr);
    }
    /* STP */
    else if ((opcode & 0x7fc00000) == 0x29000000)
    {
        if (opcode & 0x80000000)
            size = 8;
        else
            size = 4;

        int16_t offset = size * (((int16_t)(opcode >> 6)) >> 9);
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        
        ptr += offset;

        if (size == 4)
        {
            if ((opcode & 31) == 31)
                value = 0;
            else
                value = SYSGetValueFromReg(opcode & 31, ctx);

            value <<= 32;

            if (((opcode >> 10) & 31) != 31)
                value |= (SYSGetValueFromReg((opcode >> 10) & 31, ctx)) & 0xFFFFFFFFULL;

            handled = SYSWriteValToAddr(value, 0, 8, ptr);
        }
        else
        {
            uint128_t v;
            
            if ((opcode & 31) == 31)
                v.hi = 0;
            else
                v.hi = SYSGetValueFromReg(opcode & 31, ctx);

            if (((opcode >> 10) & 31) == 31)
                v.lo = 0;
            else
                v.lo = SYSGetValueFromReg((opcode >> 10) & 31, ctx);

            handled = SYSWriteValToAddr(v.hi, v.lo, 16, ptr);
        }
    }
    /* STP post index */
    else if ((opcode & 0x7fc00000) == 0x28800000)
    {
        int16_t offset = ((int16_t)(opcode >> 6)) >> 9;
        if (opcode & 0x80000000)
            size = 8;
        else
            size = 4;

        offset *= size;
    
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        if (size == 4)
        {
            if ((opcode & 31) == 31)
                value = 0;
            else
                value = SYSGetValueFromReg(opcode & 31, ctx);

            value <<= 32;

            if (((opcode >> 10) & 31) != 31)
                value |= (SYSGetValueFromReg((opcode >> 10) & 31, ctx)) & 0xFFFFFFFFULL;

            handled = SYSWriteValToAddr(value, 0, 8, ptr);
        }
        else
        {
            uint128_t v;
            
            if ((opcode & 31) == 31)
                v.hi = 0;
            else
                v.hi = SYSGetValueFromReg(opcode & 31, ctx);

            if (((opcode >> 10) & 31) == 31)
                v.lo = 0;
            else
                v.lo = SYSGetValueFromReg((opcode >> 10) & 31, ctx);

            handled = SYSWriteValToAddr(v.hi, v.lo, 16, ptr);
        }
        SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
    }
    /* STP pre index */
    else if ((opcode & 0x7fc00000) == 0x29800000)
    {
        int16_t offset = ((int16_t)(opcode >> 6)) >> 9;
        if (opcode & 0x80000000)
            size = 8;
        else
            size = 4;

        offset *= size;
        
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += offset;
        
        if (size == 4)
        {
            if ((opcode & 31) == 31)
                value = 0;
            else
                value = SYSGetValueFromReg(opcode & 31, ctx);

            value <<= 32;

            if (((opcode >> 10) & 31) != 31)
                value |= (SYSGetValueFromReg((opcode >> 10) & 31, ctx)) & 0xFFFFFFFFULL;

            handled = SYSWriteValToAddr(value, 0, 8, ptr);
        }
        else
        {
            uint128_t v;
            
            if ((opcode & 31) == 31)
                v.hi = 0;
            else
                v.hi = SYSGetValueFromReg(opcode & 31, ctx);

            if (((opcode >> 10) & 31) == 31)
                v.lo = 0;
            else
                v.lo = SYSGetValueFromReg((opcode >> 10) & 31, ctx);

            handled = SYSWriteValToAddr(v.hi, v.lo, 16, ptr);
        }
        SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
    }

    if (!handled)
    {    
        kprintf("[JIT:SYS] Unhandled page fault: opcode %08x, write to %p\n", opcode, far);
    }

    elr += 4;
    __asm__ volatile("msr ELR_EL1, %0"::"r"(elr));

    return handled;
}


int SYSPageFaultReadHandler(uint32_t vector, uint64_t *ctx, uint64_t elr, uint64_t spsr, uint64_t esr, uint64_t far)
{
    int handled = 0;
    int size = 0;
    uint64_t value = 0;
    uint32_t opcode = LE32(*(uint32_t *)elr);
    (void)vector;
    (void)spsr;

    if (esr & (1 << 24))
    {
        kprintf("PageFault with valid instruction syndrome: %08x\n", esr);
    }

    size = getOPsize(opcode);

    D(kprintf("[JIT:SYS] Fage fault: opcode %08x, %s %p size %d\n", opcode, "read from", far, size));

    /**** Floating point loads ****/
    /* FLDS */
    if ((opcode & 0xfee00c00) == 0xbc400000)
    {
        handled = SYSReadValFromAddr(&value, NULL, 4, far);
        if (handled)
        {
            set_fpn_as_single(opcode & 31, value);
        }
    }
    /* FLDS unsigned offset */
    else if ((opcode & 0xff400000) == 0xbd400000)
    {
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((opcode >> 10) & 0xfff) * 4;

        handled = SYSReadValFromAddr(&value, NULL, 4, ptr);
        if (handled)
        {
            set_fpn_as_single(opcode & 31, value);
        }
    }
    /* FLDS pre-index */
    else if ((opcode & 0xfee00c00) == 0xbc400400)
    {
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        ptr += offset;

        handled = SYSReadValFromAddr(&value, NULL, 4, ptr);
        if (handled)
        {
            set_fpn_as_single(opcode & 31, value);
            SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
        }
    }
    /* FLDS post-index */
    else if ((opcode & 0xfee00c00) == 0xbc400c00)
    {
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        handled = SYSReadValFromAddr(&value, NULL, 4, ptr);
        if (handled)
        {
            set_fpn_as_single(opcode & 31, value);
            SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
        }
    }
    /* FLDD */
    else if ((opcode & 0xfee00c00) == 0xfc400000)
    {
        handled = SYSReadValFromAddr(&value, NULL, 8, far);
        if (handled)
        {
            set_fpn_as_double(opcode & 31, value);
        }
    }
    /* FLDD unsigned offset */
    else if ((opcode & 0xff400000) == 0xfd400000)
    {
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((opcode >> 10) & 0xfff) * 8;

        handled = SYSReadValFromAddr(&value, NULL, 8, ptr);
        if (handled)
        {
            set_fpn_as_double(opcode & 31, value);
        }
    }
    /* FLDD reg offset */
    else if ((opcode & 0xfee00c00) == 0xfc600800)
    {
        handled = SYSReadValFromAddr(&value, NULL, 8, far);
        if (handled)
        {
            set_fpn_as_double(opcode & 31, value);
        }
    }
    /* FLDD pre-index */
    else if ((opcode & 0xfee00c00) == 0xfc400400)
    {
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        ptr += offset;

        handled = SYSReadValFromAddr(&value, NULL, 8, ptr);
        if (handled)
        {
            set_fpn_as_double(opcode & 31, value);
            SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
        }
    }
    /* FLDD post-index */
    else if ((opcode & 0xfee00c00) == 0xbc400c00)
    {
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        handled = SYSReadValFromAddr(&value, NULL, 8, ptr);
        if (handled)
        {
            set_fpn_as_double(opcode & 31, value);
            SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
        }
    }
    /**** Integer loads ****/
    /* LDP */
    else if ((opcode & 0x7fc00000) == 0x29400000)
    {
        if (opcode & 0x80000000)
            size = 8;
        else
            size = 4;

        int16_t offset = size * (((int16_t)(opcode >> 6)) >> 9);
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        
        ptr += offset;

        if (size == 4)
        {
            uint64_t tmp;
            handled = SYSReadValFromAddr(&tmp, NULL, 8, ptr);
            if (handled) {
                SYSPutValueToReg(tmp >> 32, opcode & 31, ctx);
                SYSPutValueToReg(tmp & 0xffffffffULL, (opcode >> 10) & 31, ctx);
            }
        }
        else
        {
            uint64_t tmp1, tmp2;
            handled = SYSReadValFromAddr(&tmp1, &tmp2, 16, ptr);

            if (handled) {
                SYSPutValueToReg(tmp1, opcode & 31, ctx);
                SYSPutValueToReg(tmp2, (opcode >> 10) & 31, ctx);
            }
        }
    }
    /* LDP post- and pre-index */
    else if ((opcode & 0x7ec00000) == 0x28c00000)
    {
        int16_t offset = ((int16_t)(opcode >> 6)) >> 9;
        if (opcode & 0x80000000)
            size = 8;
        else
            size = 4;

        offset *= size;

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        if (opcode & 0x01000000) {
            far = ptr + offset; 
        }
        else
        {
            far = ptr;
        }

        if (size == 4)
        {
            uint64_t tmp;
            handled = SYSReadValFromAddr(&tmp, NULL, 8, far);
            if (handled) {
                SYSPutValueToReg(tmp >> 32, opcode & 31, ctx);
                SYSPutValueToReg(tmp & 0xffffffffULL, (opcode >> 10) & 31, ctx);
            }
        }
        else
        {
            uint64_t tmp1, tmp2;
            handled = SYSReadValFromAddr(&tmp1, &tmp2, 16, far);

            if (handled) {
                SYSPutValueToReg(tmp1, opcode & 31, ctx);
                SYSPutValueToReg(tmp2, (opcode >> 10) & 31, ctx);
            }
        }
            
        SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
    }
    /* LDPSW */
    if ((opcode & 0xffc00000) == 0x69400000)
    {
        size = 4;
        
        int16_t offset = size * (((int16_t)(opcode >> 6)) >> 9);
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        
        ptr += offset;

        uint64_t tmp;
        handled = SYSReadValFromAddr(&tmp, NULL, 2 * size, ptr);

        if (handled) {
            uint64_t tmp1, tmp2;

            tmp1 = tmp >> 32;
            if (tmp1 & 0x80000000)
                tmp1 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp1, opcode & 31, ctx);

            tmp2 = tmp & 0xffffffffULL;
            if (tmp2 & 0x80000000)
                tmp2 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp2, (opcode >> 10) & 31, ctx);
        }
    }
    /* LDPSW post index */
    else if ((opcode & 0xffc00000) == 0x68c00000)
    {
        int16_t offset = ((int16_t)(opcode >> 6)) >> 9;
        size = 4;
        offset *= size;
        
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        uint64_t tmp;
        handled = SYSReadValFromAddr(&tmp, NULL, 2 * size, ptr);

        if (handled) {
            uint64_t tmp1, tmp2;

            tmp1 = tmp >> 32;
            if (tmp1 & 0x80000000)
                tmp1 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp1, opcode & 31, ctx);

            tmp2 = tmp & 0xffffffffULL;
            if (tmp2 & 0x80000000)
                tmp2 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp2, (opcode >> 10) & 31, ctx);
        }

        SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
    }
    /* LDPSW pre index */
    else if ((opcode & 0xffc00000) == 0x69c00000)
    {
        int16_t offset = ((int16_t)(opcode >> 6)) >> 9;
        size = 4;
        offset *= size;

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += offset;

        uint64_t tmp;
        handled = SYSReadValFromAddr(&tmp, NULL, 2 * size, ptr);

        if (handled) {
            uint64_t tmp1, tmp2;

            tmp1 = tmp >> 32;
            if (tmp1 & 0x80000000)
                tmp1 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp1, opcode & 31, ctx);

            tmp2 = tmp & 0xffffffffULL;
            if (tmp2 & 0x80000000)
                tmp2 |= 0xffffffff00000000ULL;

            SYSPutValueToReg(tmp2, (opcode >> 10) & 31, ctx);
        }

        SYSPutValueToReg(ptr, (opcode >> 5) & 31, ctx);
    }
    /* LDR (literal) */
    else if ((opcode & 0xbf000000) == 0x18000000 || (opcode & 0xff000000) == 0x98000000)
    {
        uint64_t tmp;
        int sext = 0;
        if (opcode & 0x40000000)
            size = 8;
        else
            size = 4;
        if (opcode & 0x80000000)
            sext = 1;
        
        handled = SYSReadValFromAddr(&tmp, NULL, size, far);
        if (handled & sext) {
            if (tmp & 0x80000000)
                tmp |= 0xffffffff00000000ULL;
            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDR register */
    else if ((opcode & 0x3fe00c00) == 0x38600800)
    {
        int option = (opcode >> 13) & 7;
        int s = (opcode & 0x1000);

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        uint64_t rm = SYSGetValueFromReg((opcode >> 16) & 31, ctx);

        switch (option)
        {
            case 0b010: // UXTW
                rm &= 0xffffffffULL;
                break;
            case 0b110: // SXTW
                if (rm & 0x80000000)
                    rm |= 0xffffffff00000000ULL;
                else
                    rm &= 0x00000000ffffffffULL;
                break;
        }

        if (s)
        {
            switch(size)
            {
                case 2:
                    rm <<= 1;
                    break;
                case 4:
                    rm <<= 2;
                    break;
                case 8:
                    rm <<= 3;
                    break;
            }
        }

        ptr += rm;
        
        uint64_t tmp = 0;

        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDXR register - no exclusive in this case!!! But m68k bus does not support it anyway */
    else if ((opcode & 0x3ffffc00) == 0x085f7c00)
    {
        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, far);
        if (handled) {
            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDR immediate */
    else if ((opcode & 0x3fc00000) == 0x39400000)
    {
        uint64_t offset = (opcode >> 10) & 0xfff;
        offset *= size;

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += offset;

        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDUR(B/W) */
    else if ((opcode & 0x3fe00c00) == 0x38400000)
    {
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((int16_t)(opcode >> 5)) >> 7;

        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDR immediate, post- and pre-index */
    else if ((opcode & 0x3fe00400) == 0x38400400)
    {
        int64_t offset = ((int16_t)(opcode >> 5)) >> 7;
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        if (opcode & (1 << 11 ))
        {
            far = ptr + offset;
        }
        else
        {
            far = ptr;
        }
        
        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, far);
        
        if (handled) {
            SYSPutValueToReg(tmp, opcode & 31, ctx);
            SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
        }
    }
    /* LDRSW/LDRSB/LDRSH register */
    else if ((opcode & 0x3fa00c00) == 0x38a00800)
    {
        int sext64 = 1;
        if (opcode & (1 << 22))
            sext64 = 0;
        
        int option = (opcode >> 13) & 7;
        int s = (opcode & 0x1000);

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        uint64_t rm = SYSGetValueFromReg((opcode >> 16) & 31, ctx);

        switch (option)
        {
            case 0b010: // UXTW
                rm &= 0xffffffffULL;
                break;
            case 0b110: // SXTW
                if (rm & 0x80000000)
                    rm |= 0xffffffff00000000ULL;
                else
                    rm &= 0x00000000ffffffffULL;
                break;
        }

        if (s)
        {
            switch(size)
            {
                case 2:
                    rm <<= 1;
                    break;
                case 4:
                    rm <<= 2;
                    break;
                case 8:
                    rm <<= 3;
                    break;
            }
        }

        ptr += rm;

        uint64_t tmp = 0;

        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            int sext = 0;
            switch (size)
            {
                case 1:
                    sext = tmp & 0x80;
                    if (sext) tmp |= 0xffffff00;
                    break;
                case 2:
                    sext = tmp & 0x8000;
                    if (sext) tmp |= 0xffff0000;
                    break;
                case 4:
                    sext = tmp & 0x80000000;
                    break;
            }

            if (sext && sext64) {
                tmp |= 0xffffffff00000000ULL;
            }

            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDRSW/LDRSB/LDRSH immediate */
    else if ((opcode & 0x3f800000) == 0x39800000)
    {
        int sext64 = 1;
        if (opcode & (1 << 22))
            sext64 = 0;
        
        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((opcode >> 10) & 0xfff) * size;

        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            int sext = 0;
            switch (size)
            {
                case 1:
                    sext = tmp & 0x80;
                    if (sext) tmp |= 0xffffff00;
                    break;
                case 2:
                    sext = tmp & 0x8000;
                    if (sext) tmp |= 0xffff0000;
                    break;
                case 4:
                    sext = tmp & 0x80000000;
                    break;
            }

            if (sext && sext64) {
                tmp |= 0xffffffff00000000ULL;
            }

            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDURSW/LDURSB/LDURSH immediate */
    else if ((opcode & 0x3fa00c00) == 0x38800000)
    {
        int sext64 = 1;
        if (opcode & (1 << 22))
            sext64 = 0;

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);
        ptr += ((int16_t)(opcode >> 5)) >> 7;

        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, ptr);
        if (handled) {
            int sext = 0;
            switch (size)
            {
                case 1:
                    sext = tmp & 0x80;
                    if (sext) tmp |= 0xffffff00;
                    break;
                case 2:
                    sext = tmp & 0x8000;
                    if (sext) tmp |= 0xffff0000;
                    break;
                case 4:
                    sext = tmp & 0x80000000;
                    break;
            }

            if (sext && sext64) {
                tmp |= 0xffffffff00000000ULL;
            }

            SYSPutValueToReg(tmp, opcode & 31, ctx);
        }
    }
    /* LDRSW/LDRSB/LDRSH post- and pre-index */
    else if ((opcode & 0x3fa00400) == 0x38800400)
    {
        int16_t offset = ((int16_t)(opcode >> 5)) >> 7;
        int sext64 = 1;
        if (opcode & (1 << 22))
            sext64 = 0;

        uint64_t ptr = SYSGetValueFromReg((opcode >> 5) & 31, ctx);

        if (opcode & 0x800)
        {
            far = ptr + offset;
        }
        else far = ptr;
        
        uint64_t tmp = 0;
        handled = SYSReadValFromAddr(&tmp, NULL, size, far);
        if (handled) {
            int sext = 0;
            switch (size)
            {
                case 1:
                    sext = tmp & 0x80;
                    if (sext) tmp |= 0xffffff00;
                    break;
                case 2:
                    sext = tmp & 0x8000;
                    if (sext) tmp |= 0xffff0000;
                    break;
                case 4:
                    sext = tmp & 0x80000000;
                    break;
            }

            if (sext && sext64) {
                tmp |= 0xffffffff00000000ULL;
            }

            SYSPutValueToReg(tmp, opcode & 31, ctx);
            SYSPutValueToReg(ptr + offset, (opcode >> 5) & 31, ctx);
        }
    }

    if (!handled)
    {    
        kprintf("[JIT:SYS] Unhandled page fault: opcode %08x, read from %p\n", opcode, far);
    }

    elr += 4;
    __asm__ volatile("msr ELR_EL1, %0"::"r"(elr));

    return handled;
}

#undef D
#define D(x)  x 

#include "disasm.h"

void SYSHandler(uint32_t vector, uint64_t *ctx)
{
    int handled = 0;
    uint64_t elr, spsr, esr, far, cpu_id;
    __asm__ volatile("mrs %0, ELR_EL1; mrs %1, SPSR_EL1":"=r"(elr),"=r"(spsr));
    __asm__ volatile("mrs %0, ESR_EL1":"=r"(esr));
    __asm__ volatile("mrs %0, FAR_EL1":"=r"(far));
    __asm__ volatile("mrs %0, MPIDR_EL1":"=r"(cpu_id));
   
    cpu_id &= 3;

    if ((vector & 0x1ff) == 0x00 && (esr & 0xf8000000) == 0x90000000)
    {
        int writeFault = (esr & (1 << 6)) != 0;

#if defined(AGA_PISTORM) && (defined(AGA_DIAG) || defined(AGA_PROBES))
        /* diag and test builds only - two clock reads per abort aren't free:
           what the chip RAM redirect costs core 0. Every 68k chip RAM access
           is one of these aborts while the sandbox is on; the chipset core
           publishes the per-frame total (abort_us_frame). */
        extern volatile uint64_t aga_abort_ticks, aga_abort_count;
        if (aga_sandbox_active && (far & 0xffffffff) < 0x200000) {
            uint64_t t0; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t0));
            handled = writeFault ? SYSPageFaultWriteHandler(vector, ctx, elr, spsr, esr, far) : SYSPageFaultReadHandler(vector, ctx, elr, spsr, esr, far);
            uint64_t t1; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t1));
            aga_abort_ticks += t1 - t0; aga_abort_count++;
        } else
#endif
        handled = writeFault ? SYSPageFaultWriteHandler(vector, ctx, elr, spsr, esr, far) : SYSPageFaultReadHandler(vector, ctx, elr, spsr, esr, far);
    }
    else if ((vector & 0x1ff) == 0x00 && (esr & 0xf8000000) == 0x80000000)
    {
        if ((far >> 56) == 0xaa)
        {
            handled = SYSValidateUnit(vector, ctx, elr, spsr, esr, far);
        }
    }
    else if ((esr & ~0xffff) == 0x56000000)
    {
        handled = 1;

        if ((esr & 0xffff) == 0x100)
        {
            kprintf("[JIT:SYS] AArch64 RegDump:\n");
            for (int i=0; i < 8; i++)
            {
                kprintf("[JIT:SYS]   X%02d=%p   X%02d=%p   X%02d=%p   X%02d=%p\n", 
                    4*i, SYSGetValueFromReg(4*i, ctx),
                    4*i+1, SYSGetValueFromReg(4*i+1, ctx),
                    4*i+2, SYSGetValueFromReg(4*i+2, ctx),
                    4*i+3, SYSGetValueFromReg(4*i+3, ctx));
            }
            uint64_t spsr;

            __asm__ volatile("mrs %0, SPSR_EL1":"=r"(spsr));
            kprintf("[JIT:SYS]   SPSR=%08x\n", spsr);
        }

        if ((esr & 0xffff) == 0x101)
        {
            uint64_t sr;

            __asm__ volatile("umov %w0, "REG_SR_ASM:"=r"(sr));

            kprintf("[JIT:SYS] M68k RegDump:\n[JIT] ");
            int reg_dn[] = {REG_D0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7};
            int reg_an[] = {REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7};

            for (int i=0; i < 8; i++) {
                if (i==4)
                    kprintf("\n[JIT] ");
                kprintf("    D%d = 0x%08x", i, BE32(SYSGetValueFromReg(reg_dn[i], ctx)));
            }
            kprintf("\n[JIT] ");

            for (int i=0; i < 8; i++) {
                if (i==4)
                    kprintf("\n[JIT] ");
                kprintf("    A%d = 0x%08x", i, BE32(SYSGetValueFromReg(reg_an[i], ctx)));
            }
            kprintf("\n[JIT] ");

            kprintf("    PC = 0x%08x    SR = ", BE32(SYSGetValueFromReg(REG_PC, ctx)));

            kprintf("T%d|", sr >> 14);
    
            if (sr & SR_S)
                kprintf("S");
            else
                kprintf(".");
    
            if (sr & SR_M)
                kprintf("M|");
            else
                kprintf(".|");

            kprintf("IPM%d|", (sr >> 8) & 7);

            if (sr & SR_X)
                kprintf("X");
            else
                kprintf(".");

            if (sr & SR_N)
                kprintf("N");
            else
                kprintf(".");

            if (sr & SR_Z)
                kprintf("Z");
            else
                kprintf(".");

            if (sr & SR_V)
                kprintf("V");
            else
                kprintf(".");

            if (sr & SR_C)
                kprintf("C");
            else
                kprintf(".");
            
            kprintf("\n");
        }

        if ((esr & 0xffff) == 0x102)
        {
            uint8_t *from = (uint8_t*)(intptr_t)(*(uint32_t *)elr);
            uint32_t len = *(uint32_t *)(elr + 4);

            kprintf("[SYS:JIT] RAM dump from 0x%08x, len 0x%x\n[SYS:JIT]   ", from, len);

            while (len) {
                if ((len & 15) == 0)
                    kprintf("\n[SYS:JIT]   ");
                kprintf("%02x ", *from++);
                len--;
            }

            kprintf("\n");

            elr += 8;
            __asm__ volatile("msr ELR_EL1, %0"::"r"(elr));
        }

        if ((esr & 0xffff) == 0x103)
        {
            uint16_t *from = (uint16_t*)(intptr_t)(*(uint32_t *)elr);
            uint32_t len = (*(uint32_t *)(elr + 4)) / 2;

            kprintf("[SYS:JIT] RAM dump from 0x%08x, len 0x%x\n[SYS:JIT]   ", from, len);

            while (len) {
                if ((len & 7) == 0)
                    kprintf("\n[SYS:JIT]   ");
                kprintf("%04x ", *from++);
                len--;
            }

            kprintf("\n");

            elr += 8;
            __asm__ volatile("msr ELR_EL1, %0"::"r"(elr));
        }

        if ((esr & 0xff00) == 0x200) {
            uint8_t regnum = esr & 0xff;
            if (regnum < 32) {
                kprintf("[JIT:SYS] X%02d=%p\n", regnum, SYSGetValueFromReg(regnum, ctx));
            }
        }
    }

    if (!handled)
    {
        kprintf("[JIT:SYS] Exception with vector %04x on CPU%d. ELR=%p, SPSR=%08x, ESR=%p, FAR=%p\n", vector, cpu_id, elr, spsr, esr, far);
        kprintf("[JIT:SYS] Failed instruction: %08x\n", LE32(*(uint32_t*)elr));
        uint32_t *ptr = (uint32_t *)elr;
        
        disasm_open();

        for (int i=-4; i < 5; i++) {
            kprintf("[JIT:SYS] %08x\n", LE32(ptr[i]));
        }

        disasm_print_arm_only(ptr);

        for (int i=0; i < 16; i++)
        {
            kprintf("[JIT:SYS]  X%02d=%p   X%02d=%p\n", 2*i, SYSGetValueFromReg(2*i, ctx), 
                                                        2*i+1, SYSGetValueFromReg(2*i+1, ctx));
        }
        
        while(1) { __asm__ volatile("wfe"); };
    }
}

void IRQHandler(uint32_t , uint64_t *)
{
    uint64_t elr, spsr, esr, far, cpu_id;
    __asm__ volatile("mrs %0, ELR_EL1; mrs %1, SPSR_EL1":"=r"(elr),"=r"(spsr));
    __asm__ volatile("mrs %0, ESR_EL1":"=r"(esr));
    __asm__ volatile("mrs %0, FAR_EL1":"=r"(far));
    __asm__ volatile("mrs %0, MPIDR_EL1":"=r"(cpu_id));
   
    cpu_id &= 3;

    /* Core 3 received IRQ */
    if (cpu_id == 3) {
        uint64_t tmp;
        uint32_t id = 0;

        if (gic_available()) {
            id = gic_read_iar();
        }

        /* Check if the interrupt comes from the timer */
        __asm__ volatile("mrs %0, CNTP_CTL_EL0":"=r"(tmp));

        /* Bit 2 set == interrupt reported */
        if (tmp & 4)
        {
            /*
                Mask further interrupts but keep timer running, the timer will 
                be unmasked by next write to decrementer
            */
            __asm__ volatile("msr CNTP_CTL_EL0, %0"::"r"(3));

            void PPCReportInterrupt(int interrupt);

            PPCReportInterrupt(0x900);

            if (gic_available()) {
                gic_write_eoir(id);
            }

            return;
        }
    }
    else if (cpu_id == 0)
    {
        uint32_t id = 0;
        void M68kReportInterrupt(int);

        if (gic_available()) {
            id = gic_read_iar();
        }

        M68kReportInterrupt(1);

        if (gic_available()) {
            gic_write_eoir(id);
        }
        
        return;
    }

    kprintf("[JIT:SYS] IRQ/FIQ Exception on CPU%d %08lx\n", cpu_id);
}
