/* AGA-PISTORM - diagnostics readable from the Amiga side.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The point of this file is the first boot on real hardware. If the picture
 * never appears there is still caffed: it auto-starts from S:User-Startup and
 * dials the PC, so `agastat` can read these counters and the log ring over
 * the network with no display and no serial cable.
 *
 * Cost. Everything here runs at frame granularity or rarer, so the shipped
 * build is free: a handful of increments per 20 ms. The expensive shit -
 * per-trap and per-DMA counters - is compiled out unless AGA_DIAG is defined,
 * because the trap path runs millions of times a second.
 *
 * Interface (Pi-side registers, not Amiga ones; handled in vectors.c so they
 * work in machine mode and sandbox mode alike):
 *
 *   write $DFF1E8 = selector     select a counter (0..) or the log ($100)
 *   read  $DFF1EA                next 16-bit word of the selection, auto-advancing
 *
 * A counter reads back as two words, high then low. The log reads back as a
 * stream of characters, two per word, ending at a zero byte.
 */
#include <stdint.h>
#include "aga_glue.h"

extern void kprintf(const char *fmt, ...);

extern uint32_t emu68_get_pc(void);   /* 68k PC, for the PC ring */

/* ---- counters ------------------------------------------------------------- */
/* Keep this list append-only: agastat prints by index and an older tool
   against a newer kernel must still line up. */
uint32_t aga_diag_ctr[AGA_DIAG_COUNT];

static const char *const ctr_name[AGA_DIAG_COUNT] = {
    "frames",            /* frames completed by the chipset core            */
    "frame_us_last",     /* microseconds spent rendering the last frame     */
    "frame_us_worst",    /* worst since boot: the number that decides if a
                            Pi core can sustain 50 Hz                       */
    "loop_lag_max_cck",  /* worst the chipset loop fell behind, colour clocks */
    "loop_resyncs",      /* times it gave up and resynchronised             */
    "audio_streams",     /* audio buffers pushed to the real Paula          */
    "audio_resyncs",     /* DAC loop restarts: a few per hour is normal     */
    "video_presents",    /* frames handed to the HVS                        */
    "video_errors",      /* mailbox or display list failures                */
    "sandbox_enters",
    "sandbox_leaves",
    "disk_commands",
    "reboots_requested",
    "traps_read",        /* AGA_DIAG only, else 0 */
    "traps_write",       /* AGA_DIAG only, else 0 */
    "state",             /* bit0 enabled, bit1 sandbox, bit2 native active  */
    "bpl_frames",        /* frames with bitplanes enabled: 0 = no game ever drew */
    "ipl_hook",          /* virtual chipset raised a non-zero IPL             */
    "irq_from_virtual",  /* exceptions where the virtual chipset set the level */
    "irq_l1", "irq_l2", "irq_l3", "irq_l4", "irq_l5", "irq_l6", "irq_l7",   /* exceptions taken, per level */
    "real_intena", "real_intreq", "real_dmacon",   /* the real chip, sampled per frame in the sandbox */
    "blits", "bpl_data_frames",
    "chip_reads", "chip_writes",
    "chipw_0-256k", "chipw_256-512k", "chipw_512-768k", "chipw_768k-1m",
    "chipw_1m-1.25m", "chipw_1.25-1.5m", "chipw_1.5-1.75m", "chipw_1.75-2m",
};

void aga_diag_set(int idx, uint32_t v) { if ((unsigned)idx < AGA_DIAG_COUNT) aga_diag_ctr[idx] = v; }
void aga_diag_inc(int idx)             { if ((unsigned)idx < AGA_DIAG_COUNT) aga_diag_ctr[idx]++; }
void aga_diag_max(int idx, uint32_t v) { if ((unsigned)idx < AGA_DIAG_COUNT && v > aga_diag_ctr[idx]) aga_diag_ctr[idx] = v; }
void aga_diag_jit_new(uint32_t m68k_addr)
{
    if (!aga_sandbox_active) return;
    aga_diag_ctr[AGA_D_JIT_NEW]++;
    if (m68k_addr < 0x200000) aga_diag_ctr[AGA_D_JIT_NEW_CHIP]++;
}

void aga_diag_irq(int level, int from_virtual)
{
    if (level >= 1 && level <= 7) aga_diag_ctr[AGA_D_IRQ_L1 + level - 1]++;
    if (from_virtual) aga_diag_ctr[AGA_D_IRQ_VIRT]++;
}

/* ---- log ring -------------------------------------------------------------
   Only my own [AGA] messages go in here, and they are rare (init, mode
   changes, faults), so the cost is nil. It exists so a machine that shows
   damn all can still tell how far it got. */
#define LOG_SIZE 4096
static char     log_buf[LOG_SIZE];
static uint32_t log_head;            /* next write position, wraps */
static int      log_wrapped;

/* Lock-free and whole lines only. Core 0 (traps), core 2 (housekeeper) and
   core 3 (chipset loop) all log, and the old byte-at-a-time copy let two of
   them interleave: "core 3 confirmed rring th[AGA] sandbox entered" on the
   CM4 (2026-09-14). Each writer now reserves its line's bytes with one
   compare-and-swap on the head and copies into them after; no lock, so a
   core that logs from a handler cannot deadlock on itself. */
void aga_diag_log(const char *s)
{
    uint32_t len = 0;
    while (s[len] && len < LOG_SIZE - 2) len++;
    uint32_t need = len + 1, h, nh;
    do {
        h = __atomic_load_n(&log_head, __ATOMIC_ACQUIRE);
        nh = h + need;
        if (nh >= LOG_SIZE) nh -= LOG_SIZE;
    } while (!__atomic_compare_exchange_n(&log_head, &h, nh, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
    if (h + need >= LOG_SIZE) log_wrapped = 1;
    for (uint32_t i = 0; i < len; i++) log_buf[(h + i) % LOG_SIZE] = s[i];
    log_buf[(h + len) % LOG_SIZE] = '\n';
}

/* A core that saw Ctrl-Amiga-Amiga while the housekeeper did not take it
   (ps_wedge_reset in the protocol files): one line for the kept session, so
   the next boot's C:AGA-diag.log says where the machine was stuck. No
   printf here - it runs on a core that may hold the bus lock, and the log
   can damn well wait. */
static char *wl_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *wl_u(char *o, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}
static char *wl_x(char *o, uint64_t v)
{
    for (int i = 15; i >= 0; i--) *o++ = "0123456789abcdef"[(v >> (i * 4)) & 15];
    return o;
}
void aga_diag_wedge(int where, unsigned core, unsigned owner, uintptr_t ra, uint32_t stuck_ms)
{
    char line[224], *o = line;
    o = wl_s(o, "[AGA] Ctrl-Amiga-Amiga, the housekeeper stuck: core ");
    o = wl_u(o, core);
    o = wl_s(o, where == 1 ? " was in a bus transaction that never ended"
              : where == 2 ? " was waiting for the bus"
              : " (chipset core, frame watch) took it");
    if (stuck_ms) { o = wl_s(o, " for "); o = wl_u(o, stuck_ms); o = wl_s(o, " ms"); }
    if (owner) {
        o = wl_s(o, "; bus held by core ");
        o = wl_u(o, owner - 1);
        o = wl_s(o, " for the code at ");
        o = wl_x(o, (uint64_t)ra);
    } else {
        o = wl_s(o, "; bus free");
    }
    *o = 0;
    aga_diag_log(line);
}

/* ---- trap ring (AGA_DIAG only) ---------------------------------------------
   The last TRAP_RING custom-register accesses the 68k made, oldest first.

   Counters tell you HOW MANY registers were touched; when a game stops dead
   they cannot tell you WHICH, and that is the only question left. A steady 32
   traps per frame is AmigaOS idling; a game setting up a display writes
   BPLCON0, the bitplane pointers, DDF/DIW and a copper list, and the ring
   shows that happening - or shows the last thing before it stopped.

   Cost is one store and one increment per trap, no lock: only core 0 writes
   here, from the abort handler, and the reader tolerates a torn tail. */
#define TRAP_RING 1024
/* Freeze. A WHDLoad game that draws nothing has to be quit to be read, and by
   then the OS is back: it re-programs the display, floods the trap ring with
   its own INTENA pairs, and the state I wanted is gone. So when the plane
   count DROPS from a game's display (>= 4 planes) to the OS's (< 4), stop
   recording. Everything then holds the game's last moments: the display
   registers, the last accesses with their PCs, and where the CPU was. */
static int diag_frozen;
static int blank_streak;
#define PC_RING 24
static uint32_t pc_ring[PC_RING];   /* one 68k PC per presented frame */
static uint32_t pc_head;
int aga_diag_is_frozen(void) { return diag_frozen; }

/* Which register is a stuck game reading, and what do I keep handing back?
 *
 * The trap ring answers "what did it touch last", but it skips the beam
 * registers on purpose: they are ~90% of everything a game reads, so a game
 * that sits in a raster wait leaves a ring full of its interrupt handler and
 * no trace of the wait itself. These two tables cover that gap and cost one
 * increment per access.
 *
 * reg_reads/reg_writes are indexed by register number >> 1 (256 entries).
 * beam_val counts the VALUES a beam read returned, in 64 buckets of 1024, so
 * a wait for a position I never produce shows up as an empty bucket. */
static uint32_t reg_reads[256], reg_writes[256];
static uint32_t beam_val[64];
void aga_diag_reg(uint32_t reg, int write)
{
    if (diag_frozen) return;
    uint32_t i = (reg >> 1) & 255;
    if (write) reg_writes[i]++; else reg_reads[i]++;
}
void aga_diag_beam_value(uint16_t v)
{
    if (diag_frozen) return;
    beam_val[(v >> 10) & 63]++;
}
/* CIA accesses are the one blind spot: only $BFE001 and $BFEC01 are hooked,
   the rest of $BFxxxx goes straight to the real bus, so a game polling a CIA
   timer thousands of times a second leaves no trace in anything here. Index
   is the register select, (far >> 8) & 15, split by chip. */
static uint32_t cia_a[16], cia_b[16];
/* And the VALUES on CIA-B port A, where a stuck Banshee spends 69 reads a
   frame. Counts alone say a protocol is running; only the values say whether
   it waits for a bit that never changes. One counter per byte value, read
   and written. */
static uint32_t bpra_rd[256], bpra_wr[256];
void aga_diag_cia(uint32_t far, int write)
{
    if (diag_frozen) return;
    uint32_t r = (far >> 8) & 15;
    /* $BFD000 has a D in bits 12-15, $BFE001 an E: bit 12 is SET for CIA-B and
       CLEAR for CIA-A. This was the wrong way round, so every CIA-B ICR read -
       the very access that releases a stuck /INT line - was reported as CIA-A,
       and the keyboard traffic as CIA-B. Two days on that shit. */
    if (far & 0x1000) cia_b[r] += write ? 0x10000 : 1;   /* $BFDxxx, CIA-B */
    else              cia_a[r] += write ? 0x10000 : 1;   /* $BFExxx, CIA-A */
}
/* The last 64 CIA accesses in order, with their values. Histograms said a
   handshake was running and never completing; only the sequence says what it
   IS - which line is driven, how many polls follow it, whether anything ever
   answers. Entry: write<<15 | reg<<8 | value, reg = (far>>8)&15, plus bit 14
   for CIA-B (bit 12 of the address is set for $BFDxxx). */
#define CIA_RING 64
static uint16_t cia_ring[CIA_RING];
static uint32_t cia_head, cia_wrapped;
void aga_diag_cia_value(uint32_t far, int write, uint8_t v)
{
    if (diag_frozen) return;
    cia_ring[cia_head] = (uint16_t)((write ? 0x8000 : 0) | ((far & 0x1000) ? 0x4000 : 0)
                                    | (((far >> 8) & 15) << 8) | v);
    if (++cia_head >= CIA_RING) { cia_head = 0; cia_wrapped = 1; }
    /* CIA-A PRA ($BFE001) only. This filter was labelled "CIA-B PRA", which it
       can never match: $BFD000 & $1F00 is $1000, so the first clause always
       returned. What it actually collects is CIA-A port A - OVL, LED, CHNG,
       WPRO, TK0, RDY and the two fire buttons - and the values bear that out:
       $7E and $FE differ only in bit 7, FIR1. The long "handshake" I spent a
       day reading as a stuck serial protocol was a game polling its joystick.
        A fucking joystick. */
    if ((far & 0x1F00) != 0x0000) return;
    if (write) bpra_wr[v]++; else bpra_rd[v]++;
}

/* Where the 68k is actually executing, when nothing else can tell.
 * emu68_get_pc() reads the JIT's context copy, which under a running game is
 * stale - it read $36124784 for every sample and every trap, a value that is
 * not a 68k address at all. But the JIT fetches instructions through my chip
 * RAM redirect as 16-byte cache-line fills, and THOSE addresses are the
 * program counter rounded down to 16. Sixteen slots, most-executed first, is
 * enough to name a loop. */
#define PCH_SLOTS 16
static uint32_t pch_addr[PCH_SLOTS], pch_hits[PCH_SLOTS];
void aga_diag_fetch(uint32_t addr)
{
    if (diag_frozen) return;
    addr &= ~15u;
    for (int i = 0; i < PCH_SLOTS; i++) {
        if (pch_hits[i] && pch_addr[i] == addr) { pch_hits[i]++; return; }
    }
    /* not tracked yet: take a free slot, else evict the coldest */
    int cold = 0;
    for (int i = 0; i < PCH_SLOTS; i++) {
        if (!pch_hits[i]) { pch_addr[i] = addr; pch_hits[i] = 1; return; }
        if (pch_hits[i] < pch_hits[cold]) cold = i;
    }
    if (pch_hits[cold] > 1) { pch_hits[cold]--; return; }   /* decay, do not thrash */
    pch_addr[cold] = addr; pch_hits[cold] = 1;
}

/* Everything WHDLoad sends down the serial line, in order.
 *
 * WHDLoad sets SERPER and raises DTR/RTS at startup, and its exception handler
 * formats a register dump - the binary carries "Access Fault", "Address Error",
 * "Illegal Instruction", a register-name table and
 *   "19.2.%ld eaf=%x waf=%x vbr=%lx cacr=%lx tc=%lx pcr=%lx bplcon0=%x chiprev=%x"
 * - then pages it, waiting for a keypress that can never arrive because no
 * terminal is attached. That is why the machine sits there toggling handshake
 * lines forever.
 *
 * This ring is NOT cleared by aga_diag_reg_reset(), on purpose: the dump is
 * sent once, the instant the fault happens, which is before any window that
 * starts counting after the display goes blank. That is why I have never
 * seen it. Kept from sandbox entry until the sandbox is left. */
/* The 68k code the machine was executing when it froze.
 *
 * Every counter here says WHAT is being touched; none says WHO is touching
 * it. Banshee sits at one fixed PC for every frame of the blank, polling
 * POTGOR and the CIA-B handshake port and never drawing - and no amount of
 * register counting identifies that loop. The code does.
 *
 * Emu68 dereferences 68k addresses directly as ARM pointers - ExecutionLoop.c
 * hands the raw PC to M68K_GetTranslationUnit((uint16_t *)(uintptr_t)PC) - so
 * anything the JIT just executed from is readable here. Chip RAM is the one
 * exception (it faults into the redirect), so low addresses are refused. */
/* Where the 68k REALLY is.
 *
 * emu68_get_pc() reads the JIT's context copy, and the note above says why
 * that is useless under a running game: it is only synced at certain
 * boundaries, so it returns the same stale value for every sample.
 * Twenty-four identical samples do not mean a loop, they mean nothing was
 * written.
 *
 * The interrupt dispatch in ExecutionLoop.c is different. To take an interrupt
 * the 68k must push its program counter, so the PC in scope there is the real
 * one, at the instant the game was interrupted. Sampling it is an honest
 * profiler running at interrupt rate - several thousand samples a second on a
 * game with a vertical blank handler - and the hottest bucket is where the
 * machine actually spends its time.
 *
 * Buckets are 256 bytes, fine enough to name a routine and coarse enough
 * that 24 slots cover a whole game loop. */
#define PCS_SLOTS 24
static uint32_t pcs_addr[PCS_SLOTS], pcs_hits[PCS_SLOTS];
/* the last exact PCs, unbucketed: a 256-byte bucket names a routine, but a
   spin is usually three or four instructions and only the exact addresses
   show where it turns back */
/* the line numbers a raster wait gets handed: a game polling $DFF005 and
   $DFF006 for one exact line can only make progress if it is handed that
   line, so keep the last few composed values with the gap between them */
#define BEAMSEQ_RING 96
static uint32_t beamseq[BEAMSEQ_RING];
static uint32_t beamseq_head;
/* which line numbers got handed out, one bit each: if a raster wait is stuck
   and every line in the frame appears here, the line it waits for is not one
   that can be produced, and the fault is in what gets composed, not coverage */
/* 0-7: line low bytes ($DFF006), 8-15: positions along a line ($DFF007),
   16-31: the 9-bit line numbers composed out of a $DFF005+$DFF006 pair */
static uint32_t beam_cov[32];
static uint16_t beam_or, beam_and = 0xFFFF;
void aga_diag_beam_reset(void)
{
    for (int i = 0; i < 32; i++) beam_cov[i] = 0;
    beam_or = 0; beam_and = 0xFFFF;
    beamseq_head = 0;
    aga_diag_ctr[AGA_D_LOOP_VPOS_MAX] = 0;
    aga_diag_ctr[AGA_D_TRAP_VPOS_MAX] = 0;
    aga_diag_ctr[AGA_D_TRAP_ADD_MAX] = 0;
    aga_diag_ctr[AGA_D_BEAM005_OR] = 0;
    aga_diag_ctr[AGA_D_BEAM005_READS] = 0;
    aga_diag_ctr[AGA_D_BEAM005_SET] = 0;
    for (int i = 0; i < BEAMSEQ_RING; i++) beamseq[i] = 0;
}
void aga_diag_beam_byte(uint8_t v, uint32_t us, int is_hpos)
{
    if (diag_frozen) return;
    if (us > 0xFFFF) us = 0xFFFF;
    beamseq[beamseq_head % BEAMSEQ_RING] =
        (is_hpos ? 0x40000000u : 0) | ((uint32_t)v << 16) | us;
    beamseq_head++;
    beam_cov[(is_hpos ? 8 : 0) + (v >> 5)] |= 1u << (v & 31);
    beam_or |= v; beam_and &= v;
}

/* the 9-bit line number a game composed out of two byte reads: for a stuck
   raster wait, the whole question is whether the line it wants is in here */
void aga_diag_beam_line(uint16_t line, uint32_t us)
{
    if (diag_frozen) return;
    if (us > 0xFFFF) us = 0xFFFF;
    line &= 0x1FF;
    beamseq[beamseq_head % BEAMSEQ_RING] = 0x80000000u | ((uint32_t)line << 16) | us;
    beamseq_head++;
    beam_cov[16 + (line >> 5)] |= 1u << (line & 31);
}
#define PCE_RING 32
static uint32_t pce[PCE_RING];
static uint32_t pce_head;
void aga_diag_pc_sample(uint32_t pc)
{
    if (diag_frozen || !pc) return;
    pce[pce_head % PCE_RING] = pc;
    pce_head++;
    uint32_t b = pc & ~255u;
    int cold = -1;
    for (int i = 0; i < PCS_SLOTS; i++) {
        if (!pcs_hits[i]) { if (cold < 0) cold = i; continue; }
        if (pcs_addr[i] == b) { pcs_hits[i]++; return; }
        if (cold < 0 || (pcs_hits[cold] && pcs_hits[i] < pcs_hits[cold])) cold = i;
    }
    pcs_addr[cold] = b;
    pcs_hits[cold] = 1;
}
/* Freeze everything where it stands and keep the 68k code around the PC the
   samples cluster on. Used by the stall watchdog: once the machine is handed
   back the desktop runs, and without this it shits right over the very
   rings that say where the game stopped. */
/* One line per frame, last 64 kept: the 68k's SR, how much it said to the
   chipset and to chip RAM, how many interrupts it got. A game that stops
   dead leaves the shape of its last seconds here - which of those went
   quiet first, and whether interrupts kept arriving after it did. */
#define TRACE_LEN 64
static uint32_t trace_a[TRACE_LEN], trace_b[TRACE_LEN];
static uint32_t trace_c[TRACE_LEN], trace_d[TRACE_LEN];
static uint32_t trace_head;
/* The long view: one sample a frame for the last 4096 frames (82 s) - where
   the interrupt found the 68k, its SR, that frame's chip RAM writes - and it
   does NOT stop when the freeze detector fires. A demo whose effects never
   appear leaves the shape of the whole part here: a render loop spread over
   many PCs, or one PC it sits and waits in. Kept across a soft reset with
   the code around its most frequent PCs (agastat PREVPROF). */
#define PROF_LEN 4096
#define PROF_CODE 12                 /* code blocks kept at a reset */
#define PROF_CODE_BYTES 512
static uint32_t prof_pc[PROF_LEN];
static uint16_t prof_sr[PROF_LEN], prof_cw[PROF_LEN], prof_lag[PROF_LEN];
static uint32_t prof_head, prof_lag_now;
void aga_diag_frame_lag(uint32_t ms) { prof_lag_now = ms > 0xFFFF ? 0xFFFF : ms; }
void aga_diag_frame_trace(uint32_t sr, uint32_t traps, uint32_t chipw,
                          uint32_t l3, uint32_t l6, uint32_t ticks, uint32_t pc)
{
    prof_pc[prof_head % PROF_LEN] = pc;
    prof_sr[prof_head % PROF_LEN] = (uint16_t)sr;
    prof_cw[prof_head % PROF_LEN] = (uint16_t)(chipw > 0xFFFF ? 0xFFFF : chipw);
    prof_lag[prof_head % PROF_LEN] = (uint16_t)prof_lag_now;   /* ms late, the last frame out */
    prof_head++;
    if (diag_frozen) return;
    if (traps > 0xFFFF) traps = 0xFFFF;
    if (chipw > 0xFFFF) chipw = 0xFFFF;
    if (l3 > 0xFF) l3 = 0xFF;
    if (l6 > 0xFF) l6 = 0xFF;
    trace_a[trace_head % TRACE_LEN] = ((sr & 0xFFFF) << 16) | traps;
    trace_b[trace_head % TRACE_LEN] = (chipw << 16) | (l3 << 8) | l6;
    trace_c[trace_head % TRACE_LEN] = ticks;
    trace_d[trace_head % TRACE_LEN] = pc;
    trace_head++;
}

static uint32_t intreqr_pc;
void aga_diag_intreqr_pc(uint32_t pc)
{
    if (diag_frozen) return;
    intreqr_pc = pc;
    aga_diag_ctr[AGA_D_INTREQR_PC] = pc;
    aga_diag_ctr[AGA_D_INTREQR_N]++;
}
void aga_diag_intreqr_val(uint32_t v)
{
    if (diag_frozen) return;
    v &= 0xFFFF;
    aga_diag_ctr[AGA_D_INTREQR_VAL] = v;
    aga_diag_ctr[AGA_D_INTREQR_OR] |= v;
    if (!aga_diag_ctr[AGA_D_INTREQR_N] || aga_diag_ctr[AGA_D_INTREQR_N] == 1)
        aga_diag_ctr[AGA_D_INTREQR_AND] = v;
    else
        aga_diag_ctr[AGA_D_INTREQR_AND] &= v;
}

/* ---- surviving a soft reset ---------------------------------------------
 * On a classic PiStorm a keyboard reset makes Emu68's housekeeper reboot the
 * whole Raspberry Pi through its watchdog: the kernel is loaded again and
 * .bss is cleared, so a machine that had to be reset out of a hang took the
 * only evidence of the hang with it. The RAM stays powered through a
 * watchdog reboot (Linux's ramoops relies on the same shit). So the moment
 * the reset lands, the housekeeper copies counters, log and frame trace into
 * a block nobody clears - the top of the unused 2 MB guard below the AGA
 * chip RAM - and the next boot keeps it as the previous session:
 * `agastat PREV`. The magic, the size and a sum keep a cold power-on's
 * random RAM out. */
#define PERSIST_MAGIC 0x41474150u     /* 'AGAP' */
struct persist {
    uint32_t magic, size, sum;
    uint32_t log_head, log_wrapped, trace_head;
    uint32_t ctr[AGA_DIAG_COUNT];
    char     log[LOG_SIZE];
    uint32_t ta[TRACE_LEN], tb[TRACE_LEN], tc[TRACE_LEN], td[TRACE_LEN];
    /* the chipset as the reset found it: with the chip RAM, which the next
       boot leaves alone when this block is valid, it is a snapshot agahost
       replays on the PC (agastat PREVSNAP) */
    uint16_t regs[256];
    uint32_t colors[256];
    uint32_t prof_head;
    uint32_t prof_pc[PROF_LEN];
    uint16_t prof_sr[PROF_LEN], prof_cw[PROF_LEN];
    uint32_t code_base[PROF_CODE];
    uint16_t prof_lag[PROF_LEN];
    uint8_t  code[PROF_CODE][PROF_CODE_BYTES];
};
static volatile struct persist *persist_area;
/* the virtual chip RAM, for SEL_PCHIP and the reset-time cache clean */
static const uint8_t *diag_chip;
static uint32_t diag_chip_size;
static struct persist prev;          /* the session before the last reset */
static int have_prev, view_prev;

static uint32_t persist_sum(const struct persist *p)
{
    const uint32_t *w = (const uint32_t *)&p->log_head;
    uint32_t n = (sizeof *p - 3 * sizeof(uint32_t)) / 4, s = 0x5A5A5A5Au;
    for (uint32_t i = 0; i < n; i++) s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

/* The block is reached through the kernel's cached 1:1 map of RAM: push every
   line of it out to the RAM itself, which is what survives the watchdog. */
static void persist_clean(void)
{
    uintptr_t a = (uintptr_t)persist_area & ~(uintptr_t)63;
    uintptr_t e = (uintptr_t)persist_area + sizeof(struct persist);
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < e; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* start.c, before anything logs: take over whatever the last session left */
void aga_diag_persist_setup(void *area)
{
    persist_area = (volatile struct persist *)area;
    const struct persist *p = (const struct persist *)area;
    if (p->magic == PERSIST_MAGIC && p->size == sizeof *p && p->sum == persist_sum(p)) {
        for (uint32_t i = 0; i < sizeof prev; i++) ((char *)&prev)[i] = ((const char *)area)[i];
        have_prev = 1;
    }
    persist_area->magic = 0;           /* used once: a later reset saves afresh */
    persist_clean();
}

int aga_diag_have_prev(void) { return have_prev; }

void aga_diag_set_chip(const uint8_t *va, uint32_t size) { diag_chip = va; diag_chip_size = size; }

/* the housekeeper, the moment it sees the Amiga's reset line, before it reboots the Pi */
void aga_diag_persist(void)
{
    volatile struct persist *q = persist_area;
    if (!q) return;
    q->magic = 0;
    q->log_head = log_head;
    q->log_wrapped = (uint32_t)log_wrapped;
    q->trace_head = trace_head;
    for (int i = 0; i < AGA_DIAG_COUNT; i++) q->ctr[i] = aga_diag_ctr[i];
    for (int i = 0; i < LOG_SIZE; i++) q->log[i] = log_buf[i];
    for (int i = 0; i < TRACE_LEN; i++) {
        q->ta[i] = trace_a[i]; q->tb[i] = trace_b[i];
        q->tc[i] = trace_c[i]; q->td[i] = trace_d[i];
    }
    aga_glue_snapshot_regs((uint16_t *)q->regs, (uint32_t *)q->colors);
    q->prof_head = prof_head;
    for (int i = 0; i < PROF_LEN; i++) { q->prof_pc[i] = prof_pc[i]; q->prof_sr[i] = prof_sr[i]; q->prof_cw[i] = prof_cw[i]; q->prof_lag[i] = prof_lag[i]; }
    /* the code around the most frequent PCs, straight out of the 68k's RAM
       (fast RAM is physical RAM 1:1, chip RAM is mine); O(n*k), once */
    {
        uint32_t n = prof_head < PROF_LEN ? prof_head : PROF_LEN;
        uint32_t taken[PROF_CODE]; int nt = 0;
        for (int k = 0; k < PROF_CODE; k++) {
            uint32_t best = 0, bestc = 0;
            for (uint32_t i = 0; i < n; i += 16) {     /* candidates: every 16th sample */
                uint32_t pc = prof_pc[i], c = 0, dup = 0;
                for (int t = 0; t < nt; t++) if (pc >= taken[t] && pc < taken[t] + PROF_CODE_BYTES) dup = 1;
                if (dup || !pc) continue;
                for (uint32_t j = 0; j < n; j++)
                    if (prof_pc[j] >= pc - 64 && prof_pc[j] < pc + 64) c++;
                if (c > bestc) { bestc = c; best = pc; }
            }
            if (!bestc) break;
            uint32_t base = (best - PROF_CODE_BYTES / 2) & ~1u;
            taken[nt++] = base;
            q->code_base[k] = base;
            for (int b = 0; b < PROF_CODE_BYTES; b++) {
                uint32_t a = base + (uint32_t)b;
                uint8_t v = 0;
                if (diag_chip && a < diag_chip_size) v = diag_chip[a];
                else if (a >= 0x01000000 && a < 0x3C000000) v = *(volatile uint8_t *)(0xffffff9000000000ULL + a);
                q->code[k][b] = v;
            }
        }
        for (int k = nt; k < PROF_CODE; k++) q->code_base[k] = 0;
    }
    q->size = sizeof(struct persist);
    q->sum = persist_sum((const struct persist *)q);
    q->magic = PERSIST_MAGIC;
    persist_clean();
    /* and the chip RAM, the snapshot's other half: what the program wrote
       last may still sit in the cache, and only the RAM outlives the reboot */
    if (diag_chip) {
        for (uintptr_t a = (uintptr_t)diag_chip & ~(uintptr_t)63; a < (uintptr_t)diag_chip + diag_chip_size; a += 64)
            __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

void aga_diag_freeze_here(void)
{
    if (diag_frozen) return;
    diag_frozen = 1;
    aga_diag_capture_code(aga_diag_hot_pc());
    {   /* the address the last interrupts keep landing on, not merely the
           last one: a stuck game is in one loop and the ring is full of it */
        /* where the last FRAME samples landed - the loop the machine is in
           now, not the one it spent the run in */
        /* the INTREQR poll site if a game was polling one - the loop a
           wedged handler is in - else where the last frame sample fell */
        aga_diag_capture_code2(intreqr_pc ? intreqr_pc
                               : (pc_head ? pc_ring[(pc_head - 1) % PC_RING] : 0));
        uint32_t best = 0, bestn = 0;
        for (int i = 0; i < PCE_RING; i++) {
            uint32_t v = pce[i] & ~255u, n = 0;
            if (!v) continue;
            for (int j = 0; j < PCE_RING; j++) if ((pce[j] & ~255u) == v) n++;
            if (n > bestn) { bestn = n; best = pce[i]; }
        }
        (void)best; (void)bestn;
    }
}

uint32_t aga_diag_hot_pc(void)
{
    uint32_t best = 0, n = 0;
    for (int i = 0; i < PCS_SLOTS; i++)
        if (pcs_hits[i] > n) { n = pcs_hits[i]; best = pcs_addr[i]; }
    return best;
}

#define CODE_WORDS 128
static uint16_t code_buf[CODE_WORDS];
static uint32_t code_base;
/* a second window, taken where the CPU was at the LAST interrupt rather
   than where it spent the most time: a game stuck polling a register sits
   in one loop while its hot bucket is somewhere else entirely */
static uint16_t code2_buf[CODE_WORDS];
static uint32_t code2_base;
void aga_diag_capture_code2(uint32_t pc)
{
    if (pc < 0x00200000 || (pc & 1)) return;
    uint32_t base = pc & ~255u;
    const uint16_t *p = (const uint16_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < CODE_WORDS; i++) code2_buf[i] = p[i];
    code2_base = base;
}
void aga_diag_capture_code(uint32_t pc)
{
    if (pc < 0x00200000 || (pc & 1)) return;    /* chip RAM is not mapped, and a bad PC is not code */
    /* the whole 256-byte bucket, from its base: a window centred on the bucket
       address cut the second half off, and that is where the loop turned back */
    uint32_t base = pc & ~255u;
    const uint16_t *p = (const uint16_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < CODE_WORDS; i++) code_buf[i] = p[i];
    code_base = base;
}

#define SER_RING 2048
static char ser_buf[SER_RING];
static uint32_t ser_head, ser_wrapped;
void aga_diag_serial(uint8_t c)
{
    ser_buf[ser_head] = (char)c;
    if (++ser_head >= SER_RING) { ser_head = 0; ser_wrapped = 1; }
}

uint32_t aga_diag_serial_count(void)
{
    return ser_wrapped ? SER_RING : ser_head;
}

uint8_t aga_diag_serial_get(uint32_t i)
{
    uint32_t n = ser_wrapped ? SER_RING : ser_head;
    if (i >= n) return 0;
    uint32_t start = ser_wrapped ? ser_head : 0;
    return (uint8_t)ser_buf[(start + i) % SER_RING];
}

void aga_diag_reg_reset(void)
{
    for (int i = 0; i < PCH_SLOTS; i++) { pch_addr[i] = 0; pch_hits[i] = 0; }
    for (int i = 0; i < 16; i++) cia_a[i] = cia_b[i] = 0;
    for (int i = 0; i < 256; i++) bpra_rd[i] = bpra_wr[i] = 0;
    cia_head = cia_wrapped = 0;
    for (int i = 0; i < 256; i++) reg_reads[i] = reg_writes[i] = 0;
    for (int i = 0; i < 64; i++) beam_val[i] = 0;
}
static uint32_t trap_buf[TRAP_RING];   /* write<<31 | reg<<16 | value */
static uint32_t trap_pc[TRAP_RING];    /* 68k PC at the access */
static uint32_t trap_head;
static int      trap_wrapped;

void aga_diag_trap(uint32_t reg, uint16_t val, int write, uint32_t pc)
{
    if (diag_frozen) return;
    trap_buf[trap_head] = ((uint32_t)(write != 0) << 31) | ((reg & 0x1FF) << 16) | val;
    trap_pc[trap_head]  = pc;
    if (++trap_head >= TRAP_RING) { trap_head = 0; trap_wrapped = 1; }
}

/* ---- display state --------------------------------------------------------
   The counters say how hard the machine is working and the trap ring says what
   it touched in the last fraction of a millisecond. Neither answers the
   question you actually want when a game shows a blank screen: did it ever set
   a display up? BPLCON0 with a plane count, sane DIW/DDF and non-zero bitplane
   pointers say yes, and the fault is in my renderer. All zeros say the game
   never got that far, and my renderer is innocent. For once. */
uint16_t aga_glue_peek_reg(uint32_t reg);      /* aga_glue.c */
uint32_t aga_glue_peek_bplpt(int plane);
uint16_t aga_glue_peek_state(int which);       /* 0 DMACON, 1 INTENA, 2 INTREQ */

static const uint16_t regs_of_interest[] = {
    0x100, 0x102, 0x104, 0x106, 0x10C, 0x1FC,      /* BPLCON0-3, BPLCON4, FMODE */
    0x08E, 0x090, 0x1E4, 0x092, 0x094,             /* DIWSTRT/STOP, DIWHIGH, DDFSTRT/STOP */
    0x108, 0x10A,                                  /* BPL1MOD, BPL2MOD */
    0x1DC, 0x1C0, 0x1C8, 0x1CC,                    /* BEAMCON0, HTOTAL, VTOTAL, VBSTRT */
    0x080, 0x082, 0x084, 0x086,                    /* COP1LCH/L, COP2LCH/L */
};
#define REGS_OF_INTEREST (sizeof regs_of_interest / sizeof regs_of_interest[0])

/* High-water snapshot: the display state the last time anything actually had
   bitplanes enabled.

   A game that wedges has to be reset, and a reset runs aga_reset(), which
   wipes the whole chipset struct - so by the time the machine is reachable
   again the live registers read all zeros and say nothing. This lives in ARM
   memory outside aga_t, survives a warm reset and answers the one question
   that matters after a crash: did the game EVER set a display up, and what
   did it look like? Only a power cycle clears it. */
static uint16_t peak_regs[REGS_OF_INTEREST];
static uint16_t peak_state[3];
static uint32_t peak_bpl[8];
/* And the DEEPEST display seen: the frame with the most bitplanes. The
   high-water snapshot is the LAST display, and after a WHDLoad game quits
   that is the OS's one-plane blank view - it had overwritten Banshee's
   six-plane in-game setup before that could be read. Bastard. */
static int      deep_planes;
static uint16_t deep_regs[REGS_OF_INTEREST];
static uint16_t deep_state[3];
static uint32_t deep_bpl[8];

void aga_diag_snapshot(int had_data)
{
    uint16_t bplcon0 = aga_glue_peek_reg(0x100);
    int planes = ((bplcon0 >> 12) & 7) | ((bplcon0 & 0x0010) ? 8 : 0);
    if (diag_frozen) return;
    /* The pathology: a game display is up (four planes or more) and there is
       damn all in it - every bitplane fetch this frame returned zero. Freeze
       after a full second of that, and zero the traffic counters five frames
       in, so what is left describes that second and nothing before it. An
       intro that renders always has data, so this never fires there. */
    if (planes >= 4 && !had_data) {
        blank_streak++;
        if (blank_streak == 150) {          /* 3 s in: past the setup burst */
            aga_diag_ctr[AGA_D_CHIP_R] = aga_diag_ctr[AGA_D_CHIP_W] = 0;
            for (int i = 0; i < 8; i++) aga_diag_ctr[AGA_D_CHIPW_B0 + i] = 0;
            aga_diag_ctr[AGA_D_BLITS] = 0;
            aga_diag_ctr[AGA_D_TRAPS_R] = aga_diag_ctr[AGA_D_TRAPS_W] = 0;
            aga_diag_reg_reset();
            aga_diag_beam_reset();   /* coverage from here on describes the hang */
            aga_diag_log("[AGA] display up but empty: counters zeroed, watching");
        }
        if (blank_streak >= 400) {         /* 8 s in: the steady state */
            diag_frozen = 1;
            aga_diag_capture_code(aga_diag_hot_pc());
            aga_diag_log("[AGA] FROZEN: one second of display with no data in it");
        }
    } else {
        blank_streak = 0;
    }
    pc_ring[pc_head % PC_RING] = emu68_get_pc();
    pc_head++;
    if (!planes) return;
    aga_diag_ctr[AGA_D_BPL_FRAMES]++;
    for (unsigned i = 0; i < REGS_OF_INTEREST; i++) peak_regs[i] = aga_glue_peek_reg(regs_of_interest[i]);
    for (int i = 0; i < 3; i++) peak_state[i] = aga_glue_peek_state(i);
    for (int i = 0; i < 8; i++) peak_bpl[i] = aga_glue_peek_bplpt(i);
    if (planes >= deep_planes) {
        deep_planes = planes;
        for (unsigned i = 0; i < REGS_OF_INTEREST; i++) deep_regs[i] = peak_regs[i];
        for (int i = 0; i < 3; i++) deep_state[i] = peak_state[i];
        for (int i = 0; i < 8; i++) deep_bpl[i] = peak_bpl[i];
    }
}

/* ---- read interface -------------------------------------------------------- */
#define SEL_LOG   0x100
#define SEL_MAGIC 0x200      /* so a tool can tell my kernel from a stock one */
#define SEL_TRAPS 0x300      /* the trap ring, 2 words per entry, oldest first */
#define SEL_REGS  0x400      /* virtual chipset display state */
#define SEL_DEEP  0x500
#define SEL_PCS   0x600      /* frozen flag, then PC_RING 68k PCs, oldest first */
#define SEL_HIST  0x700      /* 256 read counts, 256 write counts, 64 beam-value buckets */
#define SEL_PCE   0xB00      /* the last PCE_RING exact interrupt PCs, oldest first */
#define SEL_WAVE  0xC00      /* the last WAVE_LEN stereo byte pairs sent to the DAC */
#define SEL_TRIG  0xD00      /* the last 32 audio channel starts, 6 longs each */
#define SEL_ENV   0xE00      /* per-channel loudness, one byte per channel per frame */
#define SEL_WRAP  0xF00      /* the last sampled loop-backs: chan, lc, len, peak */
#define SEL_BEAM  0x1100     /* the last composed beam values a raster wait saw */
#define SEL_CODE2 0x1200     /* 68k code where the last interrupt found the CPU */
#define SEL_TRACE 0x1300     /* the last 64 frames: what the 68k was doing in each */
#define SEL_PREV_Q   0x1600  /* read: 1 if the session before the last reset was kept */
#define SEL_PREV_ON  0x1601  /* counters, log and trace now read the kept session */
#define SEL_PREV_OFF 0x1602  /* ...and back to this one */
#define SEL_CTR_HI   0x1700  /* counters 256 and up: $1700 + (n - 256) */
#define SEL_PCHIP    0x1800  /* the virtual chip RAM, word by word from address 0 */
#define SEL_PPROF    0x1802  /* the kept profile: head (2 words), PROF_LEN x (pc hi, pc lo,
                                sr, chip writes), then PROF_CODE x (base hi, base lo,
                                PROF_CODE_BYTES / 2 words) */
#define SEL_PREGS    0x1801  /* the kept session's registers (256 words), then its
                                palette (256 longs, high word first) */
#define SEL_AUDW  0x1000     /* count, then the sound-register writes: 3 longs each */

static uint32_t wrapi_buf[32 * 4];
static int      wrapi_count;
void aga_diag_wrapinfo(const uint32_t *v, int n)
{
    wrapi_count = n;
    for (int i = 0; i < n * 4; i++) wrapi_buf[i] = v[i];
}

/* WHICH channel is making a sound, WHEN.
 *
 * Cumulative energy says all four channels are comparably busy over a whole
 * run, while the recording shows one side silent for seconds at a time. Both
 * cannot be true, and neither number can be lined up against the other because
 * one is a total and the other is a moment. This is the missing view: fifteen
 * seconds of per-channel loudness at frame rate, so a channel that goes quiet
 * right when a sound effect should have played is visible at a glance.
 *
 * 750 frames x 4 channels x one byte = 3 KB. */
#define ENV_FRAMES 750
static uint8_t env_buf[ENV_FRAMES][4];
static uint32_t env_head, env_wrapped;
static int env_frozen;
void aga_diag_env(const uint32_t *nrg_delta)
{
    if (env_frozen) return;
    for (int n = 0; n < 4; n++) {
        uint32_t v = nrg_delta[n] >> 6;     /* frame energy -> 0..255ish */
        env_buf[env_head][n] = v > 255 ? 255 : (uint8_t)v;
    }
    if (++env_head >= ENV_FRAMES) { env_head = 0; env_wrapped = 1; }
}
void aga_diag_env_freeze(int on)
{
    env_frozen = on;
    if (!on) { env_head = 0; env_wrapped = 0; }
}

/* Filled once a frame by the glue from the core's trigger ring. */
static uint32_t trig_buf[32 * 6];
static int      trig_count;
void aga_diag_triggers(const uint32_t *v, int n)
{
    trig_count = n;
    for (int i = 0; i < n * 6; i++) trig_buf[i] = v[i];
}

/* Every write the game makes to the sound registers, in order.
 *
 * The A1200 recording settled WHAT is wrong: real hardware plays about four
 * effects a second in Banshee's gameplay, a stream of shots plus explosions,
 * and this thing plays one - no shots at all. The counters cannot say WHY,
 * because they are totals. Channel 2 is triggered twice a second with a
 * full-amplitude sample and yet is silent in every frame, while the same run
 * counted 2354 non-zero volume writes to it. Both are true only in some
 * particular ORDER of writes, and only the order itself can show which.
 *
 * AUDx*, DMACON when it touches an audio bit or DMAEN, INTREQ and INTENA when
 * they touch an audio bit. Each entry: size<<28 | frame, vpos<<20 | hpos<<10 |
 * register, value. 4096 entries is several seconds of a busy sound driver,
 * frozen with the wave capture when the game exits. */
#define AUDW_RING 4096
static uint32_t audw_buf[AUDW_RING][3];
static uint32_t audw_head, audw_wrapped;
static int audw_frozen;
void aga_diag_audw(uint32_t reg, int size, uint32_t value, uint32_t vpos, uint32_t hpos)
{
    if (audw_frozen) return;
    uint32_t *e = audw_buf[audw_head];
    e[0] = ((uint32_t)size << 28) | (aga_diag_ctr[AGA_D_FRAMES] & 0x0FFFFFFF);
    e[1] = ((vpos & 0x1FF) << 20) | ((hpos & 0xFF) << 10) | (reg & 0x1FF);
    e[2] = value;
    if (++audw_head >= AUDW_RING) { audw_head = 0; audw_wrapped = 1; }
}

/* The exact bytes handed to Paula.
 *
 * Every counter in this file says the audio pipeline is clean - the clock is
 * PAL to 0.06%, there are no loop resyncs, no ring restarts, and 38 sample
 * corrections in two minutes - and the machine still sounds wrong. When the
 * instruments and the ears disagree this persistently, the instruments are
 * measuring the wrong thing, and the only way to settle it is to look at the
 * signal itself rather than statistics about it.
 *
 * Fifteen seconds at 27928 Hz, stereo, 8 bit: what the DAC actually plays.
 * Dumped to a file and turned into a WAV on the PC, so it can be listened to.
 * Long enough to hear how SPARSE the effects are during play, which a
 * half-second window around a single effect could never show - and long
 * enough that it does not have to be caught by quitting at the right moment.
 *
 * Recorded at a QUARTER of the output rate - 6982 Hz - which keeps fifteen
 * seconds inside 205 KB and, just as importantly, cuts the write rate on the
 * chipset core from 28000 a second to 7000.
 *
 * The full-rate version was 840 KB and stopped Banshee reaching gameplay,
 * blits back at zero: streaming that much through the cache slows core 3, and
 * a slow chipset loop delays the blit completion the game is waiting on. A
 * diagnostic that changes what it measures is a piece of shit.
 *
 * 3.5 kHz of bandwidth is dull, but the question being asked is WHEN sounds
 * happen and how often - not what they sound like. */
#define WAVE_DECIMATE 4
#define WAVE_LEN 105000
static uint8_t wave_l[WAVE_LEN], wave_r[WAVE_LEN];
static uint32_t wave_head, wave_wrapped;
/* Frozen when the sandbox is left, so the last fifteen seconds OF THE GAME
   survive whatever happens after, and cleared when it is entered again. */
static int wave_frozen;
void aga_diag_wave(uint8_t l, uint8_t r)
{
    static uint32_t skip;
    if (wave_frozen) return;
    if (++skip < WAVE_DECIMATE) return;
    skip = 0;
    wave_l[wave_head] = l;
    wave_r[wave_head] = r;
    if (++wave_head >= WAVE_LEN) { wave_head = 0; wave_wrapped = 1; }
}
void aga_diag_wave_freeze(int on)
{
    wave_frozen = on;
    audw_frozen = on;          /* the register writes behind that same audio */
    if (!on) { wave_head = 0; wave_wrapped = 0; audw_head = 0; audw_wrapped = 0; }
    else aga_diag_log("[AGA] audio: the last fifteen seconds of the game captured");
}
#define SEL_PROF  0xA00      /* PCS_SLOTS pairs: bucket address, sample count */
#define SEL_CODE  0x900      /* base address, then CODE_WORDS words of 68k code at the stuck PC */
#define SEL_SER   0x800      /* count, then that many characters WHDLoad transmitted */      /* the deepest display seen: planes, nr, regs, 3 state, 8 bpl ptrs */

static uint32_t sel;                 /* current selection */
static uint32_t pos;                 /* words already read from it */

void aga_diag_select(uint16_t v)
{
    if (v == SEL_PREV_ON)  { view_prev = have_prev; }
    if (v == SEL_PREV_OFF) { view_prev = 0; }

    sel = v;
    pos = 0;
}

uint16_t aga_diag_read_word(void)
{
    if (sel == SEL_PCHIP) {
        /* after a soft reset this is still the chip RAM the program left:
           start.c does not clear it when the kept session is valid, and the
           OS never touches it until the next sandbox entry copies over it */
        uint32_t a = pos * 2;
        pos++;
        if (!diag_chip || a + 2 > diag_chip_size) return 0;
        return (uint16_t)((diag_chip[a] << 8) | diag_chip[a + 1]);
    }
    if (sel == SEL_PPROF) {
        uint32_t i = pos++;
        if (!have_prev) return 0;
        if (i < 2) return (uint16_t)(i ? prev.prof_head : prev.prof_head >> 16);
        i -= 2;
        if (i < PROF_LEN * 4) {
            uint32_t e = i >> 2, w = i & 3;
            return (uint16_t)(w == 0 ? prev.prof_pc[e] >> 16 : w == 1 ? prev.prof_pc[e] : w == 2 ? prev.prof_sr[e] : prev.prof_cw[e]);
        }
        i -= PROF_LEN * 4;
        uint32_t per = 2 + PROF_CODE_BYTES / 2, k = i / per, w = i % per;
        if (k >= PROF_CODE) {                /* then the lateness of each frame, ms */
            uint32_t j = i - PROF_CODE * per;
            return j < PROF_LEN ? prev.prof_lag[j] : 0;
        }
        if (w == 0) return (uint16_t)(prev.code_base[k] >> 16);
        if (w == 1) return (uint16_t)prev.code_base[k];
        w -= 2;
        return (uint16_t)((prev.code[k][w * 2] << 8) | prev.code[k][w * 2 + 1]);
    }
    if (sel == SEL_PREGS) {
        uint32_t i = pos++;
        if (!have_prev) return 0;
        if (i < 256) return prev.regs[i];
        i -= 256;
        if (i < 512) return (uint16_t)((i & 1) ? prev.colors[i >> 1] : (prev.colors[i >> 1] >> 16));
        return 0;
    }
    if (sel == SEL_LOG) {
        /* oldest first: start at the head when the ring has wrapped */
        const char *lb = view_prev ? prev.log : log_buf;
        uint32_t lh = view_prev ? prev.log_head : log_head;
        int lw = view_prev ? (int)prev.log_wrapped : log_wrapped;
        uint32_t start = lw ? lh : 0;
        uint32_t len   = lw ? LOG_SIZE : lh;
        uint16_t w = 0;
        for (int b = 0; b < 2; b++) {
            uint32_t i = pos * 2 + b;
            char c = (i < len) ? lb[(start + i) % LOG_SIZE] : 0;
            w = (uint16_t)((w << 8) | (uint8_t)c);
        }
        pos++;
        return w;
    }
    if (sel == SEL_TRAPS) {
        /* word 0 is the entry count, then 4 words per entry, oldest first:
           meta high, meta low, PC high, PC low */
        uint32_t start = trap_wrapped ? trap_head : 0;
        uint32_t len   = trap_wrapped ? TRAP_RING : trap_head;
        if (pos == 0) { pos++; return (uint16_t)len; }
        uint32_t i = pos - 1, e = i >> 2, w = i & 3;
        if (e >= len) { pos++; return 0; }
        uint32_t idx = (start + e) % TRAP_RING;
        uint32_t v = (w < 2) ? trap_buf[idx] : trap_pc[idx];
        pos++;
        return (uint16_t)((w & 1) ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_WRAP) {
        if (pos == 0) { pos++; return (uint16_t)wrapi_count; }
        uint32_t i = (pos - 1) >> 1, w = (pos - 1) & 1;
        pos++;
        if (i >= (uint32_t)wrapi_count * 4) return 0;
        return (uint16_t)(w ? (wrapi_buf[i] & 0xFFFF) : (wrapi_buf[i] >> 16));
    }
    if (sel == SEL_ENV) {
        uint32_t n = env_wrapped ? ENV_FRAMES : env_head;
        if (pos == 0) { pos++; return (uint16_t)n; }
        uint32_t i = (pos - 1) >> 1, w = (pos - 1) & 1;
        pos++;
        if (i >= n) return 0;
        uint32_t start = env_wrapped ? env_head : 0;
        const uint8_t *e = env_buf[(start + i) % ENV_FRAMES];
        return (uint16_t)(w ? ((e[2] << 8) | e[3]) : ((e[0] << 8) | e[1]));
    }
    if (sel == SEL_AUDW) {
        /* count, then 3 longs per entry, high word first, oldest entry first */
        uint32_t n = audw_wrapped ? AUDW_RING : audw_head;
        if (pos == 0) { pos++; return (uint16_t)n; }
        uint32_t i = (pos - 1) >> 1, w = (pos - 1) & 1;
        pos++;
        uint32_t e = i / 3, k = i % 3;
        if (e >= n) return 0;
        uint32_t start = audw_wrapped ? audw_head : 0;
        uint32_t v = audw_buf[(start + e) % AUDW_RING][k];
        return (uint16_t)(w ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_TRIG) {
        /* count, then 6 longs per entry, high word first */
        if (pos == 0) { pos++; return (uint16_t)trig_count; }
        uint32_t i = (pos - 1) >> 1, w = (pos - 1) & 1;
        pos++;
        if (i >= (uint32_t)trig_count * 6) return 0;
        return (uint16_t)(w ? (trig_buf[i] & 0xFFFF) : (trig_buf[i] >> 16));
    }
    if (sel == SEL_WAVE) {
        /* count, then one word per frame: left in the high byte, right in the low */
        uint32_t n = wave_wrapped ? WAVE_LEN : wave_head;
        if (pos == 0) { pos++; return (uint16_t)(n >> 16); }
        if (pos == 1) { pos++; return (uint16_t)(n & 0xFFFF); }
        uint32_t i = pos - 2;
        pos++;
        if (i >= n) return 0;
        uint32_t start = wave_wrapped ? wave_head : 0;
        uint32_t k = (start + i) % WAVE_LEN;
        return (uint16_t)(((uint16_t)wave_l[k] << 8) | wave_r[k]);
    }
    if (sel == SEL_PROF) {
        /* four words per slot: addr high, addr low, hits high, hits low */
        uint32_t i = pos >> 2, w = pos & 3;
        pos++;
        if (i >= PCS_SLOTS) return 0;
        uint32_t v = (w < 2) ? pcs_addr[i] : pcs_hits[i];
        return (uint16_t)((w & 1) ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_BEAM) {
        uint32_t i = pos >> 1, w = pos & 1;
        pos++;
        if (i >= BEAMSEQ_RING + 33) return 0;
        if (i >= BEAMSEQ_RING) {          /* then the coverage map, then the bits seen */
            uint32_t j = i - BEAMSEQ_RING;
            if (j < 32) return (uint16_t)(w ? (beam_cov[j] & 0xFFFF) : (beam_cov[j] >> 16));
            return w ? beam_and : beam_or;
        }
        uint32_t n = beamseq_head < BEAMSEQ_RING ? 0 : beamseq_head;
        uint32_t v = beamseq[(n + i) % BEAMSEQ_RING];
        return (uint16_t)(w ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_PCE) {
        uint32_t i = pos >> 1, w = pos & 1;
        pos++;
        if (i >= PCE_RING) return 0;
        uint32_t n = pce_head < PCE_RING ? 0 : pce_head;
        uint32_t v = pce[(n + i) % PCE_RING];
        return (uint16_t)(w ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_TRACE) {
        uint32_t i = pos >> 3, w = pos & 7;
        pos++;
        if (i >= TRACE_LEN) return 0;
        uint32_t th = view_prev ? prev.trace_head : trace_head;
        uint32_t n = th < TRACE_LEN ? 0 : th;
        uint32_t k = (n + i) % TRACE_LEN;
        uint32_t v = view_prev
            ? ((w < 2) ? prev.ta[k] : (w < 4) ? prev.tb[k] : (w < 6) ? prev.tc[k] : prev.td[k])
            : ((w < 2) ? trace_a[k] : (w < 4) ? trace_b[k] : (w < 6) ? trace_c[k] : trace_d[k]);
        return (uint16_t)((w & 1) ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_CODE2) {
        if (pos == 0) { pos++; return (uint16_t)(code2_base >> 16); }
        if (pos == 1) { pos++; return (uint16_t)(code2_base & 0xFFFF); }
        uint32_t i = pos - 2;
        pos++;
        return (i < CODE_WORDS) ? code2_buf[i] : 0;
    }
    if (sel == SEL_CODE) {
        /* address first (high word, low word), then the words themselves */
        if (pos == 0) { pos++; return (uint16_t)(code_base >> 16); }
        if (pos == 1) { pos++; return (uint16_t)(code_base & 0xFFFF); }
        uint32_t i = pos - 2;
        pos++;
        return (i < CODE_WORDS) ? code_buf[i] : 0;
    }
    if (sel == SEL_SER) {
        uint32_t n = ser_wrapped ? SER_RING : ser_head;
        if (pos == 0) { pos++; return (uint16_t)n; }
        uint32_t i = pos - 1;
        pos++;
        if (i >= n) return 0;
        uint32_t start = ser_wrapped ? ser_head : 0;
        return (uint16_t)(uint8_t)ser_buf[(start + i) % SER_RING];
    }
    if (sel == SEL_HIST) {
        /* 32 bits per entry, high word first: 256 reads, 256 writes, 64 values */
        uint32_t i = pos >> 1, w = pos & 1;
        pos++;
        uint32_t v = (i < 256) ? reg_reads[i]
                   : (i < 512) ? reg_writes[i - 256]
                   : (i < 576) ? beam_val[i - 512]
                   : (i < 592) ? cia_a[i - 576]
                   : (i < 608) ? cia_b[i - 592]
                   : (i < 864) ? bpra_rd[i - 608]
                   : (i < 1120) ? bpra_wr[i - 864] : 0;
        if (i >= 1120 + CIA_RING && i < 1120 + CIA_RING + 2 * PCH_SLOTS) {
            uint32_t e = (i - 1120 - CIA_RING) >> 1;
            uint32_t v = ((i - 1120 - CIA_RING) & 1) ? pch_hits[e] : pch_addr[e];
            return (uint16_t)(w ? (v & 0xFFFF) : (v >> 16));
        }
        if (i >= 1120 && i < 1120 + CIA_RING) {
            /* the ring, oldest first; one word per entry so only the low half */
            uint32_t n = cia_wrapped ? CIA_RING : cia_head;
            uint32_t e = i - 1120;
            if (e >= n) return 0;
            uint32_t start = cia_wrapped ? cia_head : 0;
            return w ? cia_ring[(start + e) % CIA_RING] : 0;
        }
        return (uint16_t)(w ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_PCS) {
        if (pos == 0) { pos++; return (uint16_t)((diag_frozen ? 0x8000 : 0) | PC_RING); }
        uint32_t i = pos - 1, e = i >> 1;
        pos++;
        if (e >= PC_RING) return 0;
        uint32_t start = (pc_head > PC_RING) ? (pc_head - PC_RING) : 0;
        uint32_t v = pc_ring[(start + e) % PC_RING];
        return (uint16_t)((i & 1) ? (v & 0xFFFF) : (v >> 16));
    }
    if (sel == SEL_DEEP) {
        uint32_t nr = REGS_OF_INTEREST;
        if (pos == 0) { pos++; return (uint16_t)deep_planes; }
        if (pos == 1) { pos++; return (uint16_t)nr; }
        uint32_t i = pos - 2;
        pos++;
        if (i < nr) return deep_regs[i];
        i -= nr;
        if (i < 3) return deep_state[i];
        i -= 3;
        if (i < 16) { uint32_t p = deep_bpl[i >> 1]; return (uint16_t)((i & 1) ? (p & 0xFFFF) : (p >> 16)); }
        return 0;
    }
    if (sel == SEL_REGS) {
        /* word 0: how many register words follow. Then that many values, in the
           order of regs_of_interest; then 3 state words (DMACON, INTENA,
           INTREQ - the accumulated state, not the last write); then 8 bitplane
           pointers as 2 words each, high first. */
        uint32_t nr = REGS_OF_INTEREST;
        if (pos == 0) { pos++; return (uint16_t)nr; }
        uint32_t i = pos - 1;
        pos++;
        if (i < nr) return aga_glue_peek_reg(regs_of_interest[i]);
        i -= nr;
        if (i < 3) return aga_glue_peek_state((int)i);
        i -= 3;
        if (i < 16) {
            uint32_t p = aga_glue_peek_bplpt((int)(i >> 1));
            return (uint16_t)((i & 1) ? (p & 0xFFFF) : (p >> 16));
        }
        i -= 16;
        /* then the same layout again from the high-water snapshot, which
           survives the reset a wedged game forces */
        if (i < nr) return peak_regs[i];
        i -= nr;
        if (i < 3) return peak_state[i];
        i -= 3;
        if (i < 16) return (uint16_t)((i & 1) ? (peak_bpl[i >> 1] & 0xFFFF) : (peak_bpl[i >> 1] >> 16));
        return 0;
    }
    if (sel == SEL_MAGIC) {
        /* Every dump starts here, so the live pointer probe runs here: a tool
           that reads the counters gets the pointer as it is on screen at that
           moment, not as it was when a game last exited. */
        if (pos == 0) aga_video_probe_pointer();
        /* "AGAD" split in two, then the counter count so a tool knows how far to read */
        uint16_t w = (pos == 0) ? 0xA6A1 : (pos == 1) ? 0xD1A6 : (uint16_t)AGA_DIAG_COUNT;
        pos++;
        return w;
    }
    if (sel == SEL_PREV_Q) { pos++; return (uint16_t)have_prev; }
    /* Counter n is selector n - but only below 256: $100 is already the log.
       Counters from 256 up answer at SEL_CTR_HI + (n - 256). */
    uint32_t idx = (sel < 0x100) ? sel
                 : (sel >= SEL_CTR_HI && sel < SEL_CTR_HI + 0x100) ? 0x100 + (sel - SEL_CTR_HI)
                 : 0xFFFFFFFFu;
    if (idx < AGA_DIAG_COUNT) {
        uint32_t v = view_prev ? prev.ctr[idx] : aga_diag_ctr[idx];
        uint16_t w = (pos == 0) ? (uint16_t)(v >> 16) : (uint16_t)v;
        pos++;
        return w;
    }
    return 0;
}

/* Called once at startup so a log dump always identifies the build. */
void aga_diag_init(void)
{
    aga_diag_log("[AGA] diagnostics ready, " __DATE__ " " __TIME__
#ifdef AGA_DIAG
                 " (AGA_DIAG build: per-trap counters on, slower)"
#else
                 " (shipped build: frame-granularity counters only)"
#endif
                );
    kprintf("[AGA] diagnostics: write $DFF1E8 = selector, read $DFF1EA\n");
    if (have_prev)
        aga_diag_log("[AGA] the session before the reset was kept: agastat PREV");
    /* which Pi this kernel is for and which one it found - agaboot reads
       these two by index, so the indices must never move */
    _Static_assert(AGA_D_BUILD_BOARD == 147 && AGA_D_RUN_BOARD == 148, "agaboot reads these by index");
    _Static_assert(AGA_D_BUSTEST_MODE == 470 && AGA_D_BUSTEST_WORST_NS == 473, "agaboot BUSTEST reads these by index");
    _Static_assert(AGA_D_HDMI_STATE == 490, "agaboot SOUND reads it by index");
    _Static_assert(AGA_D_HDMI_FILTER == 502 && AGA_D_AUDIO_MIX == 503 && AGA_D_CROP == 504 && AGA_D_AUTO_Y == 506
                   && AGA_D_HDMI_SYNC == 507 && AGA_D_HDMI_SYNC_PV == 510 && AGA_D_AHI == 511,
                   "agastat and agaboot (picture.c) find them by their place");
    _Static_assert(AGA_DIAG_COUNT <= 512, "agaboot and agastat read 512 counters at most");
    aga_diag_set(AGA_D_BUILD_BOARD, AGA_PI3_BUILD ? 3 : 4);
    aga_diag_set(AGA_D_RUN_BOARD, aga_is_pi3() ? 3 : 4);
    if (AGA_PI3_BUILD != aga_is_pi3())
        aga_diag_log(AGA_PI3_BUILD
            ? "[AGA] Pi 3 kernel on a Pi 4: working as a Pi 4 kernel, reinstall for the Pi 4"
            : "[AGA] Pi 4 kernel on a Pi 3: no AGA picture, reinstall for the Pi 3");
}

const char *aga_diag_name(int idx)
{
    return ((unsigned)idx < AGA_DIAG_COUNT) ? ctr_name[idx] : "?";
}
