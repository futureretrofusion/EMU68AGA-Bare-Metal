/* AGA-PISTORM - Copper coprocessor.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Model: one memory access per two colour clocks. MOVE = 2 accesses, WAIT and
 * SKIP = 2 accesses plus a 2-cycle compare delay. Beam comparisons use the
 * position of the *next* copper cycle, like the hardware. A cycle already
 * taken by bitplane DMA is not available to the copper.
 *
 * The classic UAE copper - update_copper and its states, as kept in E-UAE's
 * custom.c - translated to C: the states and fields, reading, WAIT, SKIP,
 * the blitter wait and the idle test. The WAIT/SKIP compare values and the
 * beam compare are as in WinUAE custom.cpp (coppercomp). The cycle timing,
 * and what a COPJMP written by the copper does, were measured from WinUAE's
 * DMA records; a restart or a CPU COPJMP takes its pointer at the first
 * fetch, as WinUAE's copper does (getstrobecopip), in my own code.
 *   E-UAE custom.c:    Copyright 1995-2002 Bernd Schmidt,
 *                      Copyright 1995 Alessandro Bissacco,
 *                      Copyright 2000-2004 Toni Wilen
 *   WinUAE custom.cpp: Copyright 1995-2002 Bernd Schmidt,
 *                      Copyright 1995 Alessandro Bissacco,
 *                      Copyright 2000-2025 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include "internal.h"

void cop_reset(aga_t *a)
{
    memset(&a->cop, 0, sizeof a->cop);
    a->cop.state = COP_stop;
}

void cop_write_lc(aga_t *a, int which, int high, uint16_t v)
{
    uint32_t *lc = (which == 1) ? &a->cop.cop1lc : &a->cop.cop2lc;
    if (high) *lc = (*lc & 0x0000FFFF) | ((uint32_t)(v & 0x01FF) << 16);
    else      *lc = (*lc & 0xFFFF0000) | (v & 0xFFFE);
}

void cop_strobe(aga_t *a, int which)
{
    copper_t *c = &a->cop;
    c->ignore_next = 0;
    if (c->in_move) {
        /* Written by the copper itself: the hardware has already fetched the
           next instruction word, so exactly one more instruction from the old
           list is executed before the jump takes effect (WinUAE records:
           Castlevania line 52 and Turrican 2 line 194 both show COPJMP2, then
           one more instruction read from the old list, then the new list).
           Executing that instruction breaks every list that ends in a WAIT
           terminator after the jump, so it is fetched and discarded. */
        c->strobe = which;
        c->ignore_next = 1;       /* that instruction is fetched but not executed */
        return;
    }
    /* Written by the CPU: takes effect at once, after one idle cycle. The
       pointer itself is read at the first fetch (c->reload), as in WinUAE,
       which matters when copper DMA is off - see cop_vsync. */
    c->pc = (which == 1) ? c->cop1lc : c->cop2lc;
    c->reload = which;
    c->state = COP_strobe_delay;
    c->strobe = 0;
}

void cop_vsync(aga_t *a)
{
    /* Vertical blank restarts the copper at COP1LC. */
    if (a->cop.cop1lc != a->dbg_coprs_last) {   /* diagnostics: a new starting point */
        uint32_t k = a->dbg_coprs_n++ & 7;
        a->dbg_coprs_addr[k] = a->cop.cop1lc;
        a->dbg_coprs_frame[k] = (uint32_t)a->frame;
        a->dbg_coprs_last = a->cop.cop1lc;
    }
    aga_dbg_event(a, 8, (uint16_t)a->cop.cop1lc, 0);
    a->cop.strobe = 0;
    /* The restart is a strobe: COP1LC is read when the copper next FETCHES,
       not now (WinUAE COPJMP(1, true) only sets startstrobe; the pointer comes
       from getstrobecopip() when the first copper DMA cycle happens). With
       copper DMA on that is two colour clocks later and changes nothing. With
       it off, the fetch waits for DMA to come back - and by then a program
       may have pointed COP1LC somewhere else. OutRun AGA (system-wide) turns
       all DMA off, writes BPLCON0 $0211, points COP1LC at its own list and
       turns DMA on again. Latching the pointer here kept the OS copinit
       from the vertical blank before, and copinit's second MOVE (BPLCON0
       $0200) ran the moment DMA returned: no bitplane ever, copper road and
       sky only. */
    a->cop.pc = a->cop.cop1lc;
    a->cop.reload = 1;
    a->cop.state = COP_read1;
    a->cop.ignore_next = 0;
    a->cop.delay = 2;
}

void cop_start_line(aga_t *a)
{
    /* The first cycle a line offers the copper is 2, not 0: WinUAE DMA records
       show copper accesses at 2,4,..,226 (113 per line), never at 0. Starting
       at 0 gave the copper one extra access per line, which showed up as a
       drift of one instruction word per line whenever a long copper run was
       not interrupted by a WAIT (see docs/VALIDATION-FINDINGS.md). */
    a->cop.last_hpos = 1;
}

void cop_blitter_done(aga_t *a)
{
    if (a->cop.state == COP_bltwait) a->cop.state = COP_read1;
}

static int cop_reg_allowed(const aga_t *a, uint32_t reg)
{
    if (a->cop.danger) return 1;              /* AGA + CDANG: everything */
    return reg >= 0x80;
}

static inline int beam_reached(const aga_t *a, int hpos)
{
    int vp = a->vpos & a->cop.vmask;
    int hp = hpos & a->cop.hmask;
    if (vp > a->cop.vcmp) return 1;
    if (vp == a->cop.vcmp && hp >= a->cop.hcmp) return 1;
    return 0;
}

/* Where a WAIT on this line wakes: the first even colour clock from c_hpos
   whose cycle before reaches the compare (past the line's end if none
   does). Until then every copper slot only compares the beam and finds it
   short; cck_step counts those slots as idle instead of calling cop_sync for
   each, as it does for a WAIT on a later line. Nothing moves the copper on in
   the meantime but a strobe or a new WAIT, which leave COP_wait or forget the
   wake; it holds for this pass of this line only (line_no never repeats, and
   a beam set back before wake_from finds it again). RGB2RTG 0.75 (by the same
   author): Jim Power's copper waits for a place on nearly every line. */
static void cop_wait_wake(aga_t *a, int c_hpos)
{
    copper_t *c = &a->cop;
    int h = c_hpos;
    while (h <= a->maxhpos + 1 && !(h - 2 >= 0 && beam_reached(a, h - 2)))
        h += 2;
    c->wake_ok = 1;
    c->wake_seq = a->line_no;
    c->wake_from = c_hpos;
    c->wake_h = h;
}

static void decode_compare(aga_t *a)
{
    copper_t *c = &a->cop;
    c->vcmp = (c->i1 & (c->i2 | 0x8000)) >> 8;
    c->hcmp = c->i1 & c->i2 & 0xFE;
    c->vmask = ((c->i2 >> 8) & 0x7F) | 0x80;
    c->hmask = c->i2 & 0xFE;
    c->blitwait = (c->i2 & 0x8000) ? 0 : 1;
}

/* Advance the copper to colour clock `until` (exclusive).
 *
 * Timing model (calibrated against WinUAE DMA records): the copper uses even
 * cycles. A read planned for cycle c happens only if cycle c itself is free of
 * bitplane DMA, and a WAIT is satisfied when the beam position c-2 has been
 * reached; the first read then happens at the next free even cycle. */
void cop_sync(aga_t *a, int until)
{
    copper_t *c = &a->cop;
    int c_hpos = c->last_hpos;

    if (until > a->maxhpos) until = a->maxhpos;
    if (c_hpos >= until) return;

    if (!aga_dma_enabled(a, DMAF_COPEN) || c->state == COP_stop) {
        c->last_hpos = until;
        return;
    }
    if (c->state == COP_wait && (a->vpos & c->vmask) < c->vcmp) {
        c->last_hpos = until;
        return;
    }
    if (c->state == COP_bltwait) {
        if (a->blt.busy) { c->last_hpos = until; return; }
        c->state = COP_read1;
    }

    c_hpos = (c_hpos + 1) & ~1;
    while (c_hpos < until) {
        int prev = c_hpos - 2;
        if (c->delay > 0) {
            c->delay--;
            c_hpos += 2;
            continue;
        }
        switch (c->state) {
        case COP_strobe_delay:
            c->state = COP_read1;
            break;
        case COP_read1:
            if (pf_is_bpl_slot(a, c_hpos)) break;                 /* slot taken by bitplane DMA */
            if (c->reload) {                                      /* a strobe's pointer, read now */
                c->pc = (c->reload == 1) ? c->cop1lc : c->cop2lc;
                c->reload = 0;
            }
            aga_trace_at(a, c_hpos, AGA_DMA_COPPER, 0x8C, c->pc);
            c->i1 = chip_rd16(a, c->pc);
            c->pc += 2;
            c->state = COP_read2;
            break;
        case COP_read2:
            if (pf_is_bpl_slot(a, c_hpos)) break;
            aga_trace_at(a, c_hpos, AGA_DMA_COPPER,
                         (uint16_t)((c->i1 & 1) ? 0x8C : (c->i1 & 0x1FE)), c->pc);
            c->i2 = chip_rd16(a, c->pc);
            c->pc += 2;
            if (c->ignore_next) {
                c->ignore_next = 0;
                if (c->strobe) {
                    /* the discarded instruction after a copper-written COPJMP */
                    c->pc = (c->strobe == 1) ? c->cop1lc : c->cop2lc;
                    c->strobe = 0;
                    c->reload = 0;
                }
                c->state = COP_read1;
                break;
            }
            if (c->i1 & 1) {
                c->state = (c->i2 & 1) ? COP_skip_in2 : COP_wait_in2;
            } else {
                uint32_t reg = c->i1 & 0x1FE;
                if (cop_reg_allowed(a, reg)) {
                    c->in_move = 1;
                    if (reg == 0x100) {             /* diagnostics: the copper's own BPLCON0 writes */
                        a->dbg_cop_bc0_n++;
                        a->dbg_cop_bc0_pc = c->pc - 4;
                        a->dbg_cop_bc0_val = c->i2;
                        a->dbg_cop_bc0_frame = (uint32_t)a->frame;
                        aga_dbg_event(a, 7, c->i2, 0);
                    }
                    if (reg == 0x088 || reg == 0x08A) aga_dbg_event(a, 9, (uint16_t)reg, 0);
                    aga_custom_wput(a, reg, c->i2);
                    c->in_move = 0;
                    c->state = COP_read1;
                } else {
                    c->state = COP_stop;   /* illegal access: copper halts until next strobe/vblank */
                    c->last_hpos = until;
                    return;
                }
            }
            break;
        case COP_wait_in2:
            decode_compare(a);
            if (c->i1 == 0xFFFF && c->i2 == 0xFFFE) {
                c->state = COP_stop;
                c->last_hpos = until;
                return;
            }
            c->state = COP_wait;
            c->wake_ok = 0;
            if ((a->vpos & c->vmask) < c->vcmp) {
                c->last_hpos = until;
                return;
            }
            /* The compare happens on the NEXT copper cycle, not this one, and
               that cycle may be on the next line - with the line counter it
               carries. Alien Breed: Tower Assault waits past line 255 the usual
               way: WAIT line $FF, then WAIT line $20, which means line $120
               after the counter passes 255. Comparing in the same cycle, while
               the line still reads $FF, made "line $20" true at once ($FF is
               past $20): the modulos for the bottom panel were written 32 lines
               early and every line from there read from the wrong address - the
               band of noise above the lower panel. WinUAE's records show it
               reading those two words at the end of line $FF and the next
               instruction only at line $120. */
            break;
        case COP_wait:
            /* and no wake-up in one either: WinUAE's copper leaves a WAIT in a
               cycle it can use, so after WAIT h=$40 it reads the next MOVE at
               $48 where mine read at $44 ($40 is short, $44 is the wake). The
               wake shortcut (custom.c cck_step, wake_h) stays a lower bound.
               Shadow of the Beast sets COLOR00 from a WAIT at each edge of the
               display window on every line, and its sky band came out 8 lores
               pixels left (from RGB2RTG, EMM #2061: copper slots differing from
               WinUAE on this corpus - SOTB 221 to 1, boot 51 to 1, Castlevania
               t1/t2 59 to 1, Reshoot title 269 to 133, none worse). */
            if (pf_is_bpl_slot(a, c_hpos)) break;
            if (prev >= 0 && beam_reached(a, prev)) {
                if (c->blitwait && a->blt.busy) {
                    c->state = COP_bltwait;
                    c->last_hpos = until;
                    return;
                }
                c->state = COP_read1;
                c->wake_ok = 0;
                /* A WAIT that is already satisfied when the line begins wakes the
                   copper one slot later than a mid-line match: WinUAE's first
                   read is then at cycle 6 (Castlevania lines 52/57, Raptor line 34,
                   agatest line 10), where a mid-line wake-up reads at the next
                   free even cycle. */
                if (c_hpos <= 2) c->delay = 1;
            } else if (!c->wake_ok || c->wake_seq != a->line_no || c_hpos < c->wake_from)
                cop_wait_wake(a, c_hpos);
            break;
        case COP_skip_in2:
            decode_compare(a);
            if (prev >= 0 && beam_reached(a, prev) && !(c->blitwait && a->blt.busy))
                c->ignore_next = 1;
            c->state = COP_read1;
            break;
        default:
            c->state = COP_stop;
            break;
        }
        c_hpos += 2;
    }
    c->last_hpos = until;
}
