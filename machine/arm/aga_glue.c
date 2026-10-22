/* AGA-PISTORM - Emu68 glue: runs the AGA core on a spare ARM core and serves
 * the emulated 68k's chip RAM / custom register accesses.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This file is compiled inside the Emu68 tree (see docs/EMU68-PATCH.md). It
 * relies on the following Emu68 internals:
 *   - __m68k_state (struct M68KState *), INTF.AGA byte added to the INTF union
 *   - ps_read_16 / ps_write_16 (PiStorm bus)
 *   - mmu_map(), tlsf/allocators for the virtual chip RAM block
 *   - kprintf()
 *   - the trap path: SYSWriteValToAddr / SYSReadValFromAddr in vectors.c call
 *     aga_trap_write / aga_trap_read first when aga_enabled.
 *
 * Threading: core 0 (JIT) enters aga_trap_* from the data abort handler; core 3
 * runs aga_chipset_main(). Both take `aga_lock` around any core call.
 */
#include <stdint.h>
#include <string.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#include "mmu.h"
#include "A64.h"          /* MMU_* page attributes */
#include "cache.h"        /* Emu68's software I/D caches: flushed/invalidated around redirection */
#include "devicetree.h"  /* v0.8.5 HUD boot-config gate */

/* ---- Emu68 externs ------------------------------------------------------- */
struct M68KState;
extern struct M68KState *__m68k_state;
extern void kprintf(const char *fmt, ...);
/* The bus API is declared differently per board: plain functions for the
   classic CPLD PiStorm, function pointers for PiStorm16/32. Include the header
   rather than re-declaring it, so I cannot drift from whichever is built. */
#include "ps_protocol.h"
#ifdef FRF_VIRTUAL_AGA
#define ps_read_8(a)       ((unsigned)0xffu)
#define ps_read_16(a)      ((unsigned)0xffffu)
#define ps_read_32(a)      ((unsigned)0xffffffffu)
#define ps_write_8(a,v)    do { (void)(a); (void)(v); } while (0)
#define ps_write_16(a,v)   do { (void)(a); (void)(v); } while (0)
#define ps_write_32(a,v)   do { (void)(a); (void)(v); } while (0)
#endif
extern void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
extern void mmu_unmap(uintptr_t virt, uintptr_t length);
/* provided by the patched Emu68: set/get the AGA IPL byte in INTF */
extern void emu68_set_aga_ipl(int ipl);
extern uint32_t emu68_get_sr(void);
extern uint32_t emu68_get_intf(void);
extern uint32_t emu68_get_insn_count(void);
extern uint32_t emu68_get_loop_ticks(void);
extern uint32_t emu68_get_irq_check(void);
extern void emu68_raise_nmi(void);
extern int  emu68_get_real_ipl(void);
extern uint32_t emu68_get_pc(void);   /* 68k PC, for the trap ring */

/* Guest Exec memory probes were removed from the normal chipset
 * loop: they are not virtual chipset emulation. */

/* ---- state ---------------------------------------------------------------- */
/* volatile: core 0 writes these, core 3 polls them in a spin loop. Without it
   gcc -O3 may hoist the load out of `while (!aga_enabled) yield;` and core 3
   spins on a stale register forever - so the chipset loop never starts, frames
   stays 0, hook_ipl never fires, and since sandbox entry has just disabled the
   real chipset's interrupts, AmigaOS is left with no interrupt source at all
   and freezes. Every other cross-core flag here was already volatile; these
   two, which gate everything, were not. */
volatile int aga_enabled = 0;
int aga_enter_stop = 0;      /* aga.enter_stop=N on the cmdline */
int aga_loop_stop  = 0;      /* aga.loop_stop=N  on the cmdline */
int aga_leave_stop = 0;      /* aga.leave_stop=N on the cmdline */
int aga_use_mmu    = 0;      /* aga.mmu: the old MMU-remap path, kept for A/B */
int aga_sleep      = 0;      /* aga.sleep: timed waits sleep (aga_glue.h); off by default */
uint32_t aga_nap_min = 0xFFFFFFFFu;   /* ticks left below which a timed wait spins (aga_evstream_on) */
static volatile uint32_t last_chip_wr;   /* overlay: last chip RAM address the 68k wrote */
volatile int aga_chip_redirect = 0;   /* no-MMU: serve redirect_lo..redirect_hi from chip_va in the abort handler */
static uint32_t redirect_lo, redirect_hi;   /* the window; the sandbox uses all of chip RAM */
volatile int aga_sandbox_active = 0;
static volatile int core_idle = 1;    /* chipset loop parked (aga_enabled == 0) */
static aga_t *aga;
static uint8_t *chip_va;              /* ARM virtual address of the virtual chip RAM */
static uint32_t chip_size;   /* the virtual chip RAM: what the game gets, 2 MB */
/* The same memory for Emu68's software instruction cache (cache.c): set while
   the sandbox is up, NULL otherwise. Everything that reads 68k code below
   16 MB goes through that cache - the translator and CalcCRC32's check of a
   unit alike - and it only ever learns of a change from a bus write. In the
   sandbox chip RAM is written through the redirect or the MMU map, by my
   blitter and by disk DMA, never by a bus write, so a line kept there could
   hand out code the program had already fucking rewritten, NoCache or not.
   With this set, chip addresses are read where they live. */
uint8_t * volatile aga_code_chip;
volatile uint32_t aga_code_chip_size;
/* $5941 on (the default) / $5940 off, from the next sandbox entry: the A/B
   switch for the above (agaboot CODEDIRECT). */
static volatile int aga_code_direct = 1;
void aga_set_code_direct(int on) { aga_code_direct = on; }

/* Standalone machine OVL owns the meaning of low 68k addresses.  Keep the
   translator/cache direct-code backing in lockstep with that hardware state:
   ROM overlay ON -> no virtual Chip-RAM shortcut; overlay OFF -> read code
   straight from chip_va. */
void aga_machine_code_overlay(int rom_on)
{
#ifdef FRF_VIRTUAL_AGA
    if (rom_on || !aga_code_direct) {
        aga_code_chip = NULL;
        aga_code_chip_size = 0;
    } else {
        aga_code_chip_size = chip_size;
        __atomic_store_n(&aga_code_chip, chip_va, __ATOMIC_RELEASE);
    }
#else
    (void)rom_on;
#endif
}
static uint32_t chip_real;   /* the real chip RAM: what is copied in and out, probed at entry */
static volatile char aga_lock_v __attribute__((aligned(64)));
static volatile int frames_done;
static volatile int real_ipl_seen;

/* WFE spinlocks need a matching SEV. Without one, a core that has already
   executed WFE sleeps until some other event happens to arrive: releasing the
   lock with a plain store-release signals nothing, and __atomic_test_and_set
   completes its exclusive sequence before the WFE, so there is no outstanding
   exclusive monitor to wake it either.

   That hung the machine the moment core 0 and core 3 first contended - which
   is exactly when the chipset goes live, in sandbox and machine mode alike.
   The abort handler on core 0 runs with interrupts masked, so nothing arrived
   to wake it.

   ps_bus_lock in ps_classic_protocol.c spins on YIELD, which has no event
   dependency at all. This now pairs SEV with the WFE properly; SEV sets the
   event register on every core, so an unlock racing a WFE cannot be missed. */
/* How long core 3 waits for this lock, per frame and at worst. Core 0 holds
   it inside the abort handler, and some of what it does there is PiStorm
   bus traffic (INTREQR reads fold in the real chip, the mirrored input
   registers are real reads) - on a Pi 3 the bus stalled audio pushes for
   5 ms (2026-09-14), so the same stalls can arrive through the lock. The
   fast path is untouched: the clock is only read once the first try fails. */
static inline uint64_t cntvct(void);
static volatile uint64_t lock_acc3, lock_max3;
static inline void lock(void)
{
    if (!__atomic_test_and_set(&aga_lock_v, __ATOMIC_ACQUIRE)) return;
#ifdef AGA_PROBES
    uint64_t t0 = cntvct();
#endif
    __asm__ volatile("sevl");
    while (__atomic_test_and_set(&aga_lock_v, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
#ifdef AGA_PROBES
    uint64_t dt = cntvct() - t0, mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    if ((mpidr & 0xFF) == 3) { lock_acc3 += dt; if (dt > lock_max3) lock_max3 = dt; }
#endif
}
static inline void unlock(void)
{
    __atomic_clear(&aga_lock_v, __ATOMIC_RELEASE);
    __asm__ volatile("sev");        /* wake anyone parked in the WFE above */
}

/* ---- hooks ---------------------------------------------------------------- */
#ifdef FRF_VIRTUAL_AGA
static volatile uint16_t frf_joydat[2];
static volatile uint8_t frf_fire_bits;
static volatile uint8_t frf_mouse_buttons;
static volatile uint8_t frf_mouse_x, frf_mouse_y;
/* Host input -> emulated Amiga readback telemetry. Atomic because Pi3
   TinyUSB lives on CPU1 and 68k custom register emulation on CPU0. */
static volatile uint32_t frf_guest_joy1_reads, frf_guest_ciaa_reads;
static volatile uint32_t frf_host_joy1_writes;
void aga_virtual_joy_link_stats(uint32_t out[4])
{
    out[0] = __atomic_load_n(&frf_joydat[1],__ATOMIC_ACQUIRE) |
      ((__atomic_load_n(&frf_fire_bits,__ATOMIC_ACQUIRE)&2u)?0x10000u:0u);
    out[1] = __atomic_load_n(&frf_host_joy1_writes,__ATOMIC_ACQUIRE);
    out[2] = __atomic_load_n(&frf_guest_joy1_reads,__ATOMIC_ACQUIRE);
    out[3] = __atomic_load_n(&frf_guest_ciaa_reads,__ATOMIC_ACQUIRE);
}

/* Paula POTGO high-byte state. OUT/DAT pairs are:
   15/14 RY, 13/12 RX, 11/10 LY (port-1 right mouse), 9/8 LX. */
static volatile uint16_t frf_potgo;
static volatile uint16_t frf_pad_potgor[2];
static volatile uint32_t frf_pad_fullbuttons[2];

uint8_t aga_virtual_ciaa_buttons(void)
{
    __atomic_fetch_add(&frf_guest_ciaa_reads,1u,__ATOMIC_RELAXED);
    uint8_t v = 0xC0u;
    if (frf_fire_bits & 1u) v &= (uint8_t)~0x40u;
    if (frf_fire_bits & 2u) v &= (uint8_t)~0x80u;
    return v;
}
void aga_virtual_joy_set(int port, uint16_t joydat, int fire)
{
    if ((unsigned)port > 1u) return;
    __atomic_store_n(&frf_joydat[port], joydat, __ATOMIC_RELEASE);
    if(port==1)
        __atomic_fetch_add(&frf_host_joy1_writes,1u,__ATOMIC_RELAXED);
    uint8_t bit = (uint8_t)(1u << port);
    if (fire) __atomic_fetch_or(&frf_fire_bits, bit, __ATOMIC_ACQ_REL);
    else      __atomic_fetch_and(&frf_fire_bits, (uint8_t)~bit, __ATOMIC_ACQ_REL);
}
void aga_virtual_pad_buttons(int port,uint32_t buttons)
{
 if((unsigned)port>1u)return;
 __atomic_store_n(&frf_pad_fullbuttons[port],buttons,__ATOMIC_RELEASE);
 uint16_t mask=0;
 if(buttons&2u)mask|=port?0x4000u:0x0400u;
 if(buttons&4u)mask|=port?0x1000u:0x0100u;
 __atomic_store_n(&frf_pad_potgor[port],mask,__ATOMIC_RELEASE);
}
void aga_virtual_mouse_update(int dx, int dy, unsigned buttons)
{
    frf_mouse_x = (uint8_t)(frf_mouse_x + dx);
    frf_mouse_y = (uint8_t)(frf_mouse_y + dy);
    __atomic_store_n(&frf_joydat[0],
                     (uint16_t)(((uint16_t)frf_mouse_y << 8) | frf_mouse_x),
                     __ATOMIC_RELEASE);
    frf_mouse_buttons = (uint8_t)buttons;
    if (buttons & 1u) __atomic_fetch_or(&frf_fire_bits, 1u, __ATOMIC_ACQ_REL);
    else              __atomic_fetch_and(&frf_fire_bits, (uint8_t)~1u, __ATOMIC_ACQ_REL);
}
#endif

static uint16_t hook_ext_read(void *u, uint32_t reg)
{
    (void)u;
#ifdef FRF_VIRTUAL_AGA
    /* FRF v0.10.15 STANDALONE FIREWALL.
       ext_read is now a host-input/default provider only.  No register read
       may escape to ps_read_* or to a donor Amiga chipset. */
    if (reg == JOY0DAT) return __atomic_load_n(&frf_joydat[0], __ATOMIC_ACQUIRE);
    if (reg == JOY1DAT) {
        __atomic_fetch_add(&frf_guest_joy1_reads,1u,__ATOMIC_RELAXED);
        return __atomic_load_n(&frf_joydat[1], __ATOMIC_ACQUIRE);
    }
    if (reg == POTGOR) {
        uint16_t pg = __atomic_load_n(&frf_potgo, __ATOMIC_ACQUIRE);
        uint16_t v = (uint16_t)((pg & 0xAA00u) | 0x5500u);
        uint8_t mb = __atomic_load_n(&frf_mouse_buttons, __ATOMIC_ACQUIRE);
        if ((pg & 0x0800u) && !(pg & 0x0400u)) v &= (uint16_t)~0x0400u;
        if ((pg & 0x0200u) && !(pg & 0x0100u)) v &= (uint16_t)~0x0100u;
        extern int frf_usb_early_startup_active(void);
        if (frf_usb_early_startup_active()) mb |= 3u;
        if (mb & 2u) v &= (uint16_t)~0x0400u;
        if (mb & 4u) v &= (uint16_t)~0x0100u;
        uint16_t pad=__atomic_load_n(&frf_pad_potgor[0],__ATOMIC_ACQUIRE) | __atomic_load_n(&frf_pad_potgor[1],__ATOMIC_ACQUIRE);
        if(!(pg&0x8000u))v&=(uint16_t)~(pad&0x4000u);
        if(!(pg&0x2000u))v&=(uint16_t)~(pad&0x1000u);
        if(!(pg&0x0800u))v&=(uint16_t)~(pad&0x0400u);
        if(!(pg&0x0200u))v&=(uint16_t)~(pad&0x0100u);
        return v;
    }
    /* No receive-side serial device is attached. Real Paula reports the
       transmitter empty/shift-register empty bits; returning $FFFF here was
       a hybrid stub leak and is not a valid virtual-Paula state. */
    if (reg == SERDATR) return 0x3000u;
    /* POT counters and fallback disk data are quiescent unless the virtual
       device core itself supplied a value before ext_read was reached. */
    if (reg == POT0DAT || reg == POT1DAT || reg == DSKDATR || reg == DSKBYTR)
        return 0;
    return 0;
#else
    return (uint16_t)ps_read_16(0xDFF000 + reg);
#endif
}

/* Is a WHDLoad game running, rather than the Workbench?
 *
 * It decides one thing: what happens to the real Paula's EXTER (level 6).
 *
 * With the OS up, EXTER must stay enabled or the CIAs go silent - timer.device
 * stops, the network dies, tasks that wait are never woken. So sandbox entry
 * deliberately keeps it on and INTREQR reads fold the real bit back in, which
 * is what makes the desktop survive the sandbox at all.
 *
 * A WHDLoad game is the opposite case. WHDLoad takes the machine away from the
 * OS and installs the GAME's vector table, and a game only fills in the levels
 * it actually uses. ClockwiserAGA installs level 3 and nothing else - its
 * autovector at BaseMem $78 is zero. The real CIA-B keeps counting throughout
 * (its TOD alarm was pending in the crash dump), so the EXTER I had forced on
 * eventually fired, WHDLoad dispatched level 6 through that zero vector, and
 * the CPU jumped to address 0:
 *
 *   Exception "Line 1111 Emulator" ($2C) at $4 occurred.
 *   SR=%0010011000000000            <- interrupt mask 6
 *   $361E4D7C 2008 3613D108 0078    <- stacked frame, vector offset $078
 *
 * i.e. I was killing the game with an interrupt it never fucking asked for.
 * In game mode I leave EXTER exactly as WHDLoad set it. PORTS (level 2)
 * stays, since WHDLoad handles the keyboard itself and the quit key depends
 * on it.
 *
 * WHEN it engages matters as much as what it does. `agaboot SANDBOX AUTO` runs
 * from WHDLoad's startup script, so the sandbox opens while AmigaOS is still
 * up - and WHDLoad then loads the slave, the game and (for a KickEmu title) a
 * whole Kickstart image through the OS filesystem. Taking EXTER away at entry
 * takes the CIAs away from the OS during exactly that window: timer.device
 * stops and the load never completes. Banshee froze on startup that way, where
 * the far smaller ClockwiserAGA happened to get through.
 *
 * So game mode is only ARMED at entry. It engages at the one unambiguous sign
 * that WHDLoad has taken the machine away from the OS: an INTENA write that
 * CLEARS EXTER (bit 15 low with bit 13 set) - WHDLoad's "all interrupts off"
 * as it shuts the OS down. From then on EXTER is never put back. Before it,
 * everything behaves exactly as it did for the desktop. */
/* Which audio channels reach the mix. Set with $5D0N at $DFF1F2, so a channel
   can be isolated on a RUNNING game without a rebuild - the only way left to
   tell "the effects are not in the output" from "the effects are in the output
   and something is masking them", now that the counters show channel 2 makes as
   much sound as the music channels do. */
static volatile int aud_mask_want = 15;
void aga_audio_mask(unsigned m)
{
    /* Kept HERE and re-applied every frame, not written once into the core.
       aga_reset() puts the core's mask back to "all four", and the sandbox
       resets on leave - so a mask set from Workbench before launching a game
       would be gone by the time the game made a sound, which is precisely when
       it is needed. There is no shell during a WHDLoad game to set it again. */
    aud_mask_want = (int)(m & 15);
    aga_diag_log("[AGA] audio channel mask changed");
}

int aga_game_mode = 0;
volatile int aga_program_mode = 0;
/* FRF_RUN_COPPER_HANDOFF_V1_118
 * Preserve the proven RGBPORT/HAM6 engine unchanged. PROGRAM/RUN only needs
 * an ownership gate: remember the virtual OS Copper installed by agaboot,
 * then release RGB/HAM6 after the child owns a different useful Copper for
 * two completed frames. A double-buffered child may alternate COP1LC values. */
static volatile uint32_t aga_program_seed_cop1lc;
static volatile uint32_t aga_program_candidate_cop1lc;
static volatile unsigned aga_program_candidate_frames;
static volatile int aga_program_rgb_released;
/* FRF_RUN_RTG_ASL_BRIDGE_V0_9
 * PROGRAM/RUN can keep the virtual AGA machine alive while the existing P96
 * Workbench or an RTG ASL/custom screen owns HDMI.  This is presentation
 * ownership only: custom-register traps, redirected Chip RAM, chipset timing
 * and RGB-port HAM6 continue running. */
volatile int aga_program_rtg_yield = 0;
void aga_program_rtg_surface(int on)
{
    if (!aga_program_mode || !aga_sandbox_active) return;
    on = !!on;
    if (aga_program_rtg_yield == on) return;
    aga_program_rtg_yield = on;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    aga_diag_log(on ? "[AGA] PROGRAM RTG/ASL: P96 owns HDMI"
                    : "[AGA] PROGRAM RTG/ASL: native AGA owns HDMI");
}

/* FRF_RUN_COPPER_HANDOFF_V1_118
 * Called by agaboot with $5A12 after PROGRAM sandbox entry has installed
 * graphics.library's current OS Copper, but before RedPill is launched.
 * This is deliberately independent of RTG/P96 ownership. */
void aga_program_seed_display(void)
{
    uint32_t cop;

    if (!aga_program_mode || !aga_sandbox_active || !aga) return;

    lock();
    cop = ((((uint32_t)aga_peek_reg(aga, 0x080) & 0x01ffu) << 16) | ((uint32_t)aga_peek_reg(aga, 0x082) & 0xfffeu));
    unlock();

    aga_program_candidate_cop1lc = 0;
    aga_program_candidate_frames = 0;

    if (cop < 0x400u) {
        aga_program_seed_cop1lc = 0;
        aga_diag_log("[AGA] PROGRAM: OS Copper seed invalid; RGB/HAM6 remains deferred");
        kprintf("[AGA] PROGRAM: OS COP1LC seed invalid (%08x); RGB/HAM6 still deferred\n",
                (unsigned)cop);
        return;
    }

    aga_program_seed_cop1lc = cop;
    kprintf("[AGA] PROGRAM: seeded OS COP1LC=%08x; waiting for child Copper\n",
            (unsigned)cop);
    aga_diag_log("[AGA] PROGRAM: OS Copper seeded; RGB/HAM6 still deferred");
}
int aga_cia_mirror = 1;   /* keyboard and mouse only by default: see aga.cia= */
int aga_keep_ciab = 0;    /* see aga.keepciab */
/* The mouse quit: until this tick, the keyboard reads as F10 held. */
static volatile uint64_t aga_quit_until;
/* 1: blits report finished after the fixed 9 us of 1.101, 0: after their real duration */
volatile int aga_blit_fast = 0;
/* $580N bit 1: completions raised at once even with BLIT still pending */
void aga_blit_mode(int v)
{
    aga_blit_fast = v & 1;
    if (aga) aga_blit_set_nohold(aga, (v >> 1) & 1);
}
/* How far the beam a program reads may run ahead of the chipset core:
   0 one pacing step (default), 1 not at all (EXACT), 2 up to a frame (1.102) */
volatile int aga_beam_mode = 0;
#define BEAM_LEAD_CCK 8   /* = CHUNK_CCK, the chipset loop's pacing step */
/* the mouse quit's state, kept here so leaving the sandbox can reset it */
static uint32_t mq_held2, mq_held1, mq_stage;
static uint64_t mq_stage_at;
int aga_exter_locked = 0;
/* For the stall check in the chipset loop: every chip RAM access and every
   custom register access the 68k makes, counted in shipped builds too - the
   diag counters it used to read (CHIP_W, TRAPS_W) exist only in probe builds,
   so a shipped kernel saw chip RAM READS alone, and a game that spent ten
   seconds writing chip RAM or polling registers was taken for dead. Written
   on core 0 only, read on core 3; a stale read just delays the check. */
static volatile uint32_t cpu_activity;
/* WHDLoad has switched every interrupt off: the machine is the game's. Set
   whatever aga.keepciab says, unlike aga_exter_locked; cleared at entry. */
static volatile int game_taken;
/* core 0's time inside chip RAM data aborts, accumulated in vectors.c;
   published per frame below as abort_us_frame / aborts_frame */
volatile uint64_t aga_abort_ticks, aga_abort_count;
/* Set at the takeover, acted on by the chipset loop once the copper is off:
   WHDLoad's splash screen was drawn by graphics.library believing the
   machine is AGA (my ChipRevBits0 poke), and an AGA copper list writes the
   palette in two passes, BPLCON3 $0C40 then $0E40 - LOCT set for the low
   nibbles. The last write leaves LOCT on. An ECS game never touches
   BPLCON3, so every COLORxx it writes lands in the low nibbles: the
   "veiled and dark" ECS games of 2026-09-12, which the manual ECS toggle
   has been papering over. Clearing LOCT once, after the OS list has
   stopped and before the game's first palette, costs an AGA game nothing:
   it sets BPLCON3 itself before it writes a colour. */
static volatile int aga_loct_fix_pending;
/* Set at the takeover too, but whatever aga.keepciab says: the AGA signs an
   ECS Denise saw until then were the OS's (WHDLoad's splash), not the game's
   (see the chipset loop) */
static volatile int aga_ecs_forget_pending;

/* Which chipset the next game gets. Set at $DFF1F2 before the sandbox is
   entered: $5E00 AGA, $5E01 ECS Denise, $5E03 ECS the user forced ("always
   ECS"), from agaboot - the user's Chipset menu choice, or in AUTO its
   judgement of the game's WHDLoad slave (ws_Flags bit 5 WHDLF_ReqAGA, the
   name, the base memory), which can be wrong: two of the seven AGA titles
   I had leave ReqAGA clear (CastlevaniaAGA, GhostsNGoblinsAGADemo). So an
   ECS Denise that sees the game draw for AGA gives itself up (playfield.c
   ecs_says_aga), unless forced.

   Unlike the audio mask above this IS written into the core, because the core
   keeps it across aga_reset() for exactly this reason: entering the sandbox
   resets the chipset, and the mode has to be chosen before entering. Sandbox
   leave puts it back to AGA, so a game that says nothing gets what the desktop
   gets and the old behaviour is unchanged. */
void aga_chipset_ecs(int mode)
{
    int on = mode & 1, forced = (mode & 3) == 3;
    if (!aga) return;
    lock();
    if (forced) aga_set_ecs_forced(aga, 1);
    else        aga_set_ecs(aga, on);
    unlock();
    aga_diag_log(forced ? "[AGA] chipset: ECS Denise (always ECS)" : on ? "[AGA] chipset: ECS Denise" : "[AGA] chipset: AGA");
}

/* agaboot RTG Image, into the core the same way, and kept across the entry's
   reset the same way:
     $59Dn  the flicker fixer - interlaced frames woven (aga_set_weave)
     $59Fn  brightness 0-4 (for scanlines, which darken the picture); the RGB
            port takes it back out, it has no scanlines
     $591n  PAL (0) or NTSC (1) for the next game - agaboot sends it before
            every entry; sandbox leave puts back the machine's own */
void aga_flickerfix(int on)
{
    if (!aga) return;
    lock();
    aga_set_weave(aga, on);
    unlock();
}
/* Brightness ($59Fn) and Gamma ($512n): the core puts every shown colour
   through their one curve (aga_tone_build); the RGB port, which takes the
   core's frames to the Amiga's own RGB port and wants the real colours,
   gets the curve's inverse - nearest input for each output, the curve
   rising all the way. Up to 1.115 the brightness was a straight gain and the
   RGB port divided it back out. */
static int tone_pct = 100, tone_gamma_sel;
static void tone_apply(void)
{
    static uint8_t lut[256], inv[256];
    int on = aga_tone_build(lut, tone_pct, tone_gamma_sel);
    for (int o = 0, v = 0; o < 256; o++) {
        while (v < 255 && lut[v] < o) v++;
        inv[o] = (uint8_t)(v > 0 && o - lut[v - 1] < lut[v] - o ? v - 1 : v);
    }
#ifndef FRF_VIRTUAL_AGA
    /* Only physical RGBPORT needs inverse colour curves. FRF's standalone
       RGB output path is independent and keeps the tested 0.10.38 engine. */
    aga_rgbout_set_tone(inv, on, tone_gamma_sel << 12 | tone_pct);
#else
    (void)inv; (void)on;
#endif
    if (!aga) return;
    lock();
    aga_set_brightness(aga, tone_pct);
    aga_set_gamma(aga, tone_gamma_sel);
    unlock();
}
void aga_brightness(int level)
{
    static const int pct[5] = { 100, 110, 120, 135, 150 };
    tone_pct = pct[(level >= 0 && level <= 4) ? level : 0];
    tone_apply();
}
void aga_gamma(int gamma)
{
    tone_gamma_sel = (gamma >= 0 && gamma <= 2) ? gamma : 0;
    tone_apply();
}
void aga_video_standard(int ntsc)
{
    if (!aga) return;
    lock();
    aga_set_ntsc(aga, ntsc);
    unlock();
    aga_diag_log(ntsc ? "[AGA] next entry: NTSC" : "[AGA] next entry: PAL");
}

/* The 68k's trap path reaches the real bus from inside the chipset lock (a
   write to INTENA, INTREQ, ADKCON, the serial port...). While it is on the
   bus the chipset core waits for the lock, so its bus time is frame time:
   counted here, per frame and worst single write. On a PiStorm16 the bus
   goes through the FPGA protocol and its own mutex, which the housekeeper
   holds for its audio-ring writes - the suspect for that board's lock
   waits being four times the classic board's. */
static volatile uint64_t trapbus_ticks, trapbus_max;
static volatile uint64_t traphold_ticks, traphold_max;   /* the lock held by a register write */
static volatile uint32_t trapbus_n;
static void hook_ext_write_body(void *u, uint32_t reg, uint16_t v);

/* What the real chips hold, as far as my own forwarded writes tell me: a
   bit is known once a write has set or cleared it. Nothing else writes these
   registers while the sandbox runs (the 68k's writes all come through here),
   so a write that only repeats known state changes nothing on the chip.
   Forgotten at every chipset reset (sandbox entry and leave). For now only
   COUNTED - which writes, and how many would be skipped - before any is. */
static uint16_t fwd_intena_known, fwd_intena_val;   /* INTEN | PORTS | EXTER */
static uint16_t fwd_adkcon_known, fwd_adkcon_val;
static int      fwd_potgo_known;
static uint16_t fwd_potgo_val;
/* A real floppy transfer running for the sandbox (dskb_len_write, below the
   chip RAM copy helpers): the OS's buffer as DSKPT was last written, its
   size, the direction, and the last DSKLEN write - the second one in a row
   with DMAEN starts the transfer, as on Paula. */
volatile int aga_dskb_pending;
static uint32_t dskb_pt, dskb_bytes;
static uint16_t dskb_last_len;
static int dskb_write, dskb_said;
static void dskb_len_write(uint16_t v);

static void fwd_forget(void)
{
    fwd_intena_known = fwd_adkcon_known = 0; fwd_potgo_known = 0;
    aga_dskb_pending = 0; dskb_last_len = 0; dskb_said = 0;     /* no transfer outlives a reset */
}

/* a SET/CLR register write: does it only repeat what is known? then note it */
static int fwd_setclr_nop(uint16_t *known, uint16_t *val, uint16_t v, uint16_t mask)
{
    uint16_t bits = v & mask;
    int set = (v & 0x8000) != 0;
    int nop = ((bits & *known) == bits) && (set ? (*val & bits) == bits : (*val & bits) == 0);
    *known |= bits;
    if (set) *val |= bits; else *val &= (uint16_t)~bits;
    return nop;
}

static void __attribute__((unused)) fwd_count(uint32_t reg, uint16_t v)
{
    switch (reg) {
    case INTENA:
        aga_diag_inc(AGA_D_FWD_INTENA);
        if (fwd_setclr_nop(&fwd_intena_known, &fwd_intena_val, v, INTF_INTEN | INTF_PORTS | INTF_EXTER))
            aga_diag_inc(AGA_D_FWD_INTENA_NOP);
        break;
    case INTREQ:  aga_diag_inc(AGA_D_FWD_INTREQ); break;
    case ADKCON:
        aga_diag_inc(AGA_D_FWD_ADKCON);
        if (fwd_setclr_nop(&fwd_adkcon_known, &fwd_adkcon_val, v, 0x7FFF)) aga_diag_inc(AGA_D_FWD_ADKCON_NOP);
        break;
    case POTGO:
        aga_diag_inc(AGA_D_FWD_POTGO);
        /* bit 0 is START, a strobe: never a repeat */
        if (fwd_potgo_known && fwd_potgo_val == v && !(v & 1)) aga_diag_inc(AGA_D_FWD_POTGO_NOP);
        fwd_potgo_known = 1; fwd_potgo_val = v;
        break;
    case JOYTEST: aga_diag_inc(AGA_D_FWD_JOYTEST); break;
    case SERDAT: case SERPER: aga_diag_inc(AGA_D_FWD_SER); break;
    case DSKPTH: case DSKPTL: case DSKLEN: case DSKDAT: case DSKSYNC: aga_diag_inc(AGA_D_FWD_DSK); break;
    default: aga_diag_inc(AGA_D_FWD_OTHER); break;
    }
}

static void hook_ext_write(void *u, uint32_t reg, uint16_t v)
{
#ifdef AGA_PROBES    /* test kernels: what reaches the real bus, and how long it takes */
    if (!aga_audio_reg_write(reg, v)) fwd_count(reg, v);   /* audio stays virtual: not a bus write */
    uint64_t t0;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t0));
    hook_ext_write_body(u, reg, v);
    uint64_t t1;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t1));
    uint64_t dt = t1 - t0;
    trapbus_ticks += dt;
    trapbus_n++;
    if (dt > trapbus_max) trapbus_max = dt;
#else
    hook_ext_write_body(u, reg, v);
#endif
}

/* A level-6 handler that acknowledges EXTER in Paula but never reads the
   CIA-B ICR leaves the CIA's /INT line asserted, and Paula latches EXTER
   again the moment it is cleared: the 68k never leaves the handler.
   Shadow Fighter AGA's slave replaces the game's level-6 handler with
   exactly such a stub (move.w #$2000,$dff09c / tst.w $dff002 / rte), and
   on the A500 2,050,218 level-6 interrupts went by in 841 frames while the
   game stood still after its title, right after a WHDLoad load. WinUAE, like
   a real machine, only storms there if CIA-B is asserting, and there it is
   not; here a CIA-B source was left pending. No program acknowledges EXTER
   a thousand times in one frame on purpose - a CIA sample player at 22 kHz
   does 440 - so past that the ICR is read on the program's behalf, which
   releases the line, and the sources it reported are switched off: the
   program was not servicing them. exter_storm_icr keeps what the read saw. */
static void exter_storm_check(void)
{
    static uint32_t frame, n;
    uint32_t f = aga_diag_ctr[AGA_D_FRAMES];
    if (f != frame) { frame = f; n = 0; }
    if (++n != 1000) return;
    uint8_t icr = (uint8_t)ps_read_8(0xBFDD00);     /* the read clears it and lets go of /INT */
    ps_write_8(0xBFDD00, icr & 0x1F);               /* bit 7 clear: those sources off */
    ps_write_16(0xDFF09C, INTF_EXTER);              /* and the latch Paula took meanwhile */
    aga_diag_inc(AGA_D_EXTER_STORMS);
    aga_diag_set(AGA_D_EXTER_STORM_ICR, icr);
    if (aga_diag_ctr[AGA_D_EXTER_STORMS] <= 4)
        aga_diag_log("[AGA] level-6 storm: EXTER acknowledged 1000 times in a frame and CIA-B"
                     " never read - ICR read for the program, its sources switched off");
}

static void hook_ext_write_body(void *u, uint32_t reg, uint16_t v)
{
#ifdef FRF_VIRTUAL_AGA
    (void)u;
    /* Standalone machine: the virtual chipset is authoritative.  Extension
       writes are host-side effects only; nothing is mirrored to a physical
       Paula/Agnus/CIA. */
    switch (reg) {
    case SERDAT:
        /* Preserve WHDLoad's serial exception dump for diagnostics. */
        aga_diag_serial((uint8_t)(v & 0xFF));
        break;
    case POTGO:
        __atomic_store_n(&frf_potgo, (uint16_t)(v & 0xFF00u), __ATOMIC_RELEASE);
        break;
    default:
        break;
    }
    return;
#else

    (void)u;
    if (aga_audio_reg_write(reg, v)) return;     /* AUDx*, DMACON audio bits -> real Paula */
    switch (reg) {
    case INTREQ:
        /* clearing PORTS/EXTER in the virtual INTREQ: also drop the real line */
        ps_write_16(0xDFF09C, v & (INTF_PORTS | INTF_EXTER));
        if (v & INTF_EXTER) exter_storm_check();
        break;
    case INTENA: {
        /* mirror only the real-hardware sources so the real Paula asserts IPL
           for CIAs. A CLEAR of EXTER is always forwarded - that is how the
           real line gets switched off in the first place. A SET of it is
           dropped once the lock is on, so nothing re-arms the level 6 the
           game has no handler for. */
        int is_set = (v & 0x8000) != 0;
        ps_write_16(0xDFF09A, (v & 0x8000) | (v & (INTF_INTEN | INTF_PORTS | INTF_EXTER)));
        if (aga_game_mode && !is_set && (v & INTF_EXTER)) {
            if (!game_taken) aga_ecs_forget_pending = 1;
            game_taken = 1;
        }
        if (aga_game_mode && !aga_exter_locked && !aga_keep_ciab && !is_set && (v & INTF_EXTER)) {
            /* WHDLoad has just switched every interrupt off: the OS is gone and
               the machine is the game's now. Silence the CIA-B interrupt
               sources AmigaOS left running.
               Without this they keep the CIA's /INT line asserted, so Paula
               re-latches EXTER the instant the handler clears it and the 68k
               drowns in level 6 - which outranks the keyboard (2) and the
               game's own vertical blank (3). That is exactly what Banshee did:
               its AGA title screen rendered perfectly and then went nowhere,
               with WHDLoad's F10 quit key dead and INTREQ=$2000 as the last
               thing the CPU ever wrote.
               Killing the sources rather than Paula's EXTER enable is what
               serves both kinds of game. A plain slave installs no level 6
               handler and now never sees one. A KickEmu title boots a whole
               Kickstart whose timer.device runs on these very timers, and is
               free to enable exactly the ones it wants - which suppressing
               EXTER at Paula denied it, leaving every task that waits on a
               timer waiting for ever. */
#ifdef FRF_VIRTUAL_AGA
            extern void frf_vcia_ciab_takeover_request(void);
            frf_vcia_ciab_takeover_request();
#else
            ps_write_8(0xBFDD00, 0x7F);        /* CIA-B ICR: clear every enable */
            (void)ps_read_8(0xBFDD00);         /* and the read drops a pending line */
#endif
            aga_exter_locked = 1;
            aga_loct_fix_pending = 1;
            aga_diag_log("[AGA] WHDLoad took the machine: stale CIA-B interrupt sources silenced");
        }
        break;
    }
    case ADKCON:
        /* The UART and disk bits are real; the audio ones (0-7, which
           channel modulates which) are the emulated Paula's. Forwarded as
           well, a game that set one made the REAL channels - my sound's
           output rings - modulate each other until the next restart
           (found by RGB2RTG). */
        if (v & 0x7F00) ps_write_16(0xDFF09E, v & 0xFF00);
        break;
    case DSKPTH:                       /* a real drive's transfer: see dskb_len_write */
        dskb_pt = ((uint32_t)(v & 0x1F) << 16) | (dskb_pt & 0xFFFF);
        ps_write_16(0xDFF020, v);
        break;
    case DSKPTL:
        dskb_pt = (dskb_pt & 0x1F0000) | (v & 0xFFFE);
        ps_write_16(0xDFF022, v);
        break;
    case DSKLEN:
        dskb_len_write(v);
        break;
    case SERDAT:
        /* Every character the 68k transmits. WHDLoad's crash handler dumps its
           register state down this line and then waits for a keypress from a
           terminal that is not there - which is exactly the state a hung game
           leaves the machine in. Recording the bytes is how I fucking read it. */
        aga_diag_serial((uint8_t)(v & 0xFF));
        ps_write_16(0xDFF030, v);
        break;
#ifdef FRF_VIRTUAL_AGA
    case POTGO:
        /* Standalone mode has no physical Paula to remember POTGO. Retain its
           OUT/DAT state so POTGOR behaves like the controller port. START is
           a strobe and therefore is not retained. */
        __atomic_store_n(&frf_potgo, (uint16_t)(v & 0xFF00u), __ATOMIC_RELEASE);
        break;
#endif
    default:
        ps_write_16(0xDFF000 + reg, v);
        break;
    }
#endif
}

static void hook_ipl(void *u, int ipl)
{
    (void)u;
    if (ipl) aga_diag_inc(AGA_D_IPL_HOOK);
    emu68_set_aga_ipl(ipl);
}

/* Beam interpolation base, published by the chipset thread after every step so
   the trap path can work out where the beam is NOW without taking the lock. */
static volatile uint64_t beam_tick;          /* cntvct() at the last step */
static volatile uint64_t ticks_per_cck;      /* whole ticks; 0 until the loop runs */

static volatile uint32_t last_trap_pc;   /* 68k PC at the last register write */
/* The PC is stable and the write count climbs steadily, so the 68k is looping
   over the chipset - but BPLCON0 and DMACON never change, so it is hitting
   something else. These say which. */
static volatile uint32_t last_wr;        /* reg << 16 | value, last write */
static volatile uint32_t last_rd;        /* reg << 16 | value, last read  */

/* FRF v0.10.15 NEXT32 — diagnostics only.
   v0.10.15 proved the first 16 accesses are WHDLoad's normal state save:
     INTENA=$7FFF, then reads $DFF002,$004,...,$01E.
   Skip those 16 known-good accesses and capture the NEXT 32. */
#define WHD_TRACE_N 32u
#define WHD_TRACE_SKIP 16u
static volatile uint32_t whd_trace_pc[WHD_TRACE_N];
static volatile uint32_t whd_trace_meta[WHD_TRACE_N];
static volatile uint32_t whd_trace_n;
static volatile uint32_t whd_trace_seen;
static volatile uint32_t whd_trace_armed;

static inline void whd_trace_rearm(void)
{
    __atomic_store_n(&whd_trace_n, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&whd_trace_seen, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&whd_trace_armed, 1u, __ATOMIC_RELEASE);
}
static inline void whd_trace_push(int wr, uint32_t reg, int size,
                                  uint16_t value, uint32_t pc)
{
    if (!__atomic_load_n(&whd_trace_armed, __ATOMIC_ACQUIRE)) return;
    uint32_t seen = __atomic_fetch_add(&whd_trace_seen, 1u, __ATOMIC_ACQ_REL);
    if (seen < WHD_TRACE_SKIP) return;
    uint32_t n = seen - WHD_TRACE_SKIP;
    if (n >= WHD_TRACE_N) {
        __atomic_store_n(&whd_trace_armed, 0u, __ATOMIC_RELEASE);
        return;
    }
    whd_trace_pc[n] = pc;
    whd_trace_meta[n] = (wr ? 0x80000000u : 0u)
                      | (((uint32_t)size & 7u) << 28)
                      | ((reg & 0x0fffu) << 16)
                      | value;
    __atomic_store_n(&whd_trace_n, n + 1u, __ATOMIC_RELEASE);
    if (n + 1u >= WHD_TRACE_N)
        __atomic_store_n(&whd_trace_armed, 0u, __ATOMIC_RELEASE);
}

static volatile int frame_pending;

static void hook_frame(void *u)
{
    (void)u;
    frames_done++;
    frame_pending = 1;          /* presented outside the lock by the chipset loop */
}

/* each drawn line, from the core's cache: the RGB port keeps its own copy
   (aga_rgbout_line), so core 1 no longer reads the uncached page back */
static void hook_line(void *u, int y, int page, const uint32_t *line)
{
    (void)u;
    aga_rgbout_line(y, page, line);
}

/* The chipset for a snapshot, in agahost's regs.bin/colors.bin terms: the last
   value written to each register - which for BPLxPT and COPxLC is the frame's
   start value, the program writes them once a frame - except the set/clear
   registers, whose state is what their R registers read. Called by the
   housekeeper at a soft reset, without the lock: a torn value costs a pixel. */
void aga_glue_snapshot_regs(uint16_t *regs, uint32_t *colors)
{
    if (!aga) return;
    for (int i = 0; i < 256; i++) regs[i] = aga_peek_reg(aga, (uint32_t)i * 2);
    regs[0x096 >> 1] = aga_custom_rget(aga, 0x002);    /* DMACON <- DMACONR */
    regs[0x09A >> 1] = aga_custom_rget(aga, 0x01C);    /* INTENA <- INTENAR */
    regs[0x09E >> 1] = aga_custom_rget(aga, 0x010);    /* ADKCON <- ADKCONR */
    for (int i = 0; i < 256; i++) colors[i] = aga_peek_color(aga, i);
}

/* ---- init ----------------------------------------------------------------- */
static inline void chip7_reset_clock(void);
static int aga_start_ntsc;   /* what this kernel was BUILT for; regs[] does not know */

int aga_glue_init(uint8_t *chipram_va, uint32_t size, uint8_t *fb_va, uintptr_t fb_phys, int ntsc, int enable)
{
    aga_start_ntsc = ntsc;
    aga_config_t cfg = { .ntsc = ntsc, .blit_immediate = 0,   /* a blit must not finish inside its own BLTSIZE write */ .collisions = 0, .render = 1 };
    chip_va = chipram_va;
    chip_size = size;
    aga_diag_set_chip(chip_va, size);
    aga = aga_create(chip_va, size, &cfg);
    if (!aga) return -1;
#ifdef FRF_VIRTUAL_AGA
    /* Standalone AGA machine: HUD is OFF unless config.txt explicitly loads
       dtoverlay=diagnostic,hud.  The overlay creates /emu68/diag/frf-hud. */
    int frf_hud = 0;
    {
        of_node_t *dn = dt_find_node("/emu68/diag");
        frf_hud = dn && dt_find_property(dn, "frf-hud");
    }
    aga_video_overlay_allow(frf_hud);
    aga_video_overlay_enable(frf_hud);
    aga_overlay_idle = frf_hud;
    kprintf("[AGA] FRF v0.10.15: native AGA/Lisa personality; HUD %s by config\n",
            frf_hud ? "ENABLED" : "OFF");
#endif
    /* The RGB port's line fingerprints and line copy (aga_rgbout_line): a
       Pi 4 only. On a Pi 3 core 3 is at its limit and anything it does per
       line costs frames - and I have the numbers: the copy 902 skipped
       instead of 19 (Jim Power), the fingerprints alone - 0.5 ms a frame -
       457, with the Pi cool (Pi 3 A+, 2026-10-11). */
    aga_hooks_t h = { hook_ext_read, hook_ext_write, hook_ipl, hook_frame, NULL, aga_is_pi3() ? NULL : hook_line };
    aga_set_hooks(aga, &h);
    aga_lock_v = 0;
    aga_audio_init(chip_va, size);
    /* framebuffer is allocated and mapped here on core 0, before the 68k starts */
    if (aga_video_init_at(fb_va, fb_phys) != 0) {
        /* No display on this board (a Pi 3): then do not render either. The
           renderer streams a 900 KB frame through the shared L2 fifty times a
           second for nobody, and on the A53 cluster that is the working set
           the game's own chip RAM traffic evicts: the PMU counted 108k L2
           refills in one frame against 2k in a quiet one (2026-09-14). */
        aga_set_render(aga, 0);
        kprintf("[AGA] continuing without video output, renderer off\n");
    } else {
        /* the core renders into the HVS pages themselves - one line at a time,
           through its line buffer; present() then only points the HVS at it */
        {
            size_t frb = (size_t)AGA_OUT_W * AGA_OUT_H * 4;
            aga_set_framebuffers3(aga,
                                  (uint32_t *)fb_va,
                                  (uint32_t *)(fb_va + frb),
                                  (uint32_t *)(fb_va + 2 * frb));
        }
    }
    /* the mix on HDMI too, playing silence from now on (aga_hdmi.c) */
    aga_hdmi_boot(fb_va, fb_phys);
#ifdef FRF_VIRTUAL_AGA
    /* Standalone machine has no agaboot Sound window to send $5821.
       Use the simple/classic Paula mix first and start HDMI automatically. */
    aga_hdmi_paula(0);
    aga_hdmi_enable(1);
    kprintf("[AGA] FRF v0.10.15: standalone HDMI Paula audio requested at boot\n");
#endif
    aga_diag_init();

#ifdef FRF_VIRTUAL_AGA
    /* FRF v0.10.15: standalone Pi3 machine mode is the coherent direct-MMU
       model used by the last Workbench-working lineage.  If a non-MMU build
       is explicitly selected, retain the redirect implementation below. */
    if (enable && aga_use_mmu) {
        aga_chip_redirect = 0;
        aga_code_chip = NULL;       /* ROM overlay is still active here */
        aga_code_chip_size = 0;
        kprintf("[AGA] coherent MMU Chip-RAM mode armed; OVL will map 2 MB at zero\n");
    } else if (enable) {
        redirect_lo = 0;
        redirect_hi = size;
        aga_chip_redirect = 1;

        /* Do NOT publish aga_code_chip yet: OVL still maps Kickstart at zero.
           The overlay transition in vectors.c switches code backing at the
           exact ROM->Chip boundary. */
        aga_code_chip = NULL;
        aga_code_chip_size = 0;

        chip7_reset_clock();
        __asm__ volatile("dmb ish" ::: "memory");
        kprintf("[AGA] CHIP7 machine redirect armed: $000000-$%06x\n", size - 1);
    }
#endif

    aga_enabled = enable;
    kprintf("[AGA] chipset core ready, %d KB chip RAM at %p, %s\n", size >> 10, chip_va,
            enable ? "machine mode" : "parked (sandbox available)");
    return 0;
}

/* ---- diagnostics: let aga_diag.c see the virtual chipset ------------------
   Read without the lock. These are single words the chipset thread writes; a
   torn value would at worst mislead one dump, and taking the mutex from the
   diag read path would serialise against the chipset for no benefit. */
uint16_t aga_glue_peek_reg(uint32_t reg)   { return aga ? aga_peek_reg(aga, reg) : 0; }
uint32_t aga_glue_peek_bplpt(int plane)    { return aga ? aga_peek_bplpt(aga, plane) : 0; }
uint16_t aga_glue_peek_state(int which)
{
    if (!aga) return 0;
    /* the accumulated state, not the last value written - see the note in
       aga_sandbox_leave about why those differ for the set/clear registers */
    return aga_custom_rget(aga, which == 0 ? DMACONR : (which == 1 ? INTENAR : INTREQR));
}

/* For the RGB port (core 1), read the same way: the list the copper starts
   every frame from, as COP1LC was last written - 0 while copper DMA is off,
   when no list runs at all - and whether WHDLoad has taken the machine. A
   COP1LC caught between its two word writes is one frame's wrong answer. */
uint32_t aga_glue_copper_list(void)
{
    if (!aga || (aga_peek_dmacon(aga) & (DMAF_DMAEN | DMAF_COPEN)) != (DMAF_DMAEN | DMAF_COPEN)) return 0;
    return (((uint32_t)aga_peek_reg(aga, 0x080) << 16) | aga_peek_reg(aga, 0x082)) & 0x1FFFFE;
}
int aga_glue_game_taken(void) { return game_taken; }
#ifdef FRF_VIRTUAL_AGA
void aga_virtual_cia_irq(uint16_t bit, int active)
{
    /* CIA IRQ output is level-driven, but Paula INTREQ is latched:
       falling CIA IRQ does not acknowledge the Paula request.
       Only guest INTREQ writes clear it. */
    if (!aga || !active) return;
    lock();
    aga_custom_wput(aga, INTREQ, (uint16_t)(0x8000u | bit));
    unlock();
    __asm__ volatile("dsb ish; sev" ::: "memory");
}
#endif
/* ...and a word of the virtual chip RAM, as the 68k sees it (0 past its end) */
uint16_t aga_glue_chip_word(uint32_t addr)
{
    addr &= ~1u;
    if (!chip_va || addr + 2 > chip_size) return 0;
    return (uint16_t)(((unsigned)chip_va[addr] << 8) | chip_va[addr + 1]);
}

/* ---- sandbox mode --------------------------------------------------------- */
/* Registers replayed to the real (ECS) chips when leaving the sandbox: the
   last values written to the virtual chipset (copper included). */
static const uint16_t replay_regs[] = {
    0x100, 0x102, 0x104, 0x106,                 /* BPLCON0-3 */
    0x08E, 0x090, 0x1E4, 0x092, 0x094,          /* DIWSTRT/STOP, DIWHIGH, DDFSTRT/STOP */
    0x108, 0x10A,                               /* BPL1MOD/2MOD */
    0x0E0, 0x0E2, 0x0E4, 0x0E6, 0x0E8, 0x0EA, 0x0EC, 0x0EE, 0x0F0, 0x0F2, 0x0F4, 0x0F6, /* BPL1-6PT */
    0x120, 0x122, 0x124, 0x126, 0x128, 0x12A, 0x12C, 0x12E,                          /* SPR0-3PT */
    0x130, 0x132, 0x134, 0x136, 0x138, 0x13A, 0x13C, 0x13E,                          /* SPR4-7PT */
    0x0A0, 0x0A2, 0x0A4, 0x0A6, 0x0A8, 0x0B0, 0x0B2, 0x0B4, 0x0B6, 0x0B8,            /* AUD0/1 */
    0x0C0, 0x0C2, 0x0C4, 0x0C6, 0x0C8, 0x0D0, 0x0D2, 0x0D4, 0x0D6, 0x0D8,            /* AUD2/3 */
    0x080, 0x082, 0x084, 0x086,                 /* COP1LC, COP2LC */
};

static inline uint32_t rd_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void wr_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ---- real floppy drives in the sandbox --------------------------------------
   With System wide on (A1kmode turns it on at every boot) AmigaOS runs on the
   emulated chipset, and trackdisk's DSKPT, DSKLEN, DSKSYNC and ADKCON writes
   come here and go on to the real Paula - the core keeps them only while an
   ADF sits in a virtual drive. That alone never read a track, from the
   internal drive or an external Gotek: the real disk DMA had been off since
   entry, the data would have landed in the real chip RAM while the OS reads
   the Pi's copy, and the real DSKBLK never reached the emulated INTREQ, so
   trackdisk waited until it gave up (found 2026-10-02).
   Bridged now. The transfer runs on the real Paula at the OS's own buffer
   address. Below the machine's real chip RAM size the real chip RAM is the
   OS's map one to one, so whatever owns that buffer in the OS owns it on the
   bus too - my own users of the real chip RAM, the audio ring and the RGB
   port's picture, are agaboot's allocations and never the same memory. A
   write is copied out to the real chip RAM before the second DSKLEN starts
   it. A read is copied in once the real DSKBLK is up, which the INTREQR and
   DSKBYTR reads of a waiting program find out (vectors.c), and the emulated
   DSKBLK is raised then. A buffer past the real chip RAM, in what agaboot
   lends, is out of Paula's reach: that transfer is not started, as before. */
static void bus_to_chip(uint32_t addr, uint32_t bytes)
{
    uint32_t end = addr + bytes;
    if ((addr & 2) && addr < end) {
        uint16_t w = (uint16_t)ps_read_16(addr);
        chip_va[addr] = (uint8_t)(w >> 8); chip_va[addr + 1] = (uint8_t)w;
        addr += 2;
    }
    for (; addr + 4 <= end; addr += 4) wr_be32(chip_va + addr, ps_read_32(addr));
    if (addr < end) {
        uint16_t w = (uint16_t)ps_read_16(addr);
        chip_va[addr] = (uint8_t)(w >> 8); chip_va[addr + 1] = (uint8_t)w;
    }
}

static void chip_to_bus(uint32_t addr, uint32_t bytes)
{
    uint32_t end = addr + bytes;
    if ((addr & 2) && addr < end) {
        ps_write_16(addr, ((unsigned)chip_va[addr] << 8) | chip_va[addr + 1]);
        addr += 2;
    }
    for (; addr + 4 <= end; addr += 4) ps_write_32(addr, rd_be32(chip_va + addr));
    if (addr < end) ps_write_16(addr, ((unsigned)chip_va[addr] << 8) | chip_va[addr + 1]);
}

/* The real DSKBLK is up: the transfer is over. A read's track comes in. */
static int dskb_done(void)
{
    if (!aga_dskb_pending) return 0;
    if (!(ps_read_16(0xDFF01E) & INTF_DSKBLK)) return 0;
    ps_write_16(0xDFF09C, INTF_DSKBLK);
    if (!dskb_write) bus_to_chip(dskb_pt, dskb_bytes);
    aga_dskb_pending = 0;
    return 1;
}

/* Under the chipset lock, from hook_ext_write_body. */
static void dskb_len_write(uint16_t v)
{
    if (!(v & 0x8000)) {
        /* DMA off. A program that polled DSKBYTR may switch it off before it
           looks at the buffer: finish a transfer that is over first. */
        if (aga_dskb_pending && dskb_done())
            aga_custom_wput(aga, INTREQ, 0x8000 | INTF_DSKBLK);
        aga_dskb_pending = 0;
    } else if (dskb_last_len & 0x8000) {
        /* the second write with DMAEN: the real transfer starts with it */
        uint32_t bytes = (uint32_t)(v & 0x3FFF) * 2;
        if (dskb_pt + bytes > chip_real) {
            if (!(dskb_said & 2)) {
                dskb_said |= 2;
                aga_diag_log("[AGA] real floppy: the buffer is past the real chip RAM,"
                             " Paula cannot reach it - transfer not started");
            }
            dskb_last_len = v;
            return;
        }
        dskb_write = (v & 0x4000) != 0;
        if (dskb_write) chip_to_bus(dskb_pt, bytes);
        ps_write_16(0xDFF09C, INTF_DSKBLK);                       /* nothing stale */
        ps_write_16(0xDFF096, 0x8000 | DMAF_DMAEN | DMAF_DSKEN);  /* off since entry */
        dskb_bytes = bytes;
        aga_dskb_pending = 1;
        if (!(dskb_said & 1)) {
            dskb_said |= 1;
            aga_diag_log("[AGA] real floppy: a transfer handed to the real Paula");
        }
    }
    dskb_last_len = v;
    ps_write_16(0xDFF024, v);
}

/* The 68k trap path, outside the chipset lock: an INTREQR or DSKBYTR read
   while a transfer runs. */
void aga_disk_bridge_poll(void)
{
    if (!dskb_done()) return;
    lock();
    aga_custom_wput(aga, INTREQ, 0x8000 | INTF_DSKBLK);
    unlock();
}

/* Give the real chipset its DMA and interrupts back. Used only by the
   aga.enter_stop bring-up path, so a partial entry leaves a working machine
   instead of a deaf one. */
/* defined below, used by the entry handshake above them */
static inline uint64_t cntvct(void);
static inline uint64_t cntfrq(void);

/* The real ADKCON's audio bits (0-7, which channel modulates which) as the
   OS had them at entry. The leave after a WHDLoad game gives the real
   Paula these back, not the game's: replayed from the emulated chipset, a
   modulation bit a game left set went to the real chip and was seeded into
   the next game's chipset at its entry, where it now mutes channels (found
   by review, 2026-10-05). */
static uint16_t entry_adk_audio;

/* The ECS/AGA choice as the chipset loop last saw it, for its "AGA after
   all" log line. Set at every entry to the mode this session was given -
   the loop parks before the leave puts the core back to AGA, so a value
   kept from the last session logged a flip at the next AGA session's first
   frame, and missed one that came before this session's first frame (found
   by review). */
static int ecs_seen = -1;
static int ecs_entry;              /* the chipset this session was entered with: 1 ECS Denise */
static unsigned ecs_signs_logged;  /* aga_ecs_signs bits already in the log this session */

static void restore_real(uint16_t dmacon, uint16_t intena, uint16_t adkcon)
{
    ps_write_16(0xDFF09E, 0x8000 | (adkcon & 0x7FFF));
    ps_write_16(0xDFF096, 0x8000 | (dmacon & 0x03FF));
    ps_write_16(0xDFF09A, 0x8000 | (intena & 0x7FFF));
    aga_diag_log("[AGA] enter: stopped early, real chipset restored");
}

/* How much real chip RAM the machine has, probed on the bus with the 68k
   stalled and the real chipset quiet. The virtual chip RAM is always 2 MB -
   the game never runs in the real one - so this only decides how much is
   copied in at entry and back out at leave. Copying 2 MB out of a 1 MB
   machine reads the Agnus mirror, and copying it back would put the upper
   half over the OS. Two things distinguish real RAM from the alternatives:
   a pattern written at the top must survive a read of another address
   (an open bus hands back whatever was on it last), and must not appear at
   the same offset half a megabyte down (a mirror). Everything written is
   put back. */
static uint32_t probe_real_chip(void)
{
    static const uint32_t sizes[] = { 2u << 20, 1u << 20 };
    for (unsigned i = 0; i < 2; i++) {
        uint32_t s = sizes[i], hi = s - 16, lo = s / 2 - 16;
        uint32_t save_hi = ps_read_32(hi), save_lo = ps_read_32(lo);
        uint32_t pat = save_hi ^ 0x5A5AA5A5u;
        ps_write_32(hi, pat);
        (void)ps_read_32(lo);                    /* disturb the bus before reading back */
        int held = ps_read_32(hi) == pat;
        int mirror = ps_read_32(lo) == pat;
        ps_write_32(hi, save_hi);
        ps_write_32(lo, save_lo);
        if (held && !mirror) return s;
    }
    return 512u << 10;
}

int aga_sandbox_enter(void)
{
#ifdef FRF_VIRTUAL_AGA
    aga_diag_log("[AGA] standalone firewall: sandbox entry ignored");
    return 0;
#else
    if (!aga || aga_enabled) return 0;
    aga_diag_log("[AGA] enter 1: begin");
    /* Bring-up: stop after step N and return, leaving the machine untouched.
       Recovery from a failed entry needs a power cut, which resets the Pi and
       wipes the diag log and counters - so no instrumentation inside this
       function can be read afterwards. The only observable that survives is
       whether the machine is still alive, so make that the signal: raise the
       stop point one step at a time and see which step kills it.
       Steps 1-2 touch nothing, so the machine MUST survive them; if it does
       not, the fault is in the trap dispatch or the bus reads, before a single
       line of my chipset logic even gets to run. */
    if (aga_enter_stop && aga_enter_stop < 2) return 0;
    kprintf("[AGA] sandbox: entering\n");
    /* HDMI's sound back at 48 kHz, should an AHI program have left it at
       another rate: a game's Paula needs no more, and on a Pi 3 a higher
       rate costs core 3 time in every frame */
    aga_hdmi_rate_default();
    /* 1. readable register state of the real chips, BEFORE anything is changed */
    uint16_t dmacon = ps_read_16(0xDFF002), adkcon = ps_read_16(0xDFF010);
    entry_adk_audio = adkcon & 0x00FF;     /* the OS's own, put back at the leave */
    uint16_t intena = ps_read_16(0xDFF01C), intreq = ps_read_16(0xDFF01E);
    aga_diag_log("[AGA] enter 2: read real registers");
    if (aga_enter_stop && aga_enter_stop <= 2) return 0;

    /* 2. quiet the real chips FIRST: no display/copper/blitter/sprite/disk/audio
          DMA, no interrupts (PORTS/EXTER come back through the INTENA mirror
          below).

          This used to happen *after* the chip RAM copy below, and that ordering
          is what made sandbox entry look like a hang. The copy is 512K bus
          reads; with Agnus still running display and copper DMA, the PiStorm
          only gets the bus slots the chipset leaves over, which stretches an
          already slow operation several-fold. All of it runs in the 68k's abort
          handler on core 0 with interrupts masked, so the Amiga is frozen
          throughout - long enough to be indistinguishable from a dead machine. */
    ps_write_16(0xDFF096, 0x01FF);
    /* Clear every interrupt enable EXCEPT PORTS (bit 3) and EXTER (bit 13).
       The chipset loop mirrors CIA interrupts into the virtual INTREQ by
       watching emu68_get_real_ipl(), but the real Paula only raises an IPL for
       sources INTENA still enables - so clearing all of them left the mirror
       with nothing to mirror. timer.device and the CIAs then go silent: task
       timeouts stop and the network stack dies, while the vertical blank keeps
       running. That is precisely the observed failure: the pointer still moves
       and nothing else works.

       hook_ext_write (above) already forwards PORTS/EXTER to the real chipset,
       so keeping them enabled here is what the rest of the design assumes. */
    aga_exter_locked = 0;      /* armed, not engaged: the OS still needs its CIAs here */
    game_taken = 0;            /* and the stall check waits for WHDLoad's takeover */
    aga_diag_wave_freeze(0);   /* a fresh recording for this game */
    aga_diag_env_freeze(0);
    ps_write_16(0xDFF09A, 0x7FFF & ~(uint16_t)(INTF_PORTS | INTF_EXTER));
    ps_write_16(0xDFF09C, 0x7FFF);
    aga_diag_log("[AGA] enter 3: real chipset quiet");
    /* From here the real chipset is deaf: step 3 cleared DMACON and INTENA,
       and AmigaOS cannot survive without interrupts. So a stop at step 3 or
       beyond MUST hand the chipset back before returning, or the machine
       freezes for reasons that have nothing to do with the step under test -
       which is precisely how the earlier bisects fooled the crap out of me. */
    if (aga_enter_stop && aga_enter_stop <= 3) { restore_real(dmacon, intena, adkcon); return 0; }

    /* 3. now copy the OS state out of real chip RAM, with the bus to ourselves.
          Drop Emu68's software I/D caches first so no stale line is served
          once the redirect is on. NOT flushed from here: cache_flush_all
          writes dirty lines back with plain stores to 68k addresses, and a
          plain store to a bus address from INSIDE the abort handler is a
          nested abort. agaboot does CacheClearU() before it sends the magic
          word, so the 68k has already flushed in its own context by now. */
    cache_invalidate_all(DCACHE);
    cache_invalidate_all(ICACHE);
    chip_real = probe_real_chip();
    if (chip_real > chip_size) chip_real = chip_size;
    aga_diag_set(AGA_D_CHIP_REAL_KB, chip_real >> 10);
    kprintf("[AGA] sandbox: %d KB real chip RAM, %d KB virtual\n", chip_real >> 10, chip_size >> 10);
    for (uint32_t a = 0; a < chip_real; a += 4)
        wr_be32(chip_va + a, ps_read_32(a));
    for (uint32_t a = chip_real; a < chip_size; a += 4)   /* the part the machine lacks: lent to the OS by agaboot, empty */
        wr_be32(chip_va + a, 0);
    aga_diag_log("[AGA] enter 4: 2MB chip RAM copied");
    if (aga_enter_stop && aga_enter_stop <= 4) { restore_real(dmacon, intena, adkcon); return 0; }
    /* 4. seed the virtual chipset with that state.
     *
     * DMACON is copied MINUS the channels whose source registers cannot be
     * recovered. COP1LC, the bitplane pointers, the sprite pointers, BPLCON0
     * and DIWSTRT are all WRITE-ONLY on the real chipset: step 1 can read
     * DMACON, ADKCON, INTENA and INTREQ and nothing else. So a faithful copy of
     * DMACON says "copper, bitplane and sprite DMA are all running" while every
     * pointer those need is zero.
     *
     * The copper is the one that does damage. Enabled with COP1LC = 0, it
     * fetches chip RAM address 0 - the 68k exception vectors - and executes it
     * as a copper list: arbitrary MOVEs to arbitrary registers, fifty times a
     * second. It flattens BPLCON0 and the bitplane pointers as fast as a game
     * can set them, which is why EVERY WHDLoad game showed a grey screen under
     * the sandbox, ECS and AGA alike, while `agastat REGS` reported BPLCON0
     * $0000 and all eight BPLxPT $00000000.
     *
     * Nothing is lost by starting them off. AmigaOS is on the RTG screen here
     * and is not using the native display, and a WHDLoad game programs its own
     * copper list, bitplane pointers and DMACON before it draws anything.
     */
#define DMAF_UNRECOVERABLE (DMAF_AUDEN | DMAF_SPREN | DMAF_COPEN | DMAF_BPLEN)
    lock();
    aga_reset(aga);
    ecs_seen = aga_get_ecs(aga);   /* this session's chipset, for the AGA-after-all log */
    ecs_entry = ecs_seen;
    ecs_signs_logged = 0;
    aga_ecs_forget_pending = 0;
    fwd_forget();
    aga_custom_wput(aga, 0x09E, 0x8000 | (adkcon & 0x7FFF));
    aga_custom_wput(aga, 0x096, 0x8000 | (dmacon & 0x03FF & ~DMAF_UNRECOVERABLE));
    /* INTREQ: seed NOTHING pending.
     *
     * The same reasoning as DMAF_UNRECOVERABLE above. The real INTREQ's bits
     * belong to the OS's drivers - audio.device, the serial and disk handlers -
     * which are about to be replaced by a game that knows nothing about them.
     * Copying them in leaves flags set that nothing will ever clear: measured
     * on hardware, INTREQ read $0780 (AUD0-3 pending) for the entire life of a
     * sandboxed game, because the game clears only VERTB. WinUAE at the same
     * point in the same game reads $0040. A game that polls INTREQR - Banshee
     * reads it five times a frame - is being shown bits that could not be set
     * on a real machine. */
    aga_custom_wput(aga, 0x09C, 0x7FFF);            /* clear every pending bit */
    aga_custom_wput(aga, 0x09A, 0x8000 | (intena & 0x7FFF));
    unlock();
    aga_diag_log("[AGA] enter 5: virtual chipset seeded");
    if (aga_enter_stop && aga_enter_stop <= 5) { restore_real(dmacon, intena, adkcon); return 0; }
    /* 5. 68k address 0 now reads the virtual chip RAM. Default: NO MMU - the
          abort handler serves it (aga_chip_read/write). aga.mmu keeps the old
          page-table remap for A/B. */
    if (aga_use_mmu) {
        mmu_map((uintptr_t)chip_va, 0x000000, chip_size, MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
        __asm__ volatile("dsb ish; isb" ::: "memory");
        aga_diag_log("[AGA] enter 6: chip RAM mapped at 0 (MMU)");
    } else {
        redirect_lo = 0;
        redirect_hi = chip_size;
        aga_chip_redirect = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        aga_diag_log("[AGA] enter 6: chip RAM redirected in the abort handler (no MMU)");
    }
    if (aga_enter_stop && aga_enter_stop <= 6) {
        /* Undo the map before handing the chipset back. Leaving it in place
           would point the 68k at virtual chip RAM while Agnus still DMAs from
           the real thing - the machine would break for a reason that has
           nothing to do with whether mmu_map itself is safe. */
        if (aga_use_mmu) { mmu_unmap(0x000000, chip_size); __asm__ volatile("dsb ish; isb" ::: "memory"); }
        else             { aga_chip_redirect = 0; __asm__ volatile("dmb ish" ::: "memory"); }
        aga_diag_log("[AGA] enter 6b: chip RAM redirect undone again");
        restore_real(dmacon, intena, adkcon);
        return 0;
    }
    /* 6. go */
    if (aga_code_direct) {
        aga_code_chip_size = chip_size;
        aga_code_chip = chip_va;   /* code reads skip the software cache from here (cache.c) */
    }
    aga_diag_set(AGA_D_CODE_DIRECT, (uint32_t)aga_code_direct);
    aga_sandbox_active = 1;
    aga_enabled = 1;
    __asm__ volatile("sev");   /* wake core 3 */

    /* Do not commit until core 3 confirms it is running. From here the OS
       has no interrupts of its own - step 3 took them away - so if the
       chipset loop never starts, nothing delivers VERTB and the machine
       freezes with no way to report why. That is what made this fault so
       expensive to chase: it destroys the evidence and needs a power cut,
       which wipes the log.

       core_idle goes to 0 only when core 3 leaves its WFE park, so poll it
       briefly. If it never clears, the SEV/WFE handshake failed: roll
       everything back and return an error the caller can actually read. */
    {
        uint64_t f = cntfrq(), deadline = cntvct() + f / 5;   /* 200 ms */
        while (core_idle && (int64_t)(cntvct() - deadline) < 0)
            __asm__ volatile("sev; yield");
        if (core_idle) {
            aga_enabled = 0;
            aga_sandbox_active = 0;
            aga_code_chip = NULL;
            if (aga_use_mmu) { mmu_unmap(0x000000, chip_size); __asm__ volatile("dsb ish; isb" ::: "memory"); }
            else             { aga_chip_redirect = 0; __asm__ volatile("dmb ish" ::: "memory"); }
            restore_real(dmacon, intena, adkcon);
            aga_diag_log("[AGA] ABORT: core 3 never woke - rolled back");
            kprintf("[AGA] sandbox: core 3 did not start, rolled back\n");
            return 0;
        }
        aga_diag_log("[AGA] core 3 confirmed running");
    }
    /* FRF PROGRAM/RUN: normal SANDBOX/WHDLoad starts HAM6 immediately;
       RUN waits for the child to replace the seeded OS Copper. */
    aga_program_seed_cop1lc = 0;
    aga_program_candidate_cop1lc = 0;
    aga_program_candidate_frames = 0;
    aga_program_rgb_released = 0;
    aga_program_rtg_yield = 0;
    if (!aga_program_mode)
        aga_rgbout_start(chip_real);
    else
        aga_diag_log("[AGA] PROGRAM RUN: RGB/HAM6 armed; waiting for child Copper");
    aga_diag_inc(AGA_D_SANDBOX_IN);
    aga_diag_beam_reset();   /* the raster values from here on are the game's */
    {   /* the SoC at the start of the game, to compare with the leave sample */
        uint32_t tmc = 0, thr = 0;
        extern int aga_soc_status(uint32_t *temp_mc, uint32_t *throttled);
        if (aga_soc_status(&tmc, &thr) == 0) {
            aga_diag_set(AGA_D_SOC_TEMP_ENTER_MC, tmc);
            aga_diag_set(AGA_D_SOC_THROTTLED_ENTER, thr);
        }
    }
    aga_diag_log("[AGA] sandbox entered");
    kprintf("[AGA] sandbox: active (DMACON %04x INTENA %04x)\n", dmacon, intena);
    return 1;
#endif
}

/* Undo everything sandbox entry did, from core 3's own context: chip RAM back
   onto the real bus, the registers replayed to the real chips. Split out of
   aga_sandbox_leave so the WATCHDOG PARK can run it too.

   It did not, and that cost a whole evening of confusion: the park cleared
   aga_enabled and aga_sandbox_active - which sends the register traps back to
   the real chipset - but left aga_chip_redirect set, so the 68k kept reading
   and writing the ARM-side copy of chip RAM while Agnus fetched from the real
   one. The machine survived and the OS answered the network, but every
   display was grey and the next game launched into that half-state and
   greyed too, until someone power-cycled. A recovery path that leaves the
   machine subtly broken is worse than no recovery path. */
static void sandbox_restore(void)
{
    emu68_set_aga_ipl(0);
    aga_audio_stop();
    aga_rgbout_stop();         /* before step 3 copies chip RAM back over the HAM6 copper lists */
    if (aga_leave_stop == 1) { aga_diag_log("[AGA] leave 1: chipset parked"); return; }
    /* code reads go through the software cache again, before chip RAM stops
       being chip_va; it holds no chip line from the sandbox, none went in */
    aga_code_chip = NULL;
    __asm__ volatile("dmb ish" ::: "memory");
    /* 2. 68k address 0 is the real chip RAM again.

       The barrier is not optional. Every other mmu_map/mmu_unmap in this file
       is followed by `dsb ish; isb` - entry's map, and both of entry's rollback
       unmaps. This one was not, so the 68k could keep running on a stale TLB
       entry pointing at virtual chip RAM that is about to stop being the
       machine's memory. */
    if (aga_use_mmu) {
        mmu_unmap(0x000000, chip_size);
        __asm__ volatile("dsb ish; isb" ::: "memory");
    } else {
        /* Drop both software caches - they hold virtual-era lines - then stop
           redirecting. No flush from in here (nested-abort hazard, see entry);
           agaboot CacheClearU()s before sending MAGIC_SANDBOX_OFF, so dirty
           lines already landed in chip_va through the redirect. */
        cache_invalidate_all(DCACHE);
        cache_invalidate_all(ICACHE);
        aga_chip_redirect = 0;
        __asm__ volatile("dmb ish" ::: "memory");
    }
    if (aga_leave_stop == 2) { aga_diag_log("[AGA] leave 2: chip RAM redirect undone"); return; }
    /* A real floppy transfer still running would go on writing the real chip
       RAM after the copy below: stop it. trackdisk, on the real chipset from
       here, times out and tries again. */
    if (aga_dskb_pending) {
        ps_write_16(0xDFF024, 0x4000);
        ps_write_16(0xDFF09C, INTF_DSKBLK);
        aga_dskb_pending = 0;
    }
    /* 3. copy the (restored) OS state back into the real chip RAM */
    for (uint32_t a = 0; a < chip_real; a += 4)          /* only what the machine really has */
        ps_write_32(a, rd_be32(chip_va + a));
    if (aga_leave_stop == 3) { aga_diag_log("[AGA] leave 3: chip RAM copied back"); return; }
    /* 4. replay the register state to the real chips */
    ps_write_16(0xDFF096, 0x03FF);                       /* all DMA off while programming */
    ps_write_16(0xDFF09A, 0x7FFF);
    for (unsigned i = 0; i < sizeof replay_regs / sizeof replay_regs[0]; i++)
        ps_write_16(0xDFF000 + replay_regs[i], aga_peek_reg(aga, replay_regs[i]));
    for (int c = 0; c < 32; c++) {
        uint32_t rgb = aga_peek_color(aga, c);
        ps_write_16(0xDFF180 + 2 * c, ((rgb >> 12) & 0xF00) | ((rgb >> 8) & 0x0F0) | ((rgb >> 4) & 0x00F));
    }
    if (aga_leave_stop == 4) { aga_diag_log("[AGA] leave 4: registers replayed"); return; }
    ps_write_16(0xDFF088, 0);                            /* COPJMP1: copper restarts from COP1LC */
    /* ADKCON, INTENA and DMACON are set/clear registers: bit 15 says whether the
       other bits are set or cleared, so the STATE is an accumulation, not the
       last value written.

       aga_peek_reg returns the last value WRITTEN - chipset.h says so plainly.
       Using it here meant that if the OS's last INTENA write had been a clear of
       one source (bit 15 low, say $0010), leave first cleared everything with
       $7FFF and then wrote $8010, re-enabling exactly one interrupt and leaving
       every other one off. The machine came back with no timer and no CIA
       interrupts: network dead, tasks that wait never woken, a pulled-down menu
       stuck on screen - while the Pi carried on drawing the RTG desktop, so it
       looked alive. Same for DMACON and ADKCON.

       The accumulated state is what the R registers return, so read those. */
    uint16_t adk_now  = aga_custom_rget(aga, ADKCONR);
    uint16_t inte_now = aga_custom_rget(aga, INTENAR);
    uint16_t dmac_now = aga_custom_rget(aga, DMACONR);
    /* The audio bits: the OS's. After a WHDLoad game, the ones read at
       entry - WHDLoad itself puts the OS's custom registers back when a game
       quits. Without one (System wide, a manual SANDBOX ON) the OS ran in
       here and the emulated ADKCON is its own, so a program that set or
       cleared a modulation bit meanwhile keeps it, as on the real machine
       (found by review). */
    uint16_t adk_audio = aga_game_mode ? entry_adk_audio : (uint16_t)(adk_now & 0x00FF);
    ps_write_16(0xDFF09E, 0x7FFF);
    ps_write_16(0xDFF09E, 0x8000 | (adk_now & 0x7F00) | adk_audio);
    ps_write_16(0xDFF09A, 0x8000 | (inte_now & 0x7FFF));
    ps_write_16(0xDFF096, 0x8000 | (dmac_now & 0x07FF));   /* incl. DMAEN and BLTPRI */
    aga_sandbox_active = 0;
    aga_game_mode = 0;
    aga_program_mode = 0;
    aga_program_seed_cop1lc = 0;
    aga_program_candidate_cop1lc = 0;
    aga_program_candidate_frames = 0;
    aga_program_rgb_released = 0;
    aga_program_rtg_yield = 0;
    aga_exter_locked = 0;
    game_taken = 0;
    aga_diag_wave_freeze(1);   /* keep the game's last fifteen seconds of audio */
    aga_diag_env_freeze(1);

    /* Hand the NEXT game a clean chipset, not this one's leftovers.
     *
     * The virtual chipset is created once and lives for as long as the Pi is
     * powered, so without this everything a quitting game leaves behind is
     * still there when the next one starts: bitplane and copper pointers into
     * memory that has since been freed, DMACON with planes and the blitter
     * still enabled, a copper list mid-execution, sprite pointers, audio
     * channels mid-sample, a blit still counted as running. The next game
     * inherits all of it before it has written a single register, and anything
     * that reads back a register it has not set yet gets an answer from the
     * previous game.
     *
     * Chip RAM itself is re-copied from the real machine on the next entry, so
     * only the register state needs clearing. aga_reset() zeroes the lot and
     * recomputes the IPL; emu68_set_aga_ipl(0) after it makes sure the virtual
     * Paula is not still asserting an interrupt level it can no longer justify,
     * since aga_update_ipl only calls the hook when the level CHANGES and the
     * reset makes it 0 by memset without ever passing through the hook. */
    lock();
    /* the standard first: aga_reset takes the line count and VPOSR's chip ID
       from it, so set after the reset the desktop kept an NTSC game's */
    aga_set_ntsc(aga, aga_start_ntsc);   /* the desktop is the machine's own */
    aga_reset(aga);
    fwd_forget();
    aga_set_ecs(aga, 0);     /* next game chooses again; the desktop is AGA */
    unlock();
    emu68_set_aga_ipl(0);

    aga_video_hide();          /* or the last presented frame stays on top of the desktop */
    {   /* the SoC's own account of the game: temperature now, and the sticky
           "throttled / frequency capped / under-voltage since boot" bits.
           One mailbox call, here, where the 68k is stalled in agaboot's trap
           and the RTG driver cannot be mid-call. */
        uint32_t tmc = 0, thr = 0;
        extern int aga_soc_status(uint32_t *temp_mc, uint32_t *throttled);
        if (aga_soc_status(&tmc, &thr) == 0) {
            aga_diag_set(AGA_D_SOC_TEMP_MC, tmc);
            aga_diag_set(AGA_D_THROTTLED, thr);
        }
    }
    aga_diag_inc(AGA_D_SANDBOX_OUT);
    aga_diag_log("[AGA] sandbox left");
    kprintf("[AGA] sandbox: left (DMACON %04x INTENA %04x)\n", dmac_now, inte_now);
    return;
}

int aga_sandbox_leave(void)
{
#ifdef FRF_VIRTUAL_AGA
    aga_diag_log("[AGA] standalone firewall: sandbox leave ignored");
    return 0;
#else
    if (!aga || !aga_sandbox_active) return 0;
    kprintf("[AGA] sandbox: leaving\n");
    /* 1. park the chipset loop and the virtual interrupt line.

       This used to be `aga_enabled = 0; sev; while (!core_idle) yield;` - one
       SEV, then an unbounded spin. Entry deliberately does not do that: it
       repeats the SEV inside the wait and gives up after 200 ms, because a
       single SEV can be missed and core 0 is spinning here inside the 68k's
       abort handler with interrupts masked. If core 3 does not answer, the
       machine dies outright - no console, no counters, no network, power cut
       the only way out. That is exactly what the first ever SANDBOX OFF did.

       So: bounded, with the SEV repeated. If core 3 will not damn well park I
       abandon the leave and stay sandboxed - ugly as hell, but alive and
       reportable. */
    aga_enabled = 0;
    {
        uint64_t f = cntfrq(), deadline = cntvct() + f / 2;   /* 500 ms */
        while (!core_idle && (int64_t)(cntvct() - deadline) < 0)
            __asm__ volatile("sev; yield");
        if (!core_idle) {
            aga_enabled = 1;                 /* it never parked: put it back */
            aga_diag_log("[AGA] ABORT: core 3 would not park, still sandboxed");
            kprintf("[AGA] sandbox: core 3 would not park, leave abandoned\n");
            return 0;
        }
    }
    sandbox_restore();
    return 1;
#endif
}

/* CHIP7 governor is defined below, before the custom-register trap path. */
static inline void chip7_wait(uint64_t far, int size);

/* ---- chip RAM redirection, no MMU (core 0, abort handler) -----------------
 *
 * The old design remapped 68k address 0 onto the virtual chip RAM with the ARM
 * MMU, so that JIT-emitted loads and stores stopped faulting and hit Pi memory
 * directly. Emu68 was not written with its memory map being changed under it,
 * and every access that did NOT go through a plain load - or went through one
 * the map did not cover - was a question I could never quite close.
 *
 * This is the other way round: leave the page tables alone, let every chip RAM
 * access fault into the abort handler exactly as it does for the real bus, and
 * answer it from chip_va instead of the PiStorm. One door. Code and data
 * alike - the JIT's I-cache line fill is a plain 16-byte load from the 68k
 * address, so it comes through here too. And it is faster than the real thing:
 * an abort plus a memory copy beats a bus transaction.
 *
 * Emu68 is built big-endian, so a plain load from chip_va already yields what
 * the 68k expects, and the 128-bit split (value = first 8 bytes, value2 = next
 * 8) matches SYSReadValFromAddr's own convention for unmapped space.
 */
int aga_chip_read(uint64_t far, int size, uint64_t *value, uint64_t *value2)
{
    if (!aga_chip_redirect || far < redirect_lo || far + (uint64_t)size > redirect_hi) return 0;
    chip7_wait(far, size);
    aga_diag_ctr[AGA_D_CHIP_R]++;
    cpu_activity++;
    /* NO instrumentation here. A 16-slot scan on every JIT cache-line fill -
       the hottest path in the machine - slowed it enough that Banshee's intro
       stopped progressing at all. And it answered nothing: the fetch list came
       back empty, because a WHDLoad game's code runs from fast RAM, which is
       mapped and never faults through this door. */
    const uint8_t *p = chip_va + far;
    switch (size) {
    case 1:  *value = *(const uint8_t  *)p; break;
    case 2:  *value = *(const uint16_t *)p; break;
    case 4:  *value = *(const uint32_t *)p; break;
    case 8:  *value = *(const uint64_t *)p; break;
    case 16: *value = *(const uint64_t *)p; *value2 = *(const uint64_t *)(p + 8); break;
    default: return 0;
    }
    return 1;
}

int aga_chip_write(uint64_t far, int size, uint64_t value, uint64_t value2)
{
    if (!aga_chip_redirect || far < redirect_lo || far + (uint64_t)size > redirect_hi) return 0;
    chip7_wait(far, size);
    cpu_activity++;
#ifdef AGA_PROBES
    aga_diag_ctr[AGA_D_CHIP_W]++;
    aga_diag_ctr[AGA_D_CHIPW_B0 + (unsigned)(far >> 18 & 7)]++;
#endif
    last_chip_wr = (uint32_t)far;
    uint8_t *p = chip_va + far;
    switch (size) {
    case 1:  *(uint8_t  *)p = (uint8_t)value;  break;
    case 2:  *(uint16_t *)p = (uint16_t)value; break;
    case 4:  *(uint32_t *)p = (uint32_t)value; break;
    case 8:  *(uint64_t *)p = value;           break;
    case 16: *(uint64_t *)p = value; *(uint64_t *)(p + 8) = value2; break;
    default: return 0;
    }
    return 1;
}

/* ---- the real bus, measured: agaboot BUSTEST ----------------------------------
   Can the kernel draw a picture into REAL chip RAM fast enough for the real
   Denise to show it on the Amiga's own RGB port? A 320 x 256 HAM6 frame is
   61,440 bytes; fifty of them a second is 3 MB/s through a 68000 bus. So
   first a number: agaboot lends 64 KB of chip RAM ($DFF1EC, outside the
   sandbox only, where the OS owns chip RAM and this buffer is damn well mine
   to scribble on) and asks for one timed run per control word $596N:
     0  16-bit writes, the bus lock taken per write (what ps_write_16 does)
     1  32-bit writes, lock per write
     2  16-bit writes, lock per 64 words (a line at a time, as a writer would)
     3  16-bit reads, lock per read
   From the abort handler on core 0, while agaboot holds Disable(): nothing
   else of the 68k's runs meanwhile, the other cores still take the bus. */
#ifdef PISTORM_CLASSIC
extern void ps_bus_acquire(void);
extern void ps_bus_release(void);
extern void ps_write_16_nolock(unsigned int address, unsigned int data);
extern unsigned int ps_read_16_nolock(unsigned int address);
#else
/* PiStorm16/32 keep the lock inside ps_protocol.c: modes 2 and 3 lock per access */
#define ps_bus_acquire()            do { } while (0)
#define ps_bus_release()            do { } while (0)
#define ps_write_16_nolock(a, v)    ps_write_16((a), (v))
#define ps_read_16_nolock(a)        ps_read_16(a)
#endif
static uint32_t bustest_buf;

void aga_bustest_set_buffer(uint32_t addr)
{
    if (!aga_sandbox_active) bustest_buf = addr & ~3u;
}

void aga_bustest_run(int mode)
{
    const uint32_t len = 65536;
    if (aga_sandbox_active || !bustest_buf || bustest_buf + len > chip_size) return;
    uint64_t f = cntfrq(), worst = 0, t0 = cntvct(), tp = t0;
    uint32_t a = bustest_buf, sum = 0;
    switch (mode) {
    case 0:
        for (uint32_t o = 0; o < len; o += 2) {
            ps_write_16(a + o, o);
            uint64_t t = cntvct(); if (t - tp > worst) worst = t - tp; tp = t;
        }
        break;
    case 1:
        for (uint32_t o = 0; o < len; o += 4) {
            ps_write_32(a + o, o);
            uint64_t t = cntvct(); if (t - tp > worst) worst = t - tp; tp = t;
        }
        break;
    case 2:
        for (uint32_t o = 0; o < len; o += 128) {
            ps_bus_acquire();
            for (uint32_t k = 0; k < 128; k += 2) ps_write_16_nolock(a + o + k, o + k);
            ps_bus_release();
            uint64_t t = cntvct(); if (t - tp > worst) worst = t - tp; tp = t;
        }
        break;
    default:
        mode = 3;
        for (uint32_t o = 0; o < len; o += 2) {
            ps_bus_acquire(); sum += ps_read_16_nolock(a + o); ps_bus_release();
            uint64_t t = cntvct(); if (t - tp > worst) worst = t - tp; tp = t;
        }
        break;
    }
    uint64_t t1 = cntvct();
    (void)sum;
    aga_diag_set(AGA_D_BUSTEST_MODE, (uint32_t)mode);
    aga_diag_set(AGA_D_BUSTEST_BYTES, len);
    aga_diag_set(AGA_D_BUSTEST_US, (uint32_t)(((t1 - t0) * 1000000u) / f));
    aga_diag_set(AGA_D_BUSTEST_WORST_NS, (uint32_t)((worst * 1000000000u) / f));
}

/* Redirect test WITHOUT the sandbox: control word $5C0N.
 *
 * Both sandbox entries on the no-MMU path killed the machine before the first
 * counter could be read, and a dead machine says nothing about WHICH of the
 * two things entry does is at fault: switching the chipset (interrupts and
 * DMA away from the real chips) or serving chip RAM from the abort handler.
 * This does only the second. The real chipset keeps running untouched, so
 * the OS keeps its interrupts and caffed keeps talking: if the redirect is
 * what breaks, the break happens on a machine that can still report it.
 *
 *   N=1  the top 64 KB of chip RAM (agastat CHIPTEST's last two rows)
 *   N=2  all of chip RAM: every OS access, ExecBase, vectors, stacks, code
 *   N=0  off again; the window is copied back to the real chip RAM first,
 *        since the OS has been writing to the copy in the meantime.
 *
 * The window is copied from the bus first, so the 68k sees the same bytes
 * either side of the switch. agaboot sends this inside Disable() after a
 * CacheClearU(), exactly like the sandbox words. */
int aga_redirect_test(unsigned n)
{
    if (aga_sandbox_active || !chip_va) return 0;
    if (n == 0) {
        if (!aga_chip_redirect) return 1;
        aga_chip_redirect = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        cache_invalidate_all(ICACHE);
        for (uint32_t a = redirect_lo; a < redirect_hi; a += 4)
            ps_write_32(a, rd_be32(chip_va + a));
        aga_diag_log("[AGA] redirect test: off, window copied back");
        return 1;
    }
    if (aga_chip_redirect) return 1;           /* already on: no double copy */
    uint32_t lo = (n == 1) ? chip_size - 0x10000 : 0, hi = chip_size;
    for (uint32_t a = lo; a < hi; a += 4)
        wr_be32(chip_va + a, ps_read_32(a));
    cache_invalidate_all(ICACHE);
    redirect_lo = lo;
    redirect_hi = hi;
    aga_chip_redirect = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    aga_diag_log(n == 1 ? "[AGA] redirect test: top 64 KB served from chip_va"
                        : "[AGA] redirect test: ALL chip RAM served from chip_va");
    return 1;
}

/* ---- A1200 Chip-RAM bus governor -----------------------------------------
 *
 * FRF v0.10.15 CHIP7
 *
 * The standalone machine must not let Emu68 treat Chip RAM as ordinary host
 * RAM. The real A1200 shared chip-memory domain is clocked from the 7 MHz
 * chipset/bus clock (PAL master 28.37512 MHz / 4 = 7.09378 MHz; NTSC
 * 28.63636 MHz / 4 = 7.15909 MHz). Fast/Z3 RAM is intentionally untouched.
 *
 * One 1/2/4-byte CPU Chip-RAM access consumes one 32-bit bus cycle. Wider
 * accesses generated by Emu68's cache-line fill consume as many 32-bit cycles
 * as the touched address range spans. Idle time is deliberately not banked.
 */
static uint64_t chip7_next_ticks;
static uint32_t chip7_next_frac;
static uint64_t chip7_ticks_per_slot_fp;
static uint32_t chip7_hz;

static inline void chip7_reset_clock(void)
{
    chip7_next_ticks = 0;
    chip7_next_frac = 0;
    chip7_ticks_per_slot_fp = 0;
    chip7_hz = 0;
}

static inline void chip7_wait(uint64_t far, int size)
{
#ifdef FRF_VIRTUAL_AGA
    uint64_t now = cntvct();

    if (!chip7_ticks_per_slot_fp) {
        uint64_t f;
        __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
        chip7_hz = aga_start_ntsc ? 7159090u : 7093780u;
        chip7_ticks_per_slot_fp = (f << 32) / chip7_hz;
        if (!chip7_ticks_per_slot_fp) chip7_ticks_per_slot_fp = 1ull << 32;
    }

    if (!chip7_next_ticks || (int64_t)(now - chip7_next_ticks) > 0) {
        chip7_next_ticks = now;
        chip7_next_frac = 0;
    }

    unsigned slots = (unsigned)(((far & 3u) + (uint64_t)size + 3u) >> 2);
    if (!slots) slots = 1;

    while (slots--) {
        uint64_t inc = chip7_ticks_per_slot_fp;
        uint32_t oldf = chip7_next_frac;
        uint32_t newf = oldf + (uint32_t)inc;
        chip7_next_ticks += (inc >> 32) + (newf < oldf ? 1u : 0u);
        chip7_next_frac = newf;
    }

    while ((int64_t)(cntvct() - chip7_next_ticks) < 0)
        __asm__ volatile("yield");

    __asm__ volatile("dmb ish" ::: "memory");
#else
    (void)far; (void)size;
#endif
}

/* ---- CPU trap path (core 0) ----------------------------------------------- */
int aga_trap_write(uint64_t far, int size, uint64_t value, uint64_t value2)
{
    if (!aga_enabled || far < 0xDFF000 || far >= 0xE00000) return 0;
    uint32_t reg = (uint32_t)far & 0x1FF;
    last_trap_pc = emu68_get_pc();
    last_wr = (reg << 16) | (uint16_t)value;
    /* WHDLoad takeover signature seen in the v0.10.15 film. Re-arm on every
       occurrence so an earlier OS clear cannot steal the capture. */
    if ((reg & 0x1FEu) == INTENA && size == 2 && (uint16_t)value == 0x7FFFu)
        whd_trace_rearm();
    whd_trace_push(1, reg, size, (uint16_t)value, last_trap_pc);
    cpu_activity++;
    AGA_DIAG_INC(AGA_D_TRAPS_W);
    aga_diag_reg(reg, 1);
    AGA_DIAG_TRAP(reg, (uint16_t)value, 1, emu68_get_pc());
#ifdef AGA_PROBES
    aga_diag_set(AGA_D_LAST_TRAP_REG, reg);
    aga_diag_set(AGA_D_LAST_TRAP_VAL, (uint32_t)(uint16_t)value);
    aga_diag_set(AGA_D_LAST_TRAP_RW, 1);
#endif
#ifdef AGA_PROBES
    if ((reg & 0x1FE) == 0x096) {      /* DMACON: bit 15 sets, clear clears */
        uint16_t dv = (uint16_t)value;
        aga_diag_inc(AGA_D_DMACON_W);
        aga_diag_set(AGA_D_DMACON_LAST_VAL, dv);
        if (!(dv & 0x8000) && (dv & 0x0100)) {     /* clearing bitplane DMA */
            aga_diag_inc(AGA_D_DMACON_OFF_W);
            aga_diag_set(AGA_D_DMACON_OFF_PC, emu68_get_pc());
            aga_diag_set(AGA_D_DMACON_OFF_VAL, dv);
        }
    }
    if ((reg & 0x1FE) == 0x1DC) {      /* BEAMCON0: bit 5 clear means NTSC */
        aga_diag_inc(AGA_D_BEAMCON0_W);
        aga_diag_set(AGA_D_BEAMCON0_LAST, (uint32_t)(uint16_t)value);
        if (!((uint16_t)value & 0x0020)) {
            aga_diag_inc(AGA_D_BEAMCON0_NTSC_W);
            aga_diag_set(AGA_D_BEAMCON0_PC, emu68_get_pc());
            aga_diag_set(AGA_D_BEAMCON0_NTSC_SBX, (uint32_t)aga_sandbox_active);
            aga_diag_set(AGA_D_BEAMCON0_NTSC_FRAME, (uint32_t)aga_frame_count(aga));
        }
    }
#endif
    /* Real 68k Chip RAM and custom-register writes share one ordered bus.
       Publish all prior cached Chip RAM stores before exposing the register
       write that may make Copper/blitter/bitplane DMA consume them. */
    __asm__ volatile("dmb ish" ::: "memory");
    lock();
#ifdef AGA_PROBES
    uint64_t hold0;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(hold0));
#endif
    /* The sound driver's writes, in order, with where the beam was. A few
       thousand a second at most; the blitter's millions never get past the
       register test. */
#ifdef AGA_PROBES
    if ((reg >= 0x0A0 && reg < 0x0E0)
        || ((reg & 0x1FE) == 0x096 && (value & 0x020F))
        || (((reg & 0x1FE) == 0x09A || (reg & 0x1FE) == 0x09C) && (value & 0x0780))) {
        uint16_t vp = aga_custom_rget(aga, 0x004), vh = aga_custom_rget(aga, 0x006);
        aga_diag_audw(reg, size, (uint32_t)value, ((uint32_t)(vp & 7) << 8) | (vh >> 8), vh & 0xFF);
    }
    /* Synergy sets all eight BPLxPT from the CPU, never the copper, and on the
       PC it renders frame for frame like WinUAE once the pointers are right.
       On the machine it is corrupt - so: WHEN do those writes arrive? After
       the display window opens, the top of the frame is fetched from the
       previous frame's run-on pointers. */
    if (aga_sandbox_active && reg >= 0x0E0 && reg < 0x100) {
        uint16_t vp = aga_custom_rget(aga, 0x004), vh = aga_custom_rget(aga, 0x006);
        uint32_t line = ((uint32_t)(vp & 1) << 8) | (vh >> 8);
        uint32_t vs = aga_peek_reg(aga, 0x08E) >> 8, ve = (aga_peek_reg(aga, 0x090) >> 8) | 0x100;
        aga_diag_inc(AGA_D_BPLPT_W);
        aga_diag_set(AGA_D_BPLPT_LINE_LAST, line);
        aga_diag_inc(AGA_D_BPLPT_H0 + (line / 40 < 7 ? line / 40 : 7));
        if (line >= vs && line < ve) {
            aga_diag_inc(AGA_D_BPLPT_W_LATE);
            aga_diag_max(AGA_D_BPLPT_LATE_MAX, line);
        }
    }
#endif
#ifdef AGA_PROBES     /* test kernels only: the recorders that found OutRun's copper-only screen */
    /* Who writes BPLCON0, and what: OutRun AGA's own $0211 arrives and is
       $0200 again by the end of the frame, with no copper MOVE to it. A ring
       of the last eight CPU writes, value and 68k PC (slot = count & 7). */
    if ((reg & 0x1FE) == 0x100 || (size >= 4 && (reg & 0x1FE) == 0x0FE) || (size == 16 && reg >= 0x0F2 && reg < 0x100)) {
        uint32_t k = aga_diag_ctr[AGA_D_BC0W_N] & 7;
        uint16_t v16 = (uint16_t)value;
        if (size == 4 && (reg & 0x1FE) == 0x0FE) v16 = (uint16_t)value;          /* .L at $FE: low word is BPLCON0 */
        else if (size == 4) v16 = (uint16_t)(value >> 16);
        else if (size == 8 || size == 16) v16 = 0xFFFF;                          /* part of a wide store: marked */
        aga_diag_ctr[AGA_D_BC0W_VAL0 + k] = v16;
        aga_diag_ctr[AGA_D_BC0W_PC0 + k] = emu68_get_pc();
        aga_diag_ctr[AGA_D_BC0W_N]++;
        aga_diag_ctr[AGA_D_BC0W_FRAME] = aga_diag_ctr[AGA_D_FRAMES];
    }
    {   /* the event log wants every write to these, word by word */
        uint32_t r0 = reg & 0x1FE;
        if (size == 2 || size == 1) aga_dbg_event_cpu(aga, r0, (uint16_t)value);
        else if (size == 4) { aga_dbg_event_cpu(aga, r0, (uint16_t)(value >> 16)); aga_dbg_event_cpu(aga, r0 + 2, (uint16_t)value); }
        else if (size == 8) { for (int w = 0; w < 4; w++) aga_dbg_event_cpu(aga, r0 + 2 * w, (uint16_t)(value >> (48 - 16 * w))); }
    }
    if (reg >= 0x080 && reg <= 0x08A) {           /* COP1LC, COP2LC, COPJMP1, COPJMP2 */
        uint32_t k = aga_diag_ctr[AGA_D_COPW_N] & 7;
        uint16_t v16 = (size == 4) ? (uint16_t)(value >> 16) : (uint16_t)value;
        aga_diag_ctr[AGA_D_COPW_RV0 + k] = (reg << 16) | v16;
        if (size == 4) aga_diag_ctr[AGA_D_COPW_RV0 + k] = (reg << 16) | (uint16_t)value | ((uint32_t)1 << 31); /* .L: low word shown, bit 31 marks it */
        aga_diag_ctr[AGA_D_COPW_PC0 + k] = emu68_get_pc();
        aga_diag_ctr[AGA_D_COPW_N]++;
    }
#endif
    switch (size) {
    case 1: aga_write(aga, reg, (uint32_t)value, 1); break;
    case 2: aga_write(aga, reg, (uint32_t)value, 2); break;
    case 4: aga_write(aga, reg, (uint32_t)value, 4); break;
    case 8:
        aga_write(aga, reg, (uint32_t)(value >> 32), 4);
        aga_write(aga, reg + 4, (uint32_t)value, 4);
        break;
    case 16:
        /* A MOVEM.L of four or more registers reaches me as 128-bit stores
           (value = the first eight bytes, value2 = the next eight, as in
           aga_chip_write). They fell into the default below and were
           dropped without a fucking trace: OutRun AGA loads sixteen registers
           from BPLCON0 up with one movem.l d0-d7,$100(a6), so BPLCON0 kept
           the OS's blank $0200 and the game ran for ever with no bitplane on
           screen - the copper's road and sky, no cars, no scenery, no text. */
        aga_write(aga, reg, (uint32_t)(value >> 32), 4);
        aga_write(aga, reg + 4, (uint32_t)value, 4);
        aga_write(aga, reg + 8, (uint32_t)(value2 >> 32), 4);
        aga_write(aga, reg + 12, (uint32_t)value2, 4);
        aga_diag_inc(AGA_D_TRAP_W16);
        break;
    default: aga_diag_inc(AGA_D_TRAP_ODD_SIZE); break;
    }
#ifdef AGA_PROBES
    {
        uint64_t hold1;
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(hold1));
        uint64_t dt = hold1 - hold0;
        traphold_ticks += dt;
        if (dt > traphold_max) traphold_max = dt;
    }
#endif
    unlock();
    return 1;
}

int aga_trap_read(uint64_t far, int size, uint64_t *value, uint64_t *value2)
{
    if (!aga_enabled || far < 0xDFF000 || far >= 0xE00000) return 0;
    uint32_t reg = (uint32_t)far & 0x1FF;
    cpu_activity++;
    AGA_DIAG_INC(AGA_D_TRAPS_R);
    aga_diag_reg(reg, 0);
    if ((reg & 0x1FE) == 0x01E && aga_sandbox_active)
        aga_diag_intreqr_pc(emu68_get_pc());
    /* The beam registers bypass the mutex - see aga_peek_beam. They are 90% of
       all reads a game makes, and locking them saturated the machine. */
    if (aga_is_beam_reg(reg)) {
        uint64_t tpc = ticks_per_cck, base = beam_tick, add = 0;
        if (tpc) {
            uint64_t now;
            __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
            if (now > base && aga_beam_mode != 1) add = (now - base) / tpc;
            /* Never more than one pacing step ahead of the line the chipset core
               has reached. The estimate used to run up to $FFFF colour clocks -
               most of a frame - past it whenever the loop paused (presenting a
               frame takes ~0.9 ms), and a program racing the beam then drew into
               lines it believed were already on screen: Nexus 7's Gouraud scene
               showed a fixed horizontal cut with the old object below it, gone
               with the estimate off. Within a step the beam still moves, so an
               exact-position raster wait sees every value. Mode 2 = the old way. */
            if (aga_beam_mode == 2) { if (add > 0xFFFF) add = 0xFFFF; }
            else {
                if (add > BEAM_LEAD_CCK) add = BEAM_LEAD_CCK;
                /* v1.117: clamp by the SAME packed snapshot used to
                   calculate the beam value, in aga_peek_beam_at(). */
            }
        }
        *value = aga_peek_beam_at(aga, reg, size, (uint32_t)add, aga_beam_mode != 2);
        aga_diag_beam_value((uint16_t)*value);   /* the values a raster wait keeps seeing */
#ifdef AGA_PROBES
        /* A game that waits for one exact raster line does it in one of two
           ways: the 9-bit number as two byte reads ($DFF005 then $DFF006), or,
           when the line is below 256, the low byte alone in a tight loop.
           Worms: The Director's Cut uses the second, nearly two million times a
           second, and never gets out of it. Record what I hand that loop: the
           values, the gap between them, and which of the 256 low bytes I ever
           produced - if the line it wants is missing from that, the wait cannot
           end. */
        if (size == 1 && reg >= 0x005 && reg <= 0x007 && aga_sandbox_active) {
            static uint64_t pair_t0, prev_t;
            static uint32_t pair_line0, pair_open;
            static uint16_t pair_hi;
            uint64_t now2, f2;
            __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now2));
            __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f2));
            uint32_t gap = (f2 && prev_t && now2 > prev_t)
                         ? (uint32_t)(((now2 - prev_t) * 1000000u) / f2) : 0;
            prev_t = now2;
            {   /* the WHOLE line number, not just its low byte */
                uint32_t rv = (((uint32_t)aga_peek_beam(aga, VPOSR, 2) & 7) << 8)
                            | ((aga_peek_beam(aga, VHPOSR, 2) >> 8) & 0xFF);
                aga_diag_max(AGA_D_TRAP_VPOS_MAX, rv);
            }
            if (reg == 0x005) {
                aga_diag_inc(AGA_D_BEAM005_READS);
                aga_diag_set(AGA_D_BEAM005_OR,
                             aga_diag_ctr[AGA_D_BEAM005_OR] | (uint32_t)(*value & 0xFF));
                if (*value & 1) aga_diag_inc(AGA_D_BEAM005_SET);
            }
            aga_diag_max(AGA_D_TRAP_ADD_MAX, (uint32_t)add);
            aga_diag_set(AGA_D_CORE_LINES, (uint32_t)aga_lines_per_frame(aga));
            aga_diag_set(AGA_D_CORE_MAXHPOS, (uint32_t)aga_cycles_per_line(aga));
            if (reg == 0x006 || reg == 0x007) {
                /* $006 is the line, $007 the position along it: a wait can
                   test either, and mine only ever produces the positions the
                   interpolation lands on */
                aga_diag_beam_byte((uint8_t)*value, gap, reg == 0x007);
            } else {
                uint32_t both = aga_peek_beam_at(aga, VPOSR, 4, (uint32_t)add, aga_beam_mode != 2);
                pair_line0 = (((both >> 16) & 7) << 8) | ((both >> 8) & 0xFF);
                pair_t0 = now2; pair_hi = (uint16_t)*value; pair_open = 1;
            }
            if (reg == 0x006 && pair_open) {
                aga_diag_beam_line((uint16_t)(((pair_hi & 7) << 8) | (*value & 0xFF)), gap);
                uint32_t both = aga_peek_beam_at(aga, VPOSR, 4, (uint32_t)add, aga_beam_mode != 2);
                uint32_t line_now = (((both >> 16) & 7) << 8) | ((both >> 8) & 0xFF);
                aga_diag_inc(AGA_D_BEAM_PAIRS);
                if (line_now != pair_line0) aga_diag_inc(AGA_D_BEAM_PAIR_SKIPS);
                if (f2) aga_diag_max(AGA_D_BEAM_PAIR_US,
                                     (uint32_t)(((now2 - pair_t0) * 1000000u) / f2));
                pair_open = 0;
            }
            (void)pair_hi;
        }
#endif
        /* Deliberately NOT recorded in the trap ring. Beam reads are ~90% of all
           traps - 2.7 million a second - so recording them made a 256-entry ring
           a 0.1 ms window filled with noise. Everything else together runs at a
           few thousand a second, which makes the same ring about 100 ms of the
           machine actually doing things: what it programmed, and where it
           stopped. The rate itself is still counted in traps_read. */
        whd_trace_push(0, reg, size, (uint16_t)*value, emu68_get_pc());
        return 1;
    }
    lock();
    switch (size) {
    case 1: *value = aga_read(aga, reg, 1); break;
    case 2: *value = aga_read(aga, reg, 2); break;
    case 4: *value = aga_read(aga, reg, 4); break;
    case 8: *value = ((uint64_t)aga_read(aga, reg, 4) << 32) | aga_read(aga, reg + 4, 4); break;
    case 16:   /* the same 128-bit split as the write path */
        *value  = ((uint64_t)aga_read(aga, reg, 4) << 32)     | aga_read(aga, reg + 4, 4);
        *value2 = ((uint64_t)aga_read(aga, reg + 8, 4) << 32) | aga_read(aga, reg + 12, 4);
        aga_diag_inc(AGA_D_TRAP_R16);
        break;
    default: *value = 0; aga_diag_inc(AGA_D_TRAP_ODD_SIZE); break;
    }
    unlock();
    /* after the read, so the ring records what the 68k actually got back - the
       whole question with a busy-wait is what value it keeps seeing */
    last_rd = (reg << 16) | (uint16_t)*value;
    {
        uint32_t pc_now = emu68_get_pc();
        whd_trace_push(0, reg, size, (uint16_t)*value, pc_now);
        AGA_DIAG_TRAP(reg, (uint16_t)*value, 0, pc_now);
    }
    return 1;
}

/* ---- chipset loop (core 3) ------------------------------------------------ */
static inline uint64_t cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}
static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}

#define CHUNK_CCK 8      /* colour clocks per pacing step (~2.3 us) */

/* ---- libc shims for the core (Emu68 has no malloc/free) ------------------- */
extern void *tlsf;
extern void *tlsf_malloc(void *handle, uintptr_t size);
extern void  tlsf_free(void *handle, void *ptr);
extern void *memset(void *ptr, int fill, uintptr_t sz);

void *calloc(uintptr_t n, uintptr_t size)
{
    void *p = tlsf_malloc(tlsf, n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void free(void *p)
{
    if (p) tlsf_free(tlsf, p);
}

void aga_chipset_main(void)
{
  aga_evstream_on();                /* with aga.sleep, core 3's timed waits sleep (aga_glue.h) */
  for (;;) {
    core_idle = 1;
    /* WFE, not a yield spin. Once aga_enabled became volatile, a tight
       `while (!aga_enabled) yield;` re-reads this cache line forever, so
       core 3 generates continuous coherency traffic on a line core 0 also
       touches. On a Pi3B, whose WiFi is on SDIO, that competes with the
       network for the same memory system and showed up as hangs while
       connecting. sevl arms the first WFE so a flag set before I arrive is
       never missed; every writer of aga_enabled issues sev. */
    __asm__ volatile("sevl");
    while (!aga_enabled) {
        /* parked, but an AHI ring is on (aga-hdmi.audio): its sound goes
           to HDMI through the feed, which the chipset loop runs otherwise */
        if (aga_hdmi_ahi_live()) { aga_hdmi_pump(); continue; }
        __asm__ volatile("wfe");
    }
    core_idle = 0;
    aga_diag_log("[AGA] core 3: woke, entering the chipset loop");
    const uint64_t freq = cntfrq();
    /* PAL colour clock 3546895 Hz; NTSC 3579545 Hz. Fixed point 32.32 ticks per cck. */
    /* PAL 3546895, NTSC 3579545 - from the flag I was created with, NOT from
     * the register file.
     *
     * This used to ask aga_peek_reg(aga, BEAMCON0), and regs[] only ever holds
     * what the 68k has WRITTEN: aga_reset memsets it, and the machine's real
     * PAL/NTSC state lives in a->ntsc and a->beamcon0, not there. Almost no
     * game writes BEAMCON0, so the read returned 0, DISPLAYPAL was clear, and
     * the ternary picked NTSC - pacing a PAL machine, with PAL 227x313
     * geometry, at the NTSC colour clock. 3579545/3546895 is 1.0092, so
     * everything ran 0.92% fast.
     *
     * Two measurements already in docs/VALIDATION-FINDINGS.md match that to
     * better than 0.1%: 49.9203 x 1.0092 = 50.38 fps predicted against 50.4
     * sustained, and 436.9 audio streams/s predicted against 436.8 measured.
     *
     * It is also why the audio drifts. aga_audio_pull is asked for 27928
     * samples per second of EMULATED time, so at 0.92% fast the writer produces
     * 28185 a second while the real Paula, on the Amiga's own crystal, consumes
     * 27928 - an overrun of 257 samples every second. That is one ring restart
     * every 3.45 s, against the 3.55 s actually counted. Every correction in
     * aga_audio.c is compensating for this one line. */
    /* ...and from the standard in force since then: a game started NTSC
       ($5911 before the entry) or switched by BEAMCON0 runs at NTSC's colour
       clock - its 60 Hz frame, and the samples made from emulated time - and
       the loop re-reads it at every frame (paced_ntsc, where it presents). */
    int paced_ntsc = aga_start_ntsc;
    uint64_t ticks_per_cck_fp = (freq << 32) / (paced_ntsc ? 3579545u : 3546895u);
    /* The pacing deadline: whole timer ticks, plus a 32-bit fraction.
     *
     * NOT `cntvct() << 32`. cntvct() is a free-running 64-bit tick count, so
     * that shift throws away its top half and the time base keeps only the low
     * 32 bits - wrapping every 2^32 ticks. Every comparison against it then
     * becomes meaningless, and the counters showed it plainly: 6163 resyncs in
     * 5973 frames against a threshold of a whole frame, while the largest lag
     * ever recorded was 7380 colour clocks. A resync needs 71051 to fire. It
     * could not have fired once, and it fired more often than there were
     * frames.
     *
     * Every one of those spurious resyncs DISCARDS the time the loop owes, and
     * the audio is generated from this clock - so this is the last thing left
     * corrupting it.
     *
     * This was tried once before and broke the game, because the blitter's
     * completion delay was still counted in colour clocks: with the overflow
     * gone the loop started genuinely catching up, replaying thousands of
     * colour clocks in microseconds and expiring the delay before the game had
     * armed its wait. The glue now times the blit against the real clock
     * instead, so the two no longer interfere. */
    uint64_t next_ticks = cntvct();
    /* Where the frame's 20 ms go: the time spent waiting for the pacing
       clock is the loop's headroom, the rest is what the emulation costs.
       The Pi 3 fell a frame behind seven times a second (2026-09-13) and
       frame_us_worst, which only times the present, said nothing about
       why; these say it directly, on any board. */
    uint64_t frame_t0 = next_ticks, wait_acc = 0, audio_acc = 0;
    /* and where the busy time goes: the core itself per chunk, the once-a-
       frame statistics, and the whole loop body between two pacing waits -
       a body that is long while the parts are short is time taken from
       under me (an interrupt, the firmware). DAIF says whether interrupts
       can even reach this core. */
    uint64_t cyc_acc = 0, cyc_max = 0, stat_max = 0, body_max = 0, stat_t0 = 0;
    /* The A53's own account of where this core's time goes: L1 data refills
       (event 0x03) and L2 refills (0x17) on core 3, per frame. A frame that
       runs the same emulation six times slower while another core streams
       through the shared L2 should show it here, and which of the two says
       whether the remedy is my working set or the other core's traffic. */
#ifdef AGA_PROBES
    uint64_t pmu_l1_prev = 0, pmu_l2_prev = 0;
    {
        uint64_t v;
        __asm__ volatile("msr PMEVTYPER0_EL0, %0" :: "r"((uint64_t)0x03));
        __asm__ volatile("msr PMEVTYPER1_EL0, %0" :: "r"((uint64_t)0x17));
        __asm__ volatile("msr PMEVCNTR0_EL0, %0" :: "r"((uint64_t)0));
        __asm__ volatile("msr PMEVCNTR1_EL0, %0" :: "r"((uint64_t)0));
        __asm__ volatile("msr PMCNTENSET_EL0, %0" :: "r"((uint64_t)0x3));
        __asm__ volatile("mrs %0, PMCR_EL0" : "=r"(v));
        __asm__ volatile("msr PMCR_EL0, %0" :: "r"(v | 1));           /* E: counters on */
        __asm__ volatile("isb");
    }
    /* A memory-latency probe, once a frame: 512 dependent loads chasing a
       random cycle through 1 MB - twice the Pi 3's L2 - so it measures DRAM
       under whatever the other cores are doing to it. If it spikes in the
       frames where the emulation runs slow, the core is being starved of
       memory, not of anything of its own. */
    static uint32_t mem_probe[262144];
    static uint32_t cw_prev;
    {
        uint32_t n = 262144, seed = 12345;
        for (uint32_t i = 0; i < n; i++) mem_probe[i] = i;
        for (uint32_t i = n - 1; i > 0; i--) {          /* Sattolo: one cycle through all of it */
            seed = seed * 1103515245u + 12345u;
            uint32_t j = (seed >> 8) % i;
            uint32_t t = mem_probe[i]; mem_probe[i] = mem_probe[j]; mem_probe[j] = t;
        }
    }
#endif
    { uint64_t daif; __asm__ volatile("mrs %0, DAIF" : "=r"(daif)); aga_diag_set(AGA_D_CORE3_DAIF, (uint32_t)daif); }
    uint32_t next_frac  = 0;
    uint64_t chunk_at   = cntvct();   /* when the last pacing chunk actually ran */
    const uint64_t ticks_per_frame = freq / 50;
    /* How far behind before the owed time is dropped instead of caught up.
       One frame on a Pi 4. The Pi 3's frames spike to 31 ms with video on
       (2026-09-14) and every spike past 20 ms became a resync that dropped
       the time, starving the audio ring; two frames lets it catch up, which
       the loop does in a few milliseconds of real time. */
    const uint64_t resync_ticks = ticks_per_frame * (aga_pi3_mode() ? 2 : 1);
    /* Frame skip, Pi 3 only: ask for it once 2.5 ms behind, drop the request
       once caught up. 5 ms was the first value and let the loop build
       catch-up bursts big enough to be heard: the game's music routine runs
       in real time off the emulated vertical blank, so a burst bunches its
       note starts against the emulated Paula (Banshee, 2026-09-14). The Pi 4
       never falls that far behind. */
    const int skip_ok = aga_pi3_mode();
    int skip_want = 0;
    int last_real_ipl = 0;
    uint32_t blit_prev = 0;   /* accumulate the delta, so zeroing `blits` sticks */
    uint32_t pace_frames = 0;
    uint32_t cia_poll = 0;   /* throttle for the real-CIA mirror below */
    int audio_countdown = 0;
    /* ~9 microseconds: far more than the handful of instructions a game needs
       to arm its wait flag after starting a blit, and 0.05% of a frame, so
       nothing waiting on the blitter notices. Real time, not colour clocks. */
    const uint64_t blit_delay_ticks = freq / 110000;
    uint64_t blit_due = 0;

    kprintf("[AGA] chipset loop on core 3, timer %d Hz, %d ticks/cck\n", (int)freq, (int)(ticks_per_cck_fp >> 32));
    /* whole ticks per colour clock for the interpolating beam read. The Pi's
       26 MHz counter gives ~7 ticks per cck, so integer division is good to
       about a seventh of a colour clock - far finer than the 8 the loop steps. */
    ticks_per_cck = (ticks_per_cck_fp >> 32) ? (ticks_per_cck_fp >> 32) : 1;
    beam_tick = cntvct();

    /* Bring-up watchdog. The first time the chipset ran on real hardware,
       entering the sandbox hung the machine: agaboot never returned, so caffed
       died with the 68k and neither the counters nor the log ring could be read.
       Debugging a hang you cannot observe is not debugging.

       If the chipset is enabled but no frame completes within the timeout, give
       up and park: clear aga_enabled so this loop drops out, release the
       sandbox, and log it. That covers the likely case where core 0 is stuck in
       a trap waiting on a lock this core holds - dropping out releases it and
       the machine comes back with the counters intact and readable.

       It cannot help if THIS core is the stuck one. That needs a serial
       console; see docs/HARDWARE-BRINGUP.md. */
    uint64_t wd_deadline = cntvct() + freq * AGA_WATCHDOG_SEC;
    /* The stall check's state belongs to THIS session. It used to be two
       statics, so a new game started with the last one's count and the time
       of its last access - and core 3 gets here while the 68k is still inside
       the entry trap, before it can touch anything. The count had not moved,
       the "last seen" was however long ago the previous game ended, and the
       check fired on the first pass: the sandbox handed back within a frame,
       agaboot then read the REAL Denise ($FFFF), took the "not available"
       exit without telling graphics.library, and WHDLoad refused the game
       ("requires the AGA Chip Set"). Every game after the first one since a
       power-up, from 1.101 on (Virtual Karting, 2026-09-27). */
    uint32_t act_prev = cpu_activity;
    uint64_t act_seen = cntvct();

    while (aga_enabled) {
        /* Sandbox mode only. In machine mode the whole system runs on the
           virtual chipset from boot, so there is no real chipset to fall back
           to - parking would kill the machine outright rather than recover it.
           The watchdog exists to make a failed sandbox ENTRY survivable. */
        /* The other way a game dies: the chipset keeps running and the 68k
           stops. Frames complete, so the watchdog above never fires, and the
           machine sits there with a still picture and one note of music
           repeating - F10 does nothing, because nothing is executing. Hand
           the machine back instead of leaving a dead box: the desktop comes
           up and the counters can be read to find out why.
           Armed only once WHDLoad has taken the machine: before that it is
           AmigaOS running, which may well sit in fast RAM for seconds (and
           agaboot has not finished yet). Until then the baseline just moves
           along, so the ten seconds start at the takeover at the earliest. */
        if (aga_sandbox_active && aga_game_mode && game_taken) {
            uint32_t w = cpu_activity;
            uint64_t nowc = cntvct();
            if (w != act_prev) { act_prev = w; act_seen = nowc; }
            else if ((int64_t)(nowc - act_seen) > (int64_t)(freq * 10)) {
                aga_diag_set(AGA_D_CPU_STALL, 1);
                aga_diag_set(AGA_D_CPU_STALL_FRAME, (uint32_t)aga_frame_count(aga));
                aga_diag_set(AGA_D_STALL_SR, emu68_get_sr());
                aga_diag_set(AGA_D_STALL_INTF, emu68_get_intf());
                aga_diag_set(AGA_D_STALL_INSNS, emu68_get_insn_count());
                aga_diag_set(AGA_D_STALL_IRQCHK, emu68_get_irq_check());
                aga_diag_set(AGA_D_STALL_INTENA, aga_peek_reg(aga, 0x09A));
                aga_diag_set(AGA_D_STALL_INTREQ, aga_peek_reg(aga, 0x09C));
                aga_diag_set(AGA_D_STALL_IPL, (uint32_t)aga_get_ipl(aga));
                aga_diag_freeze_here();   /* before the desktop overwrites it */
                aga_diag_log("[AGA] the 68k stopped touching the chipset for ten"
                             " seconds: handing the machine back");
                aga_enabled = 0;
                __asm__ volatile("sev");
                aga_sandbox_active = 0;
                sandbox_restore();
                aga_video_hide();
                break;
            }
        } else {
            act_prev = cpu_activity;
            act_seen = cntvct();
        }
        if (aga_sandbox_active && (int64_t)(cntvct() - wd_deadline) > 0) {
            aga_diag_log("[AGA] watchdog: no frame completed, parking the chipset");
            kprintf("[AGA] watchdog: no frame in %d s, parking the chipset\n",
                    AGA_WATCHDOG_SEC);
            aga_enabled = 0;
            __asm__ volatile("sev");   /* wake core 3 */
            aga_sandbox_active = 0;
            sandbox_restore();         /* NOT optional - see the note on it */
            aga_video_hide();          /* same reason as in aga_sandbox_leave */
            break;
        }
        {
            uint64_t inc = ticks_per_cck_fp * CHUNK_CCK;   /* 32.32 ticks */
            uint32_t f = next_frac + (uint32_t)inc;
            next_ticks += (inc >> 32) + (f < next_frac ? 1u : 0u);
            next_frac = f;

            /* Catch up at no more than twice real time.
             *
             * Being right ON AVERAGE is not enough. Presenting a frame blocks
             * this loop for about 900us, and an uncapped catch-up then replays
             * those 900us of emulated time in a few microseconds. The average
             * rate is perfect - loop_resyncs sits at 0 - but the instantaneous
             * rate is a stall followed by a burst, and everything the chipset
             * drives comes out in that shape. A music player clocked by audio
             * or timer interrupts gets its tempo ticks bunched, so several
             * notes fire at once and then nothing: "you hear drums all at once,
             * so lag", with effects appearing only here and there.
             *
             * Requiring each chunk to occupy at least half its real duration
             * caps the loop at 2x, so a 900us present is repaid smoothly over
             * the next 900us instead of instantly - well inside one frame, and
             * the interrupts stay evenly spaced. */
            uint64_t floor_t = chunk_at + ((inc >> 32) >> 1);
            uint64_t target = next_ticks > floor_t ? next_ticks : floor_t;
            {
                uint64_t w0 = cntvct();
                if (chunk_at && w0 > chunk_at && w0 - chunk_at > body_max) body_max = w0 - chunk_at;
                while (cntvct() < target)
                    aga_nap_before(target); /* with aga.sleep: asleep while far ahead; a step's wait spins */
                chunk_at = cntvct();
                if (chunk_at > w0) wait_acc += chunk_at - w0;
            }
        }
        /* aga.loop_stop=N: a ladder, one rung per boot. Each level adds one
           thing the loop does, so a level that boots and a level that dies
           name the culprit between them.

             1  pace only - keep time, count frames, call nothing
             2  + the bus mutex: lock()/unlock() around nothing
             3  + aga_run_cycles: the chipset core itself, no video, no audio
             4  + the frame present path
             5  + aga_audio_pull only - the audio maths, but nothing streamed
             0  everything, audio included (the normal build)

           MEASURED 2026-09-07 on a Pi 4 in sandbox mode (see
           docs/VALIDATION-FINDINGS.md): rung 3 and rung 4 both run at 50.4-50.5
           frames per second, sustained, presents tracking frames exactly,
           video_errors 0, machine reachable throughout. Rung 0 dies. So
           aga_run_cycles and aga_video_present are BOTH innocent, and the fault
           is in the audio block - which is what rung 5 now splits in half.

           Rungs 1 and 2 prove nothing, and cannot. Sandbox entry installs the
           traps whatever this setting is, so at those rungs the 68k talks to a
           chipset that never ticks: no vblank, no interrupts, AmigaOS stalls.
           A hang there is the expected behaviour of correct code. They are kept
           only to check that the loop skeleton and the wake handshake run at
           all, and they now let the watchdog fire so the machine recovers and
           the counters can be read. */
        if (aga_loop_stop == 1 || aga_loop_stop == 2) {
            if (aga_loop_stop == 2) { lock(); unlock(); }
            if (++pace_frames >= 313 * 227 / CHUNK_CCK) {   /* one PAL frame */
                pace_frames = 0;
                aga_diag_inc(AGA_D_FRAMES);
                /* deliberately NOT rearming the watchdog here. Rungs 1 and 2
                   leave the chipset frozen - the traps are installed but
                   nothing ticks, so the 68k waits forever for a vblank and
                   AmigaOS stalls. That is expected, not a bug, and it means a
                   hang at these rungs proves nothing. Letting the watchdog
                   fire parks the chipset, releases the machine and leaves the
                   counters readable, which is the whole point. */
            }
            continue;
        }
        int native = 0;
#ifdef FRF_VIRTUAL_AGA
        {
            extern void frf_vcia_service(void);
            static uint32_t frf_cia_div;
            if ((++frf_cia_div & 31u) == 0u)
                frf_vcia_service();
        }
#endif
        lock();
        if (aga_loct_fix_pending && !(aga_custom_rget(aga, DMACONR) & 0x0080)) {
            /* the copper is off: the OS list is history and the game has not
               started its own yet - the one moment LOCT can be cleared safely */
            uint16_t b3 = aga_peek_reg(aga, 0x106);
            aga_loct_fix_pending = 0;
            if (b3 & 0x0200) {
                aga_custom_wput(aga, 0x106, b3 & ~0x0200);
                aga_diag_log("[AGA] takeover: BPLCON3 had LOCT set (the OS's AGA copper list), cleared");
            } else {
                aga_diag_log("[AGA] takeover: BPLCON3 LOCT already clear");
            }
        }
        if (aga_ecs_forget_pending && !(aga_custom_rget(aga, DMACONR) & 0x0080)) {
            /* The same moment: the AGA signs an ECS Denise saw until now were
               the OS's - WHDLoad's splash, drawn by a graphics.library that
               believes the machine is AGA - not the game's, so ecs_says_aga
               judges the game alone. If the OS's screen already made it AGA,
               it is an ECS Denise again. Never in "always ECS" (nothing is
               judged) or in a session entered as AGA. */
            unsigned sg = aga_ecs_signs(aga);
            aga_ecs_forget_pending = 0;
            if (ecs_entry == 1 && !(sg & 0x80)) {
                if (!aga_get_ecs(aga)) {
                    aga_set_ecs(aga, 1);
                    ecs_seen = 1;
                    ecs_signs_logged = 0;
                    aga_diag_log("[AGA] takeover: the OS's screen had made the chipset AGA - an ECS Denise again, the game is judged alone");
                } else if (aga_ecs_forget_signs(aga)) {
                    ecs_signs_logged = 0;
                    aga_diag_log("[AGA] takeover: the OS's AGA signs forgotten, the game is judged alone");
                }
            }
        }
#ifdef AGA_PROBES
        {
            uint64_t c0 = cntvct();
            aga_run_cycles(aga, CHUNK_CCK);
            uint64_t dc = cntvct() - c0;
            cyc_acc += dc; if (dc > cyc_max) cyc_max = dc;
        }
#else
        aga_run_cycles(aga, CHUNK_CCK);
#endif
        /* Report an outstanding blit finished once enough WALL time has passed.
           The core no longer counts this down in colour clocks: those only mean
           anything while the loop runs them at real-time speed, and it does
           not - so a catch-up burst used to expire the delay before the game
           had armed the wait it was about to sleep on. Timing it here decouples
           the blitter from the pacing entirely. */
        /* HOW long: at least as long as the real blitter would take. The data is
           done at once either way; what the length decides is when the program
           hears about it. Synergy runs its chunky-to-planar as a chain of blits
           driven by the blitter interrupt, and its handler starts the next blit
           BEFORE it acknowledges the last:
               jsr   next_pass              ; ... writes BLTSIZH
               move.w #$4040,$dff09c        ; then clears BLIT
           Safe on an Amiga, where a pass takes the blitter milliseconds. With
           a fixed 9 us, a level 6 music interrupt landing between the two was
           enough for the new blit's BLIT to be raised and then wiped by the
           old acknowledge; the chain stopped, the main loop waited for ever
           for its done flag, and the light effect, the whirlwind and the
           tunnel never appeared while the music played on. */
        if (aga_blit_pending(aga)) {
            uint64_t now = cntvct();
            if (!blit_due) {
                /* aga_blit_fast (agaboot BLITTIMING FAST): 1.101's fixed delay, for A/B tests */
                uint64_t real = aga_blit_fast ? 0 : ((uint64_t)aga_blit_duration(aga) * freq) / 3546895u;
                blit_due = now + (real > blit_delay_ticks ? real : blit_delay_ticks);
            }
            else if (now >= blit_due) { aga_blit_complete(aga); blit_due = 0; }
        } else {
            blit_due = 0;
        }
        /* publish the beam base for the lock-free interpolating read. Order
           matters: the timestamp goes last, so a reader that sees a fresh tick
           is guaranteed to be looking at the position that goes with it. */
        __asm__ volatile("dmb ish" ::: "memory");
        beam_tick = cntvct();
        /* A CIA interrupt has to be able to WAKE the game, not just be visible
         * once it asks.
         *
         * In the sandbox the CIAs stay real: the real Paula latches EXTER
         * (level 6) and PORTS (level 2), and an INTREQR read folds those bits
         * in (vectors.c) so a running handler sees them. A game that goes to
         * sleep on them never reads anything. Worms: The Director's Cut sits
         * on STOP #$2300 at its menu - supervisor, interrupts masked to 3 -
         * waiting for the CIA-B timer that clocks its music. My IPL is
         * computed from the VIRTUAL INTREQ alone, which knows nothing about
         * the CIAs, so the wake-up never came: the picture stood still, one
         * note of music repeated, and nothing in the machine moved again.
         *
         * Mirror the real pending bits into the virtual INTREQ so they raise
         * the IPL like any other source. Only the ones the game has enabled
         * itself - handing a game an EXTER it never asked for makes its
         * handler acknowledge a source that is not its own - and only in game
         * mode. The clear path already exists: an INTREQ write that clears
         * PORTS or EXTER is forwarded to the real Paula (custom.c), so the
         * handler's own acknowledgement stops this re-raising it. */
        /* Nudge a sleeping 68k that has an interrupt waiting for it. Emu68's
           STOP parks on WFE and only looks again when an event arrives, so
           one missed event is a machine that never moves again. An SEV
           costs nothing and any core that is not waiting ignores it. */
        if (aga_sandbox_active && (cia_poll & 255) == 128 && aga_get_ipl(aga) > 0)
            __asm__ volatile("dsb ish; sev" ::: "memory");
        if (aga_sandbox_active && frame_pending) {
            /* While the sandbox is up the real chipset's only business is the
               two CIAs - PORTS (keyboard) and EXTER (timers) - and the master
               enable. Everything else is emulated; the real Paula's audio is
               where MY mix plays, and its buffers wrap all the time. Under
               WHDLoad's own Kickstart (a KickEmu slave: Synergy) the real
               INTENA read $7FFF - everything on - so every one of my wraps
               raised a real level 4, the program's handler found nothing in
               the emulated registers to clear, and the real line pulled it
               straight back in: 1.7 million level-4 interrupts in 25 seconds,
               not one fucking frame on screen, then a reset. Put the real
               enables back to the CIA sources whenever anything else turns
               up. */
            uint16_t ri = (uint16_t)ps_read_16(0xDFF01C);
            /* AUDIO ONLY. The first version cleared every non-CIA enable, and
               when WHDLoad crashed back to a desktop before its cleanup could
               leave the sandbox, that took VBlank, blitter and disk away from
               the restored OS - Worms ended in a Software Failure. The storm
               came from audio alone, and the real Paula's audio is mine. */
            uint16_t stray = (uint16_t)(ri & 0x0780);
            if (stray) {
                ps_write_16(0xDFF09A, stray);        /* clear those enables */
                ps_write_16(0xDFF09C, stray);        /* and what they latched */
                if (!aga_diag_ctr[AGA_D_REAL_INTENA_FIX])
                    aga_diag_log("[AGA] the real Paula had its audio interrupts enabled: switched off");
                aga_diag_inc(AGA_D_REAL_INTENA_FIX);
                aga_diag_set(AGA_D_REAL_INTENA_STRAY, stray);
            }
        }
        if (aga_sandbox_active && frame_pending) {
            /* The mouse quit. Both buttons for two seconds - or the left one alone
               for six, because the right one is read through POTGO and a program
               that reconfigures that port makes it unreadable, while the left is
               a CIA pin nobody can take away. Stage one hands WHDLoad an F10 (see
               aga_cia_read); if the machine is still in the sandbox three seconds
               later, the program is not taking interrupts at all, and stage two
               is a level 7 - it cannot be masked, and it lands in WHDLoad's own
               vector table, which reports it and offers the way out. */
            uint8_t pra = (uint8_t)ps_read_8(0xBFE001);
            uint16_t pot = (uint16_t)ps_read_16(0xDFF016);
            aga_hdmi_led(!(pra & 0x02));         /* /LED low: lit, the LED filter on */
            int lmb = !(pra & 0x40), rmb = !(pot & 0x0400);
            mq_held2 = (lmb && rmb) ? mq_held2 + 1 : 0;
            mq_held1 = lmb ? mq_held1 + 1 : 0;
            uint64_t nowm = cntvct();
            if (!mq_stage && (mq_held2 == 100 || mq_held1 == 300)) {
                mq_stage = 1;
                mq_stage_at = nowm;
                aga_quit_until = nowm + freq;
                aga_diag_inc(AGA_D_MOUSE_QUIT);
                aga_diag_log(mq_held2 >= 100 ? "[AGA] mouse quit (both buttons): telling WHDLoad F10"
                                          : "[AGA] mouse quit (left button): telling WHDLoad F10");
            } else if (mq_stage == 1 && (int64_t)(nowm - mq_stage_at) > (int64_t)(freq * 3)) {
                mq_stage = 2;
                emu68_raise_nmi();
                aga_diag_log("[AGA] mouse quit: still in the sandbox, raising an NMI");
            }
        } else if (!aga_sandbox_active) {
            /* a fresh game gets a fresh mouse quit */
            mq_held2 = mq_held1 = mq_stage = 0;
        }
        if (aga_game_mode && aga_sandbox_active && (++cia_poll & 63) == 0) {
            /* ONLY once the machine has gone quiet. A running game already gets
               its CIA interrupts: the real Paula raises the real IPL and the
               68k takes them through the normal PiStorm path, and an INTREQR
               read folds the bits in for the handler. Mirroring them all the
               time instead means re-raising a source whose real latch is still
               set, over and over, which buries a loader in level 6 - the very
               storm the takeover silencing exists to prevent. A CPU asleep on
               STOP makes no accesses at all, so half a second of silence is
               the signal, and it is one no working game ever gives. */
            static uint32_t q_seen;
            static uint64_t q_since, q_last;
            /* Half a second to decide this game sleeps at all; once it has
               woken from one, a fortieth is enough, so a game that sleeps
               between every tick of its music keeps its tempo. A loader is
               never quiet for that long with nothing to acknowledge, and the
               spacing below caps the rate either way. */
            static int q_proven;
            uint32_t act = aga_diag_ctr[AGA_D_TRAPS_W] + aga_diag_ctr[AGA_D_TRAPS_R]
                         + aga_diag_ctr[AGA_D_CHIP_W] + aga_diag_ctr[AGA_D_CHIP_R];
            uint64_t nowq = cntvct();
            if (act != q_seen) { q_seen = act; q_since = nowq; }
            else if (q_since
                     && (int64_t)(nowq - q_since) > (int64_t)(q_proven ? freq / 40 : freq / 2)
                     && (int64_t)(nowq - q_last) > (int64_t)(freq / 50)) {
                uint16_t want = (uint16_t)(aga_cia_mirror >= 2 ? (INTF_EXTER | INTF_PORTS)
                                         : aga_cia_mirror == 1 ? INTF_PORTS : 0);
                uint16_t en = aga_custom_rget(aga, INTENAR) & want;
                if (en) {
                    uint16_t real = (uint16_t)ps_read_16(0xDFF01E) & en;
                    uint16_t have = aga_custom_rget(aga, INTREQR) & en;
                    if (real & ~have) {
                        /* the loop already holds the chipset lock here - taking
                           it again wedges this core, and with it the frames,
                           the watchdog and the game's only way out */
                        aga_custom_wput(aga, INTREQ, (uint16_t)(0x8000 | (real & ~have)));
                        aga_diag_inc(AGA_D_CIA_WAKE);
                        q_last = nowq;
                        q_proven = 1;
                    }
                }
            }
        }
#ifdef AGA_PROBES
        if (frame_pending) {   /* one line of the 68k's life, per frame */
            static uint32_t tp, cw, i3, i6;
            uint32_t t = aga_diag_ctr[AGA_D_TRAPS_W] + aga_diag_ctr[AGA_D_TRAPS_R];
            uint32_t c = aga_diag_ctr[AGA_D_CHIP_W];
            uint32_t a3 = aga_diag_ctr[AGA_D_IRQ_L3], a6 = aga_diag_ctr[AGA_D_IRQ_L6];
            {   /* instructions retired this frame, in the traps column when
                   nothing else is moving: it says whether the CPU is asleep
                   or spinning */
                static uint32_t lt;
                uint32_t lk = emu68_get_loop_ticks();
                aga_diag_frame_trace(emu68_get_sr(), t - tp, c - cw, a3 - i3, a6 - i6,
                                     lk - lt, emu68_get_pc());
                lt = lk;
            }
            tp = t; cw = c; i3 = a3; i6 = a6;
        }
        {   /* one line of heartbeat per chunk: a stall leaves the last one */
            uint32_t bv = aga_peek_beam(aga, VHPOSR, 2);
            aga_diag_inc(AGA_D_CHUNK_SEQ);
            aga_diag_set(AGA_D_CHUNK_VPOS,
                         (((uint32_t)aga_peek_beam(aga, VPOSR, 2) & 7) << 8) | ((bv >> 8) & 0xFF));
            aga_diag_set(AGA_D_CHUNK_HPOS, bv & 0xFF);
        }
#endif
#ifdef AGA_PROBES
        /* the highest line this core actually stepped the chipset to */
        aga_diag_max(AGA_D_LOOP_VPOS_MAX,
                     (((uint32_t)aga_peek_beam(aga, VPOSR, 2) & 7) << 8)
                     | ((aga_peek_beam(aga, VHPOSR, 2) >> 8) & 0xFF));
        if (!aga_diag_is_frozen())
        {   /* where the display is reading from, and whether anything is
               there: 256 words at the first bitplane's pointer. Frozen with
               everything else, or quitting the game overwrites the evidence
               with WHDLoad's teardown: no planes and a one-line window. */
            static uint32_t bpl1_prev;
            uint32_t p1 = aga_peek_bplpt(aga, 0);
            aga_diag_set(AGA_D_BPL1PT_NOW, p1);
            if (p1 != bpl1_prev) { aga_diag_inc(AGA_D_BPL1PT_CHANGES); bpl1_prev = p1; }
            aga_diag_max(AGA_D_BPL1PT_MAX, p1);
            if (!aga_diag_ctr[AGA_D_BPL1PT_MIN] || p1 < aga_diag_ctr[AGA_D_BPL1PT_MIN])
                aga_diag_set(AGA_D_BPL1PT_MIN, p1);
            if (chip_va && chip_size >= 512 && p1 < chip_size - 512) {
                const uint16_t *w = (const uint16_t *)(chip_va + p1);
                uint32_t nz = 0;
                for (int i = 0; i < 256; i++) if (w[i]) nz++;
                aga_diag_set(AGA_D_BPL1_NONZERO, nz);
            }
            aga_diag_set(AGA_D_DIWSTRT_NOW, aga_peek_reg(aga, 0x08E));
            aga_diag_set(AGA_D_DIWSTOP_NOW, aga_peek_reg(aga, 0x090));
            aga_diag_set(AGA_D_DDFSTRT_NOW, aga_peek_reg(aga, 0x092));
            aga_diag_set(AGA_D_DDFSTOP_NOW, aga_peek_reg(aga, 0x094));
            aga_diag_set(AGA_D_BPLCON0_NOW, aga_peek_reg(aga, 0x100));
            for (int i = 0; i < 21; i++) aga_diag_set(AGA_D_COPRS_A0 + i, aga_peek_copdbg(aga, i));
            for (int i = 0; i < 97; i++) aga_diag_set(AGA_D_EVLOG_N + i, aga_peek_evlog(aga, i));
            for (int i = 0; i < 5; i++)
                aga_diag_set(AGA_D_AUDIRQ_DATIDLE + i, aga_peek_audirq(aga, i));
            for (int i = 0; i < 4; i++)
                aga_diag_set(AGA_D_BLIT_FIN + i, aga_peek_blit_irq(aga, i));
            for (int i = 0; i < 8; i++) {
                aga_diag_set(AGA_D_DMALOG0 + i, aga_peek_dmacon_log(aga, i));
                aga_diag_set(AGA_D_BC0LOG0 + i, aga_peek_beamcon0_log(aga, i));
            }
            {   /* per frame, not since boot: the picture is about this frame */
                static uint32_t fprev[6];
                static uint32_t fframe;
                uint32_t fr = (uint32_t)aga_frame_count(aga);
                if (fr != fframe) {
                    fframe = fr;
                    for (int i = 0; i < 6; i++) {
                        uint32_t v = aga_peek_fetch_stat(aga, i);
                        aga_diag_set(AGA_D_FETCH_EVAL + i, v - fprev[i]);
                        fprev[i] = v;
                    }
                }
            }
        }
        if (!aga_diag_is_frozen())
        {   /* bitplane DMA on or off, and the frame it last changed in */
            static uint16_t dma_prev;
            uint16_t dm = aga_peek_dmacon(aga);
            aga_diag_set(AGA_D_DMACON_NOW, dm);
            if ((dm ^ dma_prev) & 0x0100) {
                aga_diag_inc(AGA_D_BPLEN_CHANGES);
                aga_diag_set((dm & 0x0100) ? AGA_D_BPLEN_ON_FRAME : AGA_D_BPLEN_OFF_FRAME,
                             (uint32_t)aga_frame_count(aga));
            }
            dma_prev = dm;
        }
        if (!aga_diag_is_frozen())
        {   /* PAL is 313 lines, NTSC 263: a game that sets one and gets the
               other waits for lines that never come round */
            static int geom_prev;
            int lpf = aga_lines_per_frame(aga);
            if (lpf != geom_prev) {
                if (geom_prev) {
                    aga_diag_inc(AGA_D_GEOM_CHANGES);
                    aga_diag_set(AGA_D_GEOM_FROM, (uint32_t)geom_prev);
                    aga_diag_set(AGA_D_GEOM_TO, (uint32_t)lpf);
                    aga_diag_set(AGA_D_GEOM_FRAME, (uint32_t)aga_frame_count(aga));
                    aga_diag_set(AGA_D_GEOM_BEAMCON0, aga_peek_reg(aga, 0x1DC));
                    aga_diag_set(AGA_D_GEOM_SANDBOX, (uint32_t)aga_sandbox_active);
                }
                geom_prev = lpf;
            }
            aga_diag_max(AGA_D_GEOM_MAX, (uint32_t)lpf);
            if (!aga_diag_ctr[AGA_D_GEOM_MIN] || (uint32_t)lpf < aga_diag_ctr[AGA_D_GEOM_MIN])
                aga_diag_set(AGA_D_GEOM_MIN, (uint32_t)lpf);
        }
#endif
        int wmode = 0, wx = 0, wy = 0, ww = 0, wh = 0;
        uint32_t program_cop1lc = 0;
        int program_display_lines = 0;
        if (frame_pending) {
            native = aga_native_active(aga);
            wmode = aga_window_mode(aga, &wx, &wy, &ww, &wh);
            if (aga_program_mode) {
                program_cop1lc = ((((uint32_t)aga_peek_reg(aga, 0x080) & 0x01ffu) << 16) |
                                  ((uint32_t)aga_peek_reg(aga, 0x082) & 0xfffeu));
                program_display_lines = aga_last_frame_display_lines(aga);
            }
        }
        /* CIA interrupts are NOT mirrored here any more. This used to compare
           emu68_get_real_ipl() against 2 and 6, but on the classic PiStorm
           that byte is a 0/1 "IPL line active" flag, never the level, so it
           never matched and the virtual INTREQ never showed PORTS/EXTER. The
           real exception path delivers those levels; vectors.c folds the real
           INTREQR bits into INTREQR reads so the OS handler acknowledges the
           CIA. */
        (void)last_real_ipl;
        unlock();
        if (frame_pending && aga_loop_stop == 3) {
            /* chipset only: a frame completed, but nothing is presented. */
            frame_pending = 0;
            aga_diag_inc(AGA_D_FRAMES);
            {   /* the frame's time budget: busy = everything but the pacing wait */
                uint64_t fn = cntvct(), ft = fn - frame_t0;
                uint64_t busy = ft > wait_acc ? ft - wait_acc : 0;
                uint32_t bus_us = (uint32_t)((busy * 1000000u) / freq);
                aga_diag_set(AGA_D_LOOP_BUSY_US, bus_us);
                aga_diag_max(AGA_D_LOOP_BUSY_WORST, bus_us);
                aga_diag_set(AGA_D_LOOP_WAIT_US, (uint32_t)((wait_acc * 1000000u) / freq));
                aga_diag_set(AGA_D_AUDIO_US_FRAME, (uint32_t)((audio_acc * 1000000u) / freq));
                aga_diag_set(AGA_D_LOCK_US_FRAME, (uint32_t)((lock_acc3 * 1000000u) / freq));
                aga_diag_max(AGA_D_LOCK_US_WORST, (uint32_t)((lock_max3 * 1000000u) / freq));
                lock_acc3 = 0;
                aga_diag_set(AGA_D_CYC_US_FRAME, (uint32_t)((cyc_acc * 1000000u) / freq));
                aga_diag_max(AGA_D_CYC_US_WORST, (uint32_t)((cyc_max * 1000000u) / freq));
                aga_diag_max(AGA_D_BODY_US_WORST, (uint32_t)((body_max * 1000000u) / freq));
                aga_diag_max(AGA_D_CYC_FRAME_WORST, (uint32_t)((cyc_acc * 1000000u) / freq));
#ifdef AGA_PROBES
                { uint64_t l1, l2;
                  __asm__ volatile("mrs %0, PMEVCNTR0_EL0" : "=r"(l1));
                  __asm__ volatile("mrs %0, PMEVCNTR1_EL0" : "=r"(l2));
                  uint32_t d1 = (uint32_t)(l1 - pmu_l1_prev), d2 = (uint32_t)(l2 - pmu_l2_prev);
                  pmu_l1_prev = l1; pmu_l2_prev = l2;
                  aga_diag_set(AGA_D_L1_REFILL_FRAME, d1); aga_diag_max(AGA_D_L1_REFILL_WORST, d1);
                  aga_diag_set(AGA_D_L2_REFILL_FRAME, d2); aga_diag_max(AGA_D_L2_REFILL_WORST, d2); }
                { static uint64_t ab_prev, ac_prev;
                  uint64_t ab = aga_abort_ticks, ac = aga_abort_count;
                  uint32_t aus = (uint32_t)(((ab - ab_prev) * 1000000u) / freq);
                  aga_diag_set(AGA_D_ABORT_US_FRAME, aus); aga_diag_max(AGA_D_ABORT_US_WORST, aus);
                  aga_diag_set(AGA_D_ABORTS_FRAME, (uint32_t)(ac - ac_prev)); aga_diag_max(AGA_D_ABORTS_WORST, (uint32_t)(ac - ac_prev));
                  ab_prev = ab; ac_prev = ac; }
                { uint32_t cw = aga_diag_ctr[AGA_D_CHIP_W];
                  aga_diag_set(AGA_D_CHIPW_FRAME, cw - cw_prev);
                  aga_diag_max(AGA_D_CHIPW_FRAME_WORST, cw - cw_prev); cw_prev = cw; }
                { uint64_t m0 = cntvct(); uint32_t p = (uint32_t)(m0 & 0x3FFFF);
                  for (int i = 0; i < 512; i++) p = mem_probe[p];      /* ~80 us on a Pi 3, less on a Pi 4 */
                  __asm__ volatile("" :: "r"(p));
                  uint32_t mus = (uint32_t)(((cntvct() - m0) * 1000000u) / freq);
                  aga_diag_set(AGA_D_MEM_US, mus); aga_diag_max(AGA_D_MEM_US_WORST, mus); }
#endif
                cyc_acc = 0; cyc_max = 0; body_max = 0;
                frame_t0 = fn; wait_acc = 0; audio_acc = 0;
            }
            wd_deadline = cntvct() + freq * AGA_WATCHDOG_SEC;
        } else if (frame_pending) {
            frame_pending = 0;
            if (AGA_PROBES_ON && aga_sandbox_active) {
                /* three bus reads per frame: what the real Paula thinks */
                /* timed: on a Pi 3 these three bus reads are the last thing
                   core 3 does on the PiStorm bus in rung 5, and the bus is
                   the suspect for the 12 ms stalls (2026-09-14) */
                uint64_t b0 = cntvct();
                aga_diag_set(AGA_D_REAL_INTENA, ps_read_16(0xDFF01C));
                aga_diag_set(AGA_D_REAL_INTREQ, ps_read_16(0xDFF01E));
                aga_diag_set(AGA_D_REAL_DMACON, ps_read_16(0xDFF002));
                uint32_t bus_us = (uint32_t)(((cntvct() - b0) * 1000000u) / freq);
                aga_diag_set(AGA_D_BUS_US_FRAME, bus_us);
                aga_diag_max(AGA_D_BUS_US_WORST, bus_us);
                {   /* the trap path's bus writes since the last frame (core 0 adds, I read) */
                    uint64_t tb = trapbus_ticks; trapbus_ticks = 0;
                    uint32_t tn = trapbus_n; trapbus_n = 0;
                    aga_diag_set(AGA_D_TRAPBUS_US_FRAME, (uint32_t)((tb * 1000000u) / freq));
                    aga_diag_set(AGA_D_TRAPBUS_N_FRAME, tn);
                    aga_diag_max(AGA_D_TRAPBUS_US_WORST, (uint32_t)((trapbus_max * 1000000u) / freq));
                    uint64_t th = traphold_ticks; traphold_ticks = 0;
                    aga_diag_set(AGA_D_TRAPHOLD_US_FRAME, (uint32_t)((th * 1000000u) / freq));
                    aga_diag_max(AGA_D_TRAPHOLD_US_WORST, (uint32_t)((traphold_max * 1000000u) / freq));
                }
                {   /* the bus mutex, per core: this frame's share added to the session totals */
                    extern volatile uint64_t ps_bus_wait_ticks[4], ps_bus_hold_ticks[4], ps_bus_wait_max[4];
                    extern volatile uint32_t ps_bus_count[4];
                    static uint64_t w_prev[4], h_prev[4];
                    static uint32_t n_prev[4];
                    for (int c = 0; c < 4; c++) {
                        uint64_t w = ps_bus_wait_ticks[c], h = ps_bus_hold_ticks[c];
                        uint32_t n = ps_bus_count[c];
                        aga_diag_ctr[AGA_D_BUSW_US_C0 + c] += (uint32_t)(((w - w_prev[c]) * 1000000u) / freq);
                        aga_diag_ctr[AGA_D_BUSH_US_C0 + c] += (uint32_t)(((h - h_prev[c]) * 1000000u) / freq);
                        aga_diag_ctr[AGA_D_BUSN_C0 + c] += n - n_prev[c];
                        w_prev[c] = w; h_prev[c] = h; n_prev[c] = n;
                    }
                    aga_diag_max(AGA_D_BUSW_WORST_US_C0, (uint32_t)((ps_bus_wait_max[0] * 1000000u) / freq));
                    aga_diag_max(AGA_D_BUSW_WORST_US_C2, (uint32_t)((ps_bus_wait_max[2] * 1000000u) / freq));
                    aga_diag_max(AGA_D_BUSW_WORST_US_C3, (uint32_t)((ps_bus_wait_max[3] * 1000000u) / freq));
                    static uint64_t ab_prev, ac_prev;
                    uint64_t ab = aga_abort_ticks, ac = aga_abort_count;
                    aga_diag_ctr[AGA_D_ABORT_US_TOTAL] += (uint32_t)(((ab - ab_prev) * 1000000u) / freq);
                    aga_diag_ctr[AGA_D_ABORTS_TOTAL] += (uint32_t)(ac - ac_prev);
                    ab_prev = ab; ac_prev = ac;
                }
            }
            uint64_t t0 = cntvct();
            aga_video_set_window(wmode, wx, wy, ww, wh);
            {   /* what the core made of the frame - woven, PAL or NTSC - and
                   the pacing clock for the standard from now on */
                int row = 0, woven = aga_frame_woven(aga, &row), ntsc = aga_is_ntsc(aga);
                aga_video_set_frame(woven, row, ntsc);
                int ax0, ax1, ay0, ay1;            /* and its display window, for Size Auto */
                int aok = aga_last_frame_window(aga, &ax0, &ax1, &ay0, &ay1);
                aga_video_auto_window(aok, ax0, ax1, ay0, ay1);
                if (ntsc != paced_ntsc) {
                    paced_ntsc = ntsc;
                    ticks_per_cck_fp = (freq << 32) / (ntsc ? 3579545u : 3546895u);
                    ticks_per_cck = (ticks_per_cck_fp >> 32) ? (ticks_per_cck_fp >> 32) : 1;
                    aga_diag_log(ntsc ? "[AGA] pacing: NTSC colour clock" : "[AGA] pacing: PAL colour clock");
                }
            }
            /* FRF V1.118 Copper handoff. P96/RTG ownership affects HDMI only;
               physical HAM6 waits for the RUN child to install a useful Copper. */
            if (aga_program_mode && !aga_program_rgb_released) {
                if (!aga_program_seed_cop1lc ||
                    program_cop1lc < 0x400u ||
                    program_cop1lc == aga_program_seed_cop1lc ||
                    program_display_lines < 16) {
                    aga_program_candidate_cop1lc = 0;
                    aga_program_candidate_frames = 0;
                } else {
                    aga_program_candidate_cop1lc = program_cop1lc;
                    if (++aga_program_candidate_frames >= 2) {
                        aga_rgbout_start(chip_real);
                        aga_program_rgb_released = 1;
                        kprintf("[AGA] PROGRAM: child Copper stable COP1LC=%08x lines=%d; RGB/HAM6 released\n",
                                (unsigned)program_cop1lc, program_display_lines);
                        aga_diag_log("[AGA] PROGRAM: child Copper stable; RGB/HAM6 released");
                    }
                }
            }
            aga_video_present(aga_framebuffer(aga), native);   /* completed frame; stable for 20 ms */
            uint32_t us = (uint32_t)(((cntvct() - t0) * 1000000u) / freq);
            aga_diag_set(AGA_D_FRAME_US, us);
            aga_diag_max(AGA_D_FRAME_US_WORST, us);
            aga_diag_inc(AGA_D_FRAMES);
            aga_diag_inc(AGA_D_PRESENTS);
            {   /* Ctrl-Amiga-Amiga that the housekeeper did not take within
                   20 ms: it is stuck, so this core reboots the Pi (the
                   protocol file's ps_wedge_reset) */
#ifndef FRF_VIRTUAL_AGA
                extern void ps_reset_watch(void);
                ps_reset_watch();
#endif
            }
            if (AGA_PROBES_ON) {   /* how far behind real time the loop is as this frame goes out */
                uint64_t nowt = cntvct();
                uint64_t lagt = nowt > next_ticks ? nowt - next_ticks : 0;
                uint32_t lms = (uint32_t)((lagt * 1000u) / freq);
                int b = lms < 2 ? 0 : lms < 5 ? 1 : lms < 10 ? 2 : lms < 20 ? 3 : lms < 40 ? 4 : 5;
                aga_diag_inc(AGA_D_LAG_HIST0 + b);
                aga_diag_frame_lag(lms);
            }
            {   /* the frame's time budget: busy = everything but the pacing wait */
                uint64_t fn = cntvct(), ft = fn - frame_t0;
                uint64_t busy = ft > wait_acc ? ft - wait_acc : 0;
                uint32_t bus_us = (uint32_t)((busy * 1000000u) / freq);
                aga_diag_set(AGA_D_LOOP_BUSY_US, bus_us);
                aga_diag_max(AGA_D_LOOP_BUSY_WORST, bus_us);
                aga_diag_set(AGA_D_LOOP_WAIT_US, (uint32_t)((wait_acc * 1000000u) / freq));
                aga_diag_set(AGA_D_AUDIO_US_FRAME, (uint32_t)((audio_acc * 1000000u) / freq));
                aga_diag_set(AGA_D_LOCK_US_FRAME, (uint32_t)((lock_acc3 * 1000000u) / freq));
                aga_diag_max(AGA_D_LOCK_US_WORST, (uint32_t)((lock_max3 * 1000000u) / freq));
                lock_acc3 = 0;
                aga_diag_set(AGA_D_CYC_US_FRAME, (uint32_t)((cyc_acc * 1000000u) / freq));
                aga_diag_max(AGA_D_CYC_US_WORST, (uint32_t)((cyc_max * 1000000u) / freq));
                aga_diag_max(AGA_D_BODY_US_WORST, (uint32_t)((body_max * 1000000u) / freq));
                aga_diag_max(AGA_D_CYC_FRAME_WORST, (uint32_t)((cyc_acc * 1000000u) / freq));
#ifdef AGA_PROBES
                { uint64_t l1, l2;
                  __asm__ volatile("mrs %0, PMEVCNTR0_EL0" : "=r"(l1));
                  __asm__ volatile("mrs %0, PMEVCNTR1_EL0" : "=r"(l2));
                  uint32_t d1 = (uint32_t)(l1 - pmu_l1_prev), d2 = (uint32_t)(l2 - pmu_l2_prev);
                  pmu_l1_prev = l1; pmu_l2_prev = l2;
                  aga_diag_set(AGA_D_L1_REFILL_FRAME, d1); aga_diag_max(AGA_D_L1_REFILL_WORST, d1);
                  aga_diag_set(AGA_D_L2_REFILL_FRAME, d2); aga_diag_max(AGA_D_L2_REFILL_WORST, d2); }
                { static uint64_t ab_prev, ac_prev;
                  uint64_t ab = aga_abort_ticks, ac = aga_abort_count;
                  uint32_t aus = (uint32_t)(((ab - ab_prev) * 1000000u) / freq);
                  aga_diag_set(AGA_D_ABORT_US_FRAME, aus); aga_diag_max(AGA_D_ABORT_US_WORST, aus);
                  aga_diag_set(AGA_D_ABORTS_FRAME, (uint32_t)(ac - ac_prev)); aga_diag_max(AGA_D_ABORTS_WORST, (uint32_t)(ac - ac_prev));
                  ab_prev = ab; ac_prev = ac; }
                { uint32_t cw = aga_diag_ctr[AGA_D_CHIP_W];
                  aga_diag_set(AGA_D_CHIPW_FRAME, cw - cw_prev);
                  aga_diag_max(AGA_D_CHIPW_FRAME_WORST, cw - cw_prev); cw_prev = cw; }
                { uint64_t m0 = cntvct(); uint32_t p = (uint32_t)(m0 & 0x3FFFF);
                  for (int i = 0; i < 512; i++) p = mem_probe[p];      /* ~80 us on a Pi 3, less on a Pi 4 */
                  __asm__ volatile("" :: "r"(p));
                  uint32_t mus = (uint32_t)(((cntvct() - m0) * 1000000u) / freq);
                  aga_diag_set(AGA_D_MEM_US, mus); aga_diag_max(AGA_D_MEM_US_WORST, mus); }
#endif
                cyc_acc = 0; cyc_max = 0; body_max = 0;
                frame_t0 = fn; wait_acc = 0; audio_acc = 0;
            }
            stat_t0 = cntvct();
            aga_audio_set_mask(aga, aud_mask_want);   /* survives any reset */
#ifdef AGA_PROBES
            {   /* per-channel audio, once a frame: which channels the game
                   actually runs, and how much data each one pulls */
                uint32_t aon[4], awd[4], anz[4];
                aga_audio_stats(aga, aon, awd, anz);
                for (int n = 0; n < 4; n++) {
                    aga_diag_set(AGA_D_AUD0_ON + n, aon[n]);
                    aga_diag_set(AGA_D_AUD0_W  + n, awd[n]);
                    aga_diag_set(AGA_D_AUD0_NZ + n, anz[n]);
                }
                uint32_t atrig[32 * 6]; int ntrig = 0;
                aga_audio_triggers(aga, atrig, &ntrig);
                aga_diag_triggers(atrig, ntrig);
                uint32_t avol[4], avmax[4], aaud[4];
                aga_audio_vols(aga, avol, avmax, aaud);
                {   /* per-channel loudness for THIS frame, not since boot */
                    static uint32_t nrg_prev[4];
                    uint32_t d[4];
                    for (int n = 0; n < 4; n++) { d[n] = anz[n] - nrg_prev[n]; nrg_prev[n] = anz[n]; }
                    aga_diag_env(d);
                }

                uint32_t vwr[4], vnz[4], voff[4], awrap[4], airq[4];
                aga_audio_volstats(aga, vwr, vnz, voff);
                aga_audio_wrapstats(aga, awrap, airq);
                for (int n = 0; n < 4; n++) aga_diag_set(AGA_D_AUD0_WRAP + n, awrap[n]);
                aga_diag_set(AGA_D_AUD_INTENA, aga_custom_rget(aga, INTENAR));
                { uint32_t wi[32 * 4]; int nwi = 0;
                  aga_audio_wrapinfo(aga, wi, &nwi); aga_diag_wrapinfo(wi, nwi); }
                for (int n = 0; n < 4; n++) {
                    aga_diag_set(AGA_D_AUD0_VWR + n, vwr[n]);
                    aga_diag_set(AGA_D_AUD0_VNZ + n, vnz[n]);
                    aga_diag_set(AGA_D_AUD0_OFF + n, voff[n]);
                    aga_diag_set(AGA_D_AUD0_VOL  + n, avol[n]);
                    aga_diag_set(AGA_D_AUD0_VMAX + n, avmax[n]);
                    aga_diag_set(AGA_D_AUD0_AUD  + n, aaud[n]);
                }
            }
#endif
            int had_data = aga_last_frame_had_data(aga);
            uint32_t bc = aga_blit_count(aga);
            aga_diag_ctr[AGA_D_BLITS] += bc - blit_prev;
            blit_prev = bc;
            if (had_data) aga_diag_inc(AGA_D_DATA_FRAMES);
            aga_diag_snapshot(had_data);   /* survives the reset a wedged game forces */
            /* and put it on the screen, which is the only channel that
               survives a wedge: no OS, no caffed, no keyboard needed. */
            /* The display state, two registers a row. On a classic PiStorm every
               Ctrl-Amiga-Amiga reboots the whole Pi (Emu68's housekeeper does it
               on purpose), so after a game that has no quit - Reshoot Proxima 3 -
               nothing the kernel recorded survives. A photograph of this does.
               Off unless `agaboot OVERLAY ON`; the diag build has it on.
                 0 BPLCON0:DMACONR  1 BPLCON1:BPLCON2  2 BPLCON3:BPLCON4
                 3 FMODE:DIWHIGH    4 DIWSTRT:DIWSTOP  5 DDFSTRT:DDFSTOP
                 6 BPL1MOD:BPL2MOD  7 BPL1PT   8 BPL2PT
                 9 frames (moving = alive)  10 blits  11 last write reg:value */
/* Non-invasive HUD: unlike the old EXECWIDE profiler, this only
 * samples already maintained core status when the HUD is requested.
 * No traversal of guest Exec lists or guest memory reads per frame. */
            if (aga_overlay_idle) {
                aga_video_overlay(0, emu68_get_pc());
                aga_video_overlay(1, emu68_get_sr());
                aga_video_overlay(2, ((uint32_t)aga_custom_rget(aga, INTENAR) << 16) |
                                      aga_custom_rget(aga, INTREQR));
                aga_video_overlay(3, ((uint32_t)aga_custom_rget(aga, DMACONR) << 16) |
                                      aga_peek_reg(aga, BPLCON0));
                aga_video_overlay(4, aga_diag_ctr[AGA_D_FRAMES]);
                aga_video_overlay(5, emu68_get_insn_count());
                aga_video_overlay(6, aga_blit_count(aga));
            }
            wd_deadline = cntvct() + freq * AGA_WATCHDOG_SEC;   /* progress: rearm */
            { extern uint32_t aga_dl_inuse, aga_dl_free_at, aga_dl_free_len;
              aga_diag_set(AGA_D_DL_INUSE, aga_dl_inuse);
              aga_diag_set(AGA_D_DL_FREE_AT, aga_dl_free_at);
              aga_diag_set(AGA_D_DL_FREE_LEN, aga_dl_free_len); }
            {   /* the core gave up the ECS Denise it was told to be: the game
                   turned on an AGA-only feature (playfield.c ecs_says_aga) */
                int e = aga_get_ecs(aga);
                if (ecs_seen == 1 && !e && aga_sandbox_active) {
                    unsigned sg = aga_ecs_signs(aga);
                    aga_diag_log((sg & 4) ? (game_taken ? "[AGA] chipset: AGA after all - the game loaded 24-bit colours across the colour banks"
                                                        : "[AGA] chipset: AGA after all - 24-bit colours across the colour banks, before WHDLoad took the machine")
                                          : (game_taken ? "[AGA] chipset: AGA after all - the game uses an AGA fetch and AGA colours"
                                                        : "[AGA] chipset: AGA after all - an AGA fetch and AGA colours, before WHDLoad took the machine"));
                }
                ecs_seen = e;
                /* and what it saw first, once each: tells a short run from a
                   switch that never came */
                if (e && aga_sandbox_active) {
                    unsigned sg = aga_ecs_signs(aga), fresh = sg & ~ecs_signs_logged & 15u;
                    if (fresh & 1) aga_diag_log("[AGA] chipset: ECS Denise sees an AGA fetch (FMODE or eight planes)");
                    if (fresh & 2) aga_diag_log("[AGA] chipset: ECS Denise sees an AGA colour (LOCT, a bank or BPLCON4)");
                    if (fresh & 8) aga_diag_log("[AGA] chipset: ECS Denise takes 24-bit colours the AGA way");
                    ecs_signs_logged |= sg;
                }
            }
            aga_diag_set(AGA_D_STATE, (uint32_t)(1 | (aga_sandbox_active ? 2 : 0) | (native ? 4 : 0)));
            { uint64_t ds = cntvct() - stat_t0; if (ds > stat_max) stat_max = ds;
              aga_diag_max(AGA_D_STAT_US_WORST, (uint32_t)((stat_max * 1000000u) / freq)); }
        }
        /* audio: every 1024 chunks (~2.3 ms) pull the emulated mix and stream it to the real Paula */
        if ((aga_loop_stop == 0 || aga_loop_stop == 5) && ++audio_countdown >= 1024) {
            audio_countdown = 0;
            int maxf;
            int16_t *buf = aga_audio_scratch(&maxf);
            /* Paula on HDMI band-limited (aga_hdmi.c): the core logs Paula's
               steps for it while it is on - all that fit its ring, so all
               the core has are taken each time */
            static aga_step_t steps[AGA_STEP_RING];
            static int steps_were;
            int bl = aga_hdmi_paula_bl(), ns = 0;
            uint64_t steps_now = 0;
            lock();
            if (bl != steps_were) { aga_audio_steps_on(aga, bl); steps_were = bl; }
            /* at the rate the real Paula plays: 3546895 / 127 on a PAL
               crystal, 3579545 / 128 on an NTSC one (aga_audio.c) */
            extern uint32_t aga_audio_rate(void);
            int n = aga_audio_pull(aga, buf, maxf, (int)aga_audio_rate());
            if (bl) ns = aga_audio_steps(aga, steps, AGA_STEP_RING, &steps_now);
            unlock();
            if (bl && aga_loop_stop == 0)
                aga_hdmi_feed_steps(steps, ns, steps_now, paced_ntsc ? 3579545u : 3546895u);
            /* rung 5 stops here: aga_audio_pull has run (pure computation over
               virtual state, under the lock) but nothing is streamed. Rung 0
               goes on to aga_audio_stream, which issues 32-bit ps_write_32
               PiStorm bus writes from core 3 and reprograms the real Paula.
               Rung 4 survives at 50 fps and rung 0 does not, so the fault is in
               this block; this splits it in half. */
            if (n > 0 && aga_loop_stop == 0) {
                uint64_t a0 = cntvct();
                aga_audio_stream(buf, n);
                uint64_t at = cntvct() - a0;
                audio_acc += at;
                aga_diag_max(AGA_D_AUDIO_US_WORST, (uint32_t)((at * 1000000u) / freq));
            }
        }
        /* if I fell far behind (e.g. blocked on the lock), resynchronise rather than race */
        uint64_t now_ticks = cntvct();
        if (now_ticks > next_ticks) {
            uint64_t behind = ((now_ticks - next_ticks) * (paced_ntsc ? 3579545u : 3546895u)) / freq;
            aga_diag_max(AGA_D_LAG_MAX, (uint32_t)behind);
        }
        /* Give up and drop the owed time only when hopelessly behind - a whole
           frame - not after a normal one.
           The threshold used to be four scanlines, 256us. Presenting a frame
           takes about 900us (frame_us_worst), so EVERY frame overshot it and
           resynchronised: 4848 resyncs in 4990 frames. Each one discards the
           time it fell behind, and the audio mix is generated from this very
           clock, so the stream ran short by ~900us a frame, the Paula ring
           repeated what was already in it, and the music came out with its
           beats in the wrong places.
           The ARM runs far faster than the emulated chipset, so the honest
           answer is to catch the cycles up, which the loop does by itself as
           soon as the present returns. */
        /* Four scanlines, NOT a frame.
         *
         * Raising this to a frame looked right - presenting a frame takes about
         * 900us and the loop resynchronised on every one of them - but it put
         * Banshee straight back to a blank screen with blits at zero.
         *
         * Resynchronising DROPS the time it owes; catching up REPLAYS it, and
         * the loop catches up as fast as the ARM can run. So with a frame-sized
         * budget the chipset runs ~3200 colour clocks in a few microseconds of
         * real time after every present. The blitter's completion delay is
         * counted in colour clocks (BLIT_DELAY_CCK), so inside a catch-up burst
         * those 64 colour clocks expire in well under a microsecond - before
         * the game has armed the wait it is about to sleep on, which is the
         * very race the delay exists to prevent.
         *
         * Anything measured in emulated colour clocks is only as trustworthy as
         * the loop's willingness to run them at real-time speed. Four scanlines
         * keeps that promise; a frame does not. The audio drift this was meant
         * to fix is handled where it belongs, in aga_audio.c, one sample at a
         * time - audio_resyncs went from 31 in 110 seconds to 0. */
        /* A whole frame, not four scanlines.
         *
         * Resynchronising DROPS the time the loop owes. Presenting a frame
         * costs about 900us - 4.5% of a 20 ms frame - so at four scanlines the
         * loop discarded that on essentially every frame (1883 resyncs in 1993
         * frames), and the emulated machine, with the audio generated from its
         * clock, ran 4.5% slow. That is what every nudge and ring restart in
         * aga_audio.c has been trying to paper over.
         *
         * Catching up instead was impossible while the blitter's completion
         * delay was counted in colour clocks, because a catch-up burst replays
         * thousands of them in a few microseconds. Now that the glue times the
         * blit against the real clock, the two are independent and the loop can
         * pay back what it owes. */
        if (skip_ok) {
            uint64_t behind_t = now_ticks > next_ticks ? now_ticks - next_ticks : 0;
            int want = skip_want;
            if (behind_t > ticks_per_frame / 8) want = 1;
            else if (behind_t < ticks_per_frame / 50) want = 0;
            if (want != skip_want) { skip_want = want; aga_set_frame_skip(aga, want); }
            aga_diag_set(AGA_D_FRAMES_SKIPPED, aga_frames_skipped(aga));
        }
        if (now_ticks > next_ticks + resync_ticks) {
            next_ticks = now_ticks;
            next_frac  = 0;
            aga_diag_inc(AGA_D_RESYNCS);
        }
    }
    frame_pending = 0;
  }
}

/* ---- virtual floppy: CIA lines, image commands, disk-swap hotkey ----------- */
#ifdef FRF_VIRTUAL_AGA
int aga_boot_insert_adf(int drive, const uint8_t *adf, uint32_t size, int wprot)
{
    int r;
    if (!aga || !adf || drive < 0 || drive >= AGA_MAX_DRIVES) return -1;
    lock();
    r = aga_disk_insert(aga, drive, adf, size, wprot);
    unlock();
    if (r == 0)
        kprintf("[AGA] boot floppy: DF%d <- SD initramfs ADF (%d bytes)%s\n",
                drive, size, wprot ? " [WP]" : "");
    else
        kprintf("[AGA] boot floppy: DF%d insert failed (%d bytes, rc=%d)\n",
                drive, size, r);
    return r;
}

void aga_virtual_disk_prb(uint8_t prb)
{
    if (!aga || !aga_disk_active(aga)) return;
    lock();
    aga_disk_cia_prb(aga, prb);
    unlock();
}

int aga_virtual_disk_pra(uint8_t *bits)
{
    int r;
    if (!aga || !bits || !aga_disk_active(aga)) return 0;
    lock();
    r = aga_disk_cia_pra(aga, bits);
    unlock();
    return r;
}
#endif

#define DSK_SLOTS 4
static uint8_t *dsk_slot[DSK_SLOTS];      /* images kept for Ctrl+F1..F4 swapping */
static uint32_t dsk_slot_size[DSK_SLOTS];
static int      key_ctrl;

static int slot_insert(int n)
{
    if (!aga || n < 0 || n >= DSK_SLOTS || !dsk_slot[n]) return 0;
    lock();
    int rc = aga_disk_insert(aga, 0, dsk_slot[n], dsk_slot_size[n], 1);
    unlock();
    if (rc != 0) return 0;
    kprintf("[AGA] disk: DF0 <- slot %d (%d KB) [WP]\n", n + 1, dsk_slot_size[n] >> 10);
    return 1;
}

/* The CIA hooks exist only for the virtual drives: with no image inserted they
   do nothing and the normal Emu68 bus path handles the access unchanged, so a
   WHDLoad game without a floppy never sees them. */
int aga_cia_write(uint64_t far, int size, uint64_t value)
{
#ifdef FRF_VIRTUAL_AGA
    (void)far; (void)size; (void)value;
    return 0;
#else
    if (!aga_enabled || !aga || !aga_disk_active(aga)) return 0;
    if (far == 0xBFD100 && (size == 1 || size == 2)) {
        /* CIA-B lives on the upper data byte: a word access carries PRB in bits 8-15 */
        uint8_t prb = (size == 1) ? (uint8_t)value : (uint8_t)(value >> 8);
        lock();
        aga_disk_cia_prb(aga, prb);
        uint8_t mask = aga_disk_cia_prb_mask(aga);
        unlock();
        prb |= mask;                               /* virtual drives stay deselected on the real bus */
        if (size == 1) ps_write_8(0xBFD100, prb);
        else           ps_write_16(0xBFD100, ((unsigned)prb << 8) | (value & 0xFF));
        return 1;
    }
    return 0;
#endif
}

int aga_cia_read(uint64_t far, int size, uint64_t *value)
{
#ifdef FRF_VIRTUAL_AGA
    (void)far; (void)size; (void)value;
    return 0;
#else
    /* The mouse quit. WHDLoad, once it has moved the VBR, compares its quit
       key with the keyboard's serial register at EVERY interrupt, whatever
       handlers the program installed - which is why F10 gets out of most
       games. A demo that takes the keyboard over still gets interrupts,
       so for a second the register reads as F10 going down, exactly as
       the keyboard would send it (~(0x59 << 1)), and WHDLoad quits the
       ordinary way: OS restored, cleanup run, sandbox left. */
    if (size == 1 && far == 0xBFEC01 && aga_quit_until) {
        uint64_t now;
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
        if (now < aga_quit_until) { *value = 0x4D; return 1; }
        aga_quit_until = 0;
    }
    if (!aga_enabled || !aga || !aga_disk_active(aga)) return 0;
    if (size == 2 && far == 0xBFE000) {
        /* A WORD read spanning $BFE000-$BFE001. The CIA sits on the odd byte
           lane, so the low half is the register and the high half is whatever
           the bus floats. Kickstart and bootblocks do use this form, and until
           now it fell straight through to the real bus, where the virtual
           drive's READY, TRACK0, WPROT and CHANGE bits do not exist - so a
           machine booting from the virtual floppy polled a drive that never
           answered. */
        uint16_t w = (uint16_t)ps_read_16(0xBFE000);
        uint8_t bits;
        lock();
        int virt = aga_disk_cia_pra(aga, &bits);
        unlock();
        if (virt) w = (uint16_t)((w & ~0x3C) | (bits & 0x3C));
        *value = w;
        return 1;
    }
    if (size != 1) return 0;
    if (far == 0xBFE001) {
        uint8_t v = (uint8_t)ps_read_8(0xBFE001), bits;
        lock();
        int virt = aga_disk_cia_pra(aga, &bits);
        unlock();
        if (virt) v = (v & ~0x3C) | (bits & 0x3C);
        *value = v;
        return 1;
    }
    if (far == 0xBFEC01) {
        /* keyboard data: watch for Ctrl+F1..F4 = insert swap slot 1..4 into DF0 */
        uint8_t v = (uint8_t)ps_read_8(0xBFEC01);
        uint8_t raw = (uint8_t)~((v >> 1) | (v << 7));
        int up = raw & 0x80, code = raw & 0x7F;
        if (code == 0x63) key_ctrl = !up;
        else if (!up && key_ctrl && code >= 0x50 && code <= 0x53) slot_insert(code - 0x50);
        *value = v;
        return 1;
    }
    return 0;
#endif
}

/* $DFF1F2 = $ADxx with the image address in $DFF1EC/$DFF1EE (68k address of a
   fast RAM buffer) and $DFF1F0 = size in KB | flags << 12 (bit 12: write protect):
     $ADF0/$ADF1  insert the image into DF0/DF1 (also kept as swap slot 1 for DF0)
     $ADC1..$ADC4 load the image into swap slot 1..4 (Ctrl+F1..F4 inserts it into DF0)
     $ADE0/$ADE1  eject DF0/DF1 */
/* Runtime AmigaOS mount tool reads the last command's status on $DFF1F2.
 * $AD80=idle/protocol present; $AD81=inserted; $AD82=ejected;
 * $AD83=slot ready; $AD40=error; $AD41=chipset unavailable.
 */
static volatile uint16_t frf_adf_status = 0xAD80u;
uint16_t aga_disk_status(void) { return frf_adf_status; }
int aga_disk_command(unsigned cmd)
{
    aga_diag_inc(AGA_D_DISK_CMDS);
    if (!aga_enabled || !aga) { frf_adf_status = 0xAD41u; kprintf("[AGA] disk command %02x ignored (chipset not active)\n", cmd); return 0; }
    unsigned op = (cmd >> 4) & 0xF, n = cmd & 0xF;
    if (op == 0xB && n >= 1 && n <= DSK_SLOTS) {
        int rc = slot_insert(n - 1);
        frf_adf_status = rc ? 0xAD84u : 0xAD40u;
        return rc;
    }
    if (op == 0xE) {
        lock(); aga_disk_eject(aga, n & 1); unlock();
        kprintf("[AGA] disk: DF%d ejected\n", n & 1);
        frf_adf_status = 0xAD82u;
        return 1;
    }
    uint32_t addr = ((uint32_t)aga_peek_reg(aga, 0x1EC) << 16) | aga_peek_reg(aga, 0x1EE);
    uint32_t szf = aga_peek_reg(aga, 0x1F0);
    uint32_t size = (szf & 0xFFF) << 10;
    int wprot = (szf >> 12) & 1;
    if (addr < 0x200000 || (uint64_t)addr + size > 0x100000000ULL || (size != 901120 && size != 1802240)) {
        frf_adf_status = 0xAD40u;
        kprintf("[AGA] disk command %02x: bad image %08x/%d\n", cmd, addr, size);
        return 0;
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)addr;   /* 68k fast RAM is Pi memory, identity mapped */
    if (op == 0xF && n < 2) {
        lock(); int r = aga_disk_insert(aga, n, src, size, wprot); unlock();
        if (n == 0 && r == 0) {
            if (!dsk_slot[0]) dsk_slot[0] = calloc(1, 1802240);
            if (dsk_slot[0]) { memcpy(dsk_slot[0], src, size); dsk_slot_size[0] = size; }
        }
        frf_adf_status = r == 0 ? 0xAD81u : 0xAD40u;
        kprintf("[AGA] disk: DF%d insert %d KB from %08x -> %d\n", n, size >> 10, addr, r);
        return r == 0;
    }
    if (op == 0xC && n >= 1 && n <= DSK_SLOTS) {
        if (!dsk_slot[n - 1]) dsk_slot[n - 1] = calloc(1, 1802240);
        if (!dsk_slot[n - 1]) { frf_adf_status = 0xAD40u; return 0; }
        memcpy(dsk_slot[n - 1], src, size);
        dsk_slot_size[n - 1] = size;
        frf_adf_status = 0xAD83u;
        kprintf("[AGA] disk: slot %d loaded (%d KB)\n", n, size >> 10);
        return 1;
    }
    frf_adf_status = 0xAD40u;
    return 0;
}

void aga_machine_reset(void)
{
    if (!aga_enabled || !aga) return;
    lock();
    uint16_t ctrl = aga_peek_reg(aga, 0x1F2), win[4];
    for (int i = 0; i < 4; i++) win[i] = aga_peek_reg(aga, 0x1F4 + 2 * i);
    aga_reset(aga);                           /* registers; disks stay inserted */
    fwd_forget();
    for (int i = 0; i < 4; i++) aga_custom_wput(aga, 0x1F4 + 2 * i, win[i]);
    aga_custom_wput(aga, 0x1F2, ctrl);        /* keep the fullscreen/windowed choice */
    unlock();
    key_ctrl = 0;
    kprintf("[AGA] machine reset on the virtual chipset (ctrl %04x)\n", ctrl);
}
