/* AGA-PISTORM — bitplane DMA fetch and line rendering (Alice/Lisa playfield).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * UAE's line model: fetch units, fetch start, plane sequence and delay
 * offset, then one rendering pass per line over the fetched planar data and
 * the register changes recorded during the line. From UAE/WinUAE, much of it
 * in the older form kept in E-UAE's custom.c and drawing.c:
 *   copied      the fetchunits/fetchstarts/fm_maxplanes tables,
 *               expand_fmodes, calcdiw (calchdiw and the DIWHIGH mask, first
 *               written after E-UAE's calcdiw, with its rule for a DDFSTOP
 *               past the end of the line), the delay offset
 *               (compute_delay_offset, compute_toscr_delay_1), the BSCAN2
 *               modulo choice (getbplmod), the dual playfield bit split
 *               (gen_pfield_tables)
 *   translated  the colour register write (update_color, COLOR_WRITE), the
 *               BPLxPT..FMODE handlers, HAM6/HAM8 (decode_ham_pixel_aga),
 *               the CLXCON masks (expand_colmask)
 *   data        the plane order (bpl_sequence_8/4/2), the sprite-to-sprite
 *               collision table (E-UAE's sprclx), HARD_DDF_START
 *   model       the fetch loop (started by the window, BPLEN and DDFSTRT
 *               alone, each slot against BPLCON0's plane count, as WinUAE's
 *               bprun and bplcon0_planes_limit), the bitplane slot test,
 *               the DDF stop at or before the start, the change-list
 *               renderer (do_color_changes), dual playfield, EHB, BPLCON4
 *               sprite colours, collisions (denise_collide_sprites)
 * Where the fetch starts, and when register changes take effect, were
 * calibrated against WinUAE's DMA records.
 *   WinUAE custom.cpp:  Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   WinUAE drawing.cpp: Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   E-UAE custom.c:     Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2004 Toni Wilen
 *   E-UAE drawing.c:    Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000,2001 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include <string.h>
#ifdef PFDBG
#include <stdio.h>
#include <stdlib.h>
#endif
#include "internal.h"

#define HARD_DDF_START 0x18
#define HARD_DDF_STOP  0xD8
/* Bitplane DMA starts DDF_OFFSET colour clocks after the DDFSTRT match (WinUAE
   DMA records show the first fetch block at DDFSTRT+4). A block that completes
   at hpos h is displayed from hardware lores pixel h*2 + DISP_OFFSET. DIW register
   values are compared against hardware coordinates minus DIW_HW_OFFSET (UAE:
   DIW_DDF_OFFSET 9 with fetch at DDFSTRT gives the same picture). */
#define DDF_OFFSET     4
/* A BPLCON0 write reaches the bitplane DMA this many colour clocks after the
   copper's slot: WinUAE pipelines it 2 cycles (BPLCON0_delayed), and its DMA
   records show the old plane count's last fetch 3 colour clocks after the
   copper's write (from RGB2RTG d888409). */
#define BPLCON0_DMA_DELAY 4
#define DISP_OFFSET    1
#define DIW_HW_OFFSET  8

static const int fetchunits[]  = { 8, 8, 8, 0, 16, 8, 8, 0, 32, 16, 8, 0 };
static const int fetchstarts[] = { 3, 2, 1, 0,  4, 3, 2, 0,  5,  4, 3, 0 };
static const int fm_maxplanes[]= { 3, 2, 1, 0,  3, 3, 2, 0,  3,  3, 3, 0 };
static const uint8_t seq8[8] = { 8, 4, 6, 2, 7, 3, 5, 1 };
static const uint8_t seq4[4] = { 4, 2, 3, 1 };
static const uint8_t seq2[2] = { 2, 1 };

/* the planes the bitplane DMA fetches now: BPLCON0's, or none at all when
   that is more than the fetch mode has slots for (WinUAE real_bitplane_number:
   "Disable bitplane DMA if planes > available DMA slots", the Sanity WOC
   party effect) - whether set before the line's fetch began or during it */
static inline int fetch_planes(const playfield_t *p)
{
    return p->planes > p->maxplanes ? 0 : p->planes;
}

static inline int slot_plane(int maxplanes, int cyc)
{
    return maxplanes == 8 ? seq8[cyc] : maxplanes == 4 ? seq4[cyc] : seq2[cyc];
}

/* per-line snapshot of state at line start, used by the renderer. With the
   brightness up (aga_set_brightness) line_color is the palette through its
   table - what is shown - and line_raw the palette itself, which HAM needs:
   a HAM pixel keeps channels of the pixel before and replaces one, so its
   base colours must be the real ones, and only what comes out is brightened.
   line_ehb is what EHB's half-bright pixels of the first 32 colours show:
   I halve the real colour, as the chip does, and only then brighten it -
   halving the brightened one instead made bright colours' halves too damn
   dark wherever the brightening clips (found by RGB2RTG, 2026-10-10). */
static uint32_t line_color[256];
static uint32_t line_raw[256];
static uint32_t line_ehb[32];
static inline uint32_t ehb_half(uint32_t c) { return (c >> 1) & 0x7F7F7F; }
static inline uint32_t ehb_shown(const aga_t *a, uint32_t raw)
{
    return a->cfg.bright > 100 ? bright_rgb(a, ehb_half(raw)) : ehb_half(raw);
}
static uint16_t line_bplcon0, line_bplcon1, line_bplcon2, line_bplcon3, line_bplcon4;

static void calcdiw(playfield_t *p, int maxhpos);

void pf_reset(aga_t *a)
{
    /* The chipset personality outlives a reset: it is a property of the game
       about to run, chosen before the sandbox is entered, and aga_reset() runs
       as part of entering. */
    int keep_ecs = a->pf.ecs_mode, keep_forced = a->pf.ecs_forced;
    playfield_t *p = &a->pf;
    memset(p, 0, sizeof *p);
    p->bplcon4 = 0x0011;      /* sprite colour bases 16 (OCS compatible) */
    p->bplcon3 = 0x0C00;      /* PF2OF = 3 (offset 8), like OCS dual playfield */
    p->diwstrt = 0x2C81;
    p->diwstop = 0x2CC1;
    p->ddfstrt = 0x0038;
    p->ddfstop = 0x00D0;
    p->fetchunit = 8; p->fetchstart = 8; p->fetchstart_shift = 3; p->maxplanes = 8;
    p->line_plfleft = -1;
    p->ecs_mode = keep_ecs;
    p->ecs_forced = keep_forced;
    /* Work the window edges out from those defaults right away. Left until the
       first DIWSTRT write, diw_vstart stayed 0, and the very first line of the
       frame then matched it and set the vertical window flip-flop for a window
       nobody had asked for. */
    calcdiw(p, aga_short_line(a));
}

/* ECS Denise on or off. Turned on, the AGA shadow palette (color_aga) starts
   as the palette now; every colour write from then on goes into both. */
void aga_set_ecs(aga_t *a, int on)
{
    if (on && !a->pf.ecs_mode) {
        memcpy(a->color_aga, a->color, sizeof a->color_aga);
        aga_ecs_forget_signs(a);               /* what the AGA chip saw is no sign */
    }
    a->pf.ecs_mode = !!on;
    a->pf.ecs_forced = 0;
}
/* The user's "always ECS" (agaboot ECSGAMES ON): ECS that ecs_says_aga never
   gives up */
void aga_set_ecs_forced(aga_t *a, int on)
{
    aga_set_ecs(a, on);
    a->pf.ecs_forced = !!on;
}
int  aga_get_ecs(const aga_t *a)   { return a->pf.ecs_mode; }
unsigned aga_ecs_signs(const aga_t *a)
{
    const playfield_t *p = &a->pf;
    return (p->aga_fetch_seen ? 1u : 0) | (p->aga_colour_seen ? 2u : 0) |
           (p->aga_pair_bank ? 4u : 0) | (p->aga_pairs ? 8u : 0) |
           (p->ecs_forced ? 0x80u : 0);
}
unsigned aga_ecs_forget_signs(aga_t *a)
{
    playfield_t *p = &a->pf;
    unsigned was = aga_ecs_signs(a) & 15u;
    p->aga_fetch_seen = p->aga_colour_seen = p->aga_pair_bank = 0;
    p->aga_pairs = 0;
    p->pair_banks = 0;
    p->loct_clear_seen = 0;
    memset(p->col_hi_pending, 0, sizeof p->col_hi_pending);
    return was;
}

/* An ECS Denise given to an AGA program after all. agaboot picks ECS for a
   game whose WHDLoad slave neither asks for AGA nor has AGA in its name, and
   Wit Premium (WHDLoad, slave "WitPremium") is such an AGA demo: it loads a
   24-bit palette across the colour banks, which an ECS Denise turned into
   bands of wrong colours (2026-10-05, the user's A500).
   It takes BOTH signs to change its mind, in either order: an AGA fetch - a
   non-zero FMODE or exactly eight bitplanes - and an AGA colour - a colour
   written under LOCT or into a bank, or a BPLCON4. Some ECS games turn an
   AGA fetch mode on for speed and want their ECS colours all the same; some
   carry a stray LOCT bit, which is why ECS mode exists; neither has both.
   And ECS mode only ever changes the colours, so a game with no AGA colour
   needs no change. Or ONE sign that no ECS game gives: 24-bit colour pairs
   (pf_color_write) in two colour banks. Then the palette an AGA chip would
   hold - kept beside the ECS one all along (color_aga) - takes over, so
   colours the program loaded once, before it turned the AGA fetch on, are
   right too. Never against the user's "always ECS". */
static void ecs_says_aga(aga_t *a)
{
    playfield_t *p = &a->pf;
    if (!p->ecs_mode || p->ecs_forced) return;
    if (!p->aga_pair_bank && !(p->aga_fetch_seen && p->aga_colour_seen)) return;
    p->ecs_mode = 0;
    a->ecs_upgrades++;
    memcpy(a->color, a->color_aga, sizeof a->color);
    a->bright_dirty = 1;            /* the brightened palette, at the next line */
    spr_expand_res(a);              /* SPRES now comes from BPLCON3 itself */
    aga_record_change(a, BPLCON3, p->bplcon3);   /* and the rest of this line too */
    aga_record_change(a, BPLCON4, p->bplcon4);
}

static void expand_fmodes(playfield_t *p)
{
    int i = p->fm * 4 + p->res;
    p->fetchunit = fetchunits[i];
    p->fetchstart_shift = fetchstarts[i];
    p->fetchstart = 1 << p->fetchstart_shift;
    p->maxplanes = 1 << fm_maxplanes[i];
}

static void calcdiw(playfield_t *p, int maxhpos)
{
    /* Horizontal edges are kept in superhires units (4 per lores pixel) so the
       AGA sub-lores bits of DIWHIGH can be applied; see WinUAE calchdiw(). */
    int hstrt = (p->diwstrt & 0xFF) << 2, hstop = (p->diwstop & 0xFF) << 2;
    int vstrt = p->diwstrt >> 8,   vstop = p->diwstop >> 8;
    if (p->diwhigh_written) {
        /* a written DIWHIGH is masked before use (WinUAE DIWHIGH()) */
        int dh = p->diwhigh & ~(0x8000 | 0x4000 | 0x0080 | 0x0040);
        hstrt |= ((dh >> 5) & 1) << (8 + 2);
        hstop |= ((dh >> 13) & 1) << (8 + 2);
        /* AGA quarter-lores window edges: measurably wrong in my coordinate
           model (see docs/VALIDATION-FINDINGS.md), left off until calibrated
           hstrt |= (dh >> 3) & 3;
           hstop |= (dh >> 11) & 3; */
        /* V10-V8 only. On AGA bits 3 and 11 are the H0 bits of the start and
           stop (only ECS Agnus reads them as an undocumented V11 - WinUAE
           calcvdiw); taken as V11 they moved the window 2048 lines down, and a
           game that set them would have kept nothing but the copper colours. */
        vstrt |= (dh & 7) << 8;
        vstop |= ((dh >> 8) & 7) << 8;
    } else {
        hstop |= 0x100 << 2;
        if ((vstop & 0x80) == 0) vstop |= 0x100;
    }
    /* the output is hires, so window edges land on whole hires pixels
       (WinUAE masks with hbmask for the current output resolution) */
    hstrt &= ~1;
    hstop &= ~1;
    p->diw_hstart = hstrt;
    p->diw_hstop = hstop;
    p->diw_vstart = vstrt;
    p->diw_vstop = vstop;

    p->plfstrt = p->ddfstrt;
    /* A DDFSTOP past the end of the line never matches, so the ECS/AGA Agnus's
       fetch window never closes: every line's fetch runs from the hard start,
       as if DDFSTRT were $18 (E-UAE calcdiw: "if (ddfstop > maxhpos) plfstrt
       = 0"; WinUAE shows it the same). Wit Premium's PREMIUM screen - DDFSTRT
       $28, DDFSTOP $E8, FMODE 3 - came out one 64-pixel block too far right
       without it, its last letter cut off (2026-10-04; from RGB2RTG
       f2a0073). maxhpos is 0 before the first line: then the PAL line's $E3. */
    if (p->ddfstop > (maxhpos > 0 ? maxhpos : 0xE3)) p->plfstrt = 0;
    if (p->plfstrt < HARD_DDF_START) p->plfstrt = HARD_DDF_START;
    p->plfstop = p->ddfstop;
    if (p->plfstop > HARD_DDF_STOP) p->plfstop = HARD_DDF_STOP;
    /* A DDFSTOP at or before DDFSTRT has matched before the fetch began - its
       compare comes a cycle ahead of DDFSTRT's - and stops nothing: the fetch
       runs to the hard stop (WinUAE's DDF state machine, the $D7 limit).
       Aladdin AGA's logo screens - DDFSTRT $38, DDFSTOP written $0100, so $00,
       FMODE 3 - are six 64-pixel blocks a line; I fetched one, and the
       interleaved 384-pixel picture came out as a narrow striped column (from
       RGB2RTG d63b149). */
    /* The registers as written decide that, not the start clamped to $18: a
       DDFSTRT and DDFSTOP both at or below the hard start - $10 and $14 -
       match DDFSTOP before the fetch can begin, and the line fetches nothing
       (WinUAE's ECS/AGA DDF state machine). plfstop below plfstrt says so. */
    if (p->ddfstop <= p->ddfstrt) p->plfstop = HARD_DDF_STOP;
    else if (p->ddfstop <= HARD_DDF_START) p->plfstop = -1;
}

/* Scroll delays in native pixels for a BPLCON1 value, using the line's fetch
   parameters. UAE: an undocumented offset applies for fetch starts that are
   not aligned to the fetch unit. */
static void delays_for(const playfield_t *p, uint16_t con1, int *d1out, int *d2out)
{
    int fetchwidth = 16 << p->line_fm;
    int d1 = (con1 & 0x0F) | ((con1 & 0x0C00) >> 6);
    int d2 = ((con1 >> 4) & 0x0F) | (((con1 >> 4) & 0x0C00) >> 6);
    int mask = (fetchwidth - 1) >> p->line_res;
    *d1out = ((d1 + p->line_delayoffset) & mask) << p->line_res;
    *d2out = ((d2 + p->line_delayoffset) & mask) << p->line_res;
}

static void compute_delays(playfield_t *p)
{
    int fetchwidth = 16 << p->fm;
    p->line_delayoffset = fetchwidth - (((p->plfstrt - HARD_DDF_START) & (p->fetchstart - 1)) << 1);
    delays_for(p, p->bplcon1, &p->line_delay1, &p->line_delay2);
}

/* What the line's fetch and its drawing take from where the fetch began
   (fetch_start_h): the resolution, fetch mode and scroll delays, the number
   of fetch cycles to DDFSTOP, and where the first block shows. */
static void latch_line(playfield_t *p)
{
    p->line_res = p->res;
    p->line_fm = p->fm;
    p->line_bplcon0 = p->bplcon0;
    compute_delays(p);
    int stop = p->plfstop < p->plfstrt ? p->plfstrt : p->plfstop;
    int len = stop - p->plfstrt;
    len = (len + p->fetchunit - 1) & ~(p->fetchunit - 1);
    p->fetch_total = len + p->fetchunit;
    /* the first block becomes visible once plane 1 (fetched last in the
       block, at cycle maxplanes-1) has been fetched */
    p->line_plfleft = p->fetch_start_h + p->maxplanes;
}

/* An ECS game drives an ECS Denise, which has no LOCT bit, no colour bank and
   no sprite colour offset: a COLORxx write is a whole 12-bit colour and sprites
   take colours 16-31. My chipset is AGA and obeys all three, so an ECS game
   playing with BPLCON3 bit 9 set has its palette written into the low nibbles
   and comes out at a sixteenth brightness - the "dark ECS games", whose LOCT
   came from graphics.library in AA mode after WHDLoad's SetChipRev (agaboot
   stubs that now). In ECS mode I present those registers as their reset
   values instead, so a COLORxx write is one whole 12-bit colour, there is no
   colour bank, and sprites take colours 16-31 - an ECS Denise. BPLCON3's
   bits an ECS Denise does have (BRDRBLNK, BRDNTRAN, ZDCLKEN, EXTBLKEN) are
   kept, and BPLCON2's RDRAM, which it does not have, is ignored.

   The mode is agaboot's choice for the game (sent to $DFF1F2 at sandbox
   entry: $5E00 AGA, $5E01 ECS, $5E03 the user's "always ECS"), because
   nothing in a WHDLoad slave reliably says which chipset a game draws for;
   ecs_says_aga takes it back when the game shows it is AGA after all.
   Nothing changes for AGA games: with this off the renderer is
   byte-identical, which tools/regress.py checks. */
#define BPLCON3_RESET 0x0C00
#define BPLCON4_RESET 0x0011

#define BPLCON3_ECS_BITS 0x0035     /* BRDRBLNK, BRDNTRAN, ZDCLKEN, EXTBLKEN */
uint16_t pf_con3(const playfield_t *p) { return p->ecs_mode ? (uint16_t)(BPLCON3_RESET | (p->bplcon3 & BPLCON3_ECS_BITS)) : p->bplcon3; }
uint16_t pf_con4(const playfield_t *p) { return p->ecs_mode ? BPLCON4_RESET : p->bplcon4; }

/* Colour clocks since the reset, near enough (a line counts as 228): when a
   24-bit pair's two writes came, for the windows below. */
static inline uint32_t pf_now(const aga_t *a) { return a->line_no * 228u + (uint32_t)a->hpos; }
#define PAIR_CCK      200u          /* bank 0: the low write within this of the high one */
#define PAIR_CCK_BANK 912u          /* in a bank: four lines, for a loader that writes every
                                       bank's high nibbles before the low ones */

void pf_color_write(aga_t *a, int num, uint16_t v, int hpos)
{
    playfield_t *p = &a->pf;
    int rdram = (p->bplcon2 & BPLCON2_RDRAM) != 0;
    if (rdram && !p->ecs_mode) return;          /* an AGA Lisa reads instead */
    if (p->ecs_mode && !rdram) {                /* (and the shadow AGA palette is left alone) */
        /* the same write as an AGA Lisa takes it, into the shadow palette
           ecs_says_aga hands over: the bank and LOCT BPLCON3 really has */
        uint16_t raw = p->bplcon3;
        int ai = ((raw >> 13) & 7) * 32 + num;
        uint32_t ar = (v >> 8) & 15, ag = (v >> 4) & 15, ab = v & 15, ac = a->color_aga[ai];
        uint32_t xr = ac >> 16, xg = (ac >> 8) & 0xFF, xb = ac & 0xFF;
        if (raw & BPLCON3_LOCT) { xr = (xr & 0xF0) | ar; xg = (xg & 0xF0) | ag; xb = (xb & 0xF0) | ab; }
        else                    { xr = ar | (ar << 4); xg = ag | (ag << 4); xb = ab | (ab << 4); }
        a->color_aga[ai] = (xr << 16) | (xg << 8) | xb;
        /* A 24-bit colour pair - the sequence AGA software writes a 24-bit
           colour with: BPLCON3 written with LOCT clear, the register's high
           nibbles, BPLCON3 with LOCT set, its low nibbles, another value, all
           close together (PAIR_CCK; a bank's 32 colours fit PAIR_CCK_BANK).
           No ECS program has a reason to: a stray LOCT stays set, Chaos
           Engine's toggle writes the same value twice (taken either way, the
           same colour), and a panel's stray LOCT a line below a gradient is
           too far from the gradient's write (found by review, 2026-10-05).
           Wit Premium (WHDLoad, no AGA flag) draws its red skies that way on
           every line with FMODE 0 and one or two bitplanes, so the two signs
           came only at its wipe a minute in, after every sky had been drawn
           as rainbow bands (2026-10-05; WinUAE capture, the user's A500).
           In bank 0 the pair is taken the AGA way and nothing else changes.
           Pairs in two banks, one of them not bank 0, are a palette loaded
           across the banks - the sign ecs_says_aga needs on its own; one bank
           alone could be junk bank bits from a table (found by review). */
        int pair = 0;
        uint32_t now = pf_now(a), win = (raw & 0xE000) ? PAIR_CCK_BANK : PAIR_CCK;
        if (raw & BPLCON3_LOCT) {
            pair = p->col_hi_pending[ai] && (v & 0x0FFF) != p->col_hi_val[ai] &&
                   now - p->col_hi_when[ai] <= win && !p->ecs_forced;
            p->col_hi_pending[ai] = 0;
        } else {
            p->col_hi_pending[ai] = p->loct_clear_seen && now - p->loct_clear_when <= win;
            p->col_hi_val[ai] = (uint16_t)(v & 0x0FFF);
            p->col_hi_when[ai] = now;
        }
        if (pair) {
            p->pair_banks = (uint8_t)(p->pair_banks | (1u << ((raw >> 13) & 7)));
            if ((p->pair_banks & 0xFE) && (p->pair_banks & (p->pair_banks - 1))) p->aga_pair_bank = 1;
        }
        if (raw & (BPLCON3_LOCT | 0xE000)) { p->aga_colour_seen = 1; ecs_says_aga(a); }
        if (!p->ecs_mode) {                     /* just handed over: the AGA palette has this write */
            aga_record_change(a, LC_COLOR + ai, a->color_aga[ai]);
            return;
        }
        if (pair && !(raw & 0xE000)) {          /* bank 0: ai is num, the ECS index */
            uint32_t val = a->color_aga[ai];
            p->aga_pairs++;
            if (val == a->color[num]) return;
            a->color[num] = val;
            if (a->cfg.bright > 100) a->color_b[num] = bright_rgb(a, val);
            aga_record_change(a, LC_COLOR + num, val);
            return;
        }
    }
    uint16_t con3 = pf_con3(p);
    int idx = ((con3 >> 13) & 7) * 32 + num;
    uint32_t r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    uint32_t cur = a->color[idx];
    uint32_t cr = cur >> 16, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
    if (con3 & BPLCON3_LOCT) {
        cr = (cr & 0xF0) | r; cg = (cg & 0xF0) | g; cb = (cb & 0xF0) | b;
    } else {
        cr = r | (r << 4); cg = g | (g << 4); cb = b | (b << 4);
    }
    uint32_t val = (cr << 16) | (cg << 8) | cb;
    if (val == cur) return;
    a->color[idx] = val;
    if (a->cfg.bright > 100) a->color_b[idx] = bright_rgb(a, val);
    aga_record_change(a, LC_COLOR + idx, val);      /* the full 0..255 index */
    (void)hpos;
}

void pf_write(aga_t *a, uint32_t reg, uint16_t v, int hpos)
{
    playfield_t *p = &a->pf;
    if (reg >= BPL1PTH && reg < BPL1PTH + 32) {
        int n = (reg - BPL1PTH) >> 2;
        pf_sync(a, hpos);
        if ((reg & 2) == 0) p->bplpt[n] = (p->bplpt[n] & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16);
        else                p->bplpt[n] = (p->bplpt[n] & 0xFFFF0000u) | (v & 0xFFFE);
        return;
    }
    if (reg >= BPL1DAT && reg < BPL1DAT + 16) {
        p->bpldat[(reg - BPL1DAT) >> 1] = v;
        return;
    }
    switch (reg) {
    case BPLCON0:
        if (p->bplcon0 == v) return;
        /* The bitplane DMA fetches the planes BPLCON0 allows at each slot, so a
           change mid-line counts from BPLCON0_DMA_DELAY on (WinUAE: the plane
           against bplcon0_planes_limit at every fetch). The count was taken
           once, where the line's fetch began. Zool (AGA) ends its status bar
           with BPLCON0 $0200 at colour clock $3E of line 59 and sets the
           playfield's pointers right after: WinUAE fetches up to $41 and
           stops, I fetched the rest of the line with four planes, those
           pointers ran a line ahead and the whole playfield came from the
           wrong rows (from RGB2RTG d888409). line_planes stays the most the
           line had, for drawing: the blocks a plane missed hold zeros
           (cleared each line). A line that began with no planes - or more
           than the fetch mode allows - has its fetch running all the same
           and fetches from here on; one that has fetched nothing yet takes
           the new resolution and fetch mode for drawing as well. */
        pf_sync(a, hpos + BPLCON0_DMA_DELAY);
        if ((v & 0x7010) == 0x0010) {            /* BPU3 alone: exactly eight bitplanes */
            p->aga_fetch_seen = 1;
            ecs_says_aga(a);
        }
        p->bplcon0 = v;
        p->res = GET_RES(v);
        p->planes = GET_PLANES(v);
        expand_fmodes(p);
        if (p->fetch_active) {
            if (p->line_planes == 0) latch_line(p);
            if (fetch_planes(p) > p->line_planes) p->line_planes = fetch_planes(p);
        }
        spr_expand_res(a);
        aga_record_change(a, BPLCON0, v);
        break;
    case BPLCON1:
        if (p->bplcon1 == v) return;
        pf_sync(a, hpos);
        p->bplcon1 = v;
        if (p->fetch_active) compute_delays(p);   /* takes effect for following blocks */
        aga_record_change(a, BPLCON1, v);
        break;
    case BPLCON2:
        if (p->bplcon2 == v) return;
        p->bplcon2 = v;
        aga_record_change(a, BPLCON2, v);
        break;
    /* the line's drawing takes the change as an ECS Denise would too: its
       start uses pf_con3/pf_con4, and so must a change in mid-line */
    case BPLCON3:
        if (p->ecs_mode && !(v & BPLCON3_LOCT)) {  /* even rewritten as it was: a 24-bit pair's start */
            p->loct_clear_seen = 1;
            p->loct_clear_when = pf_now(a);
        }
        if (p->bplcon3 == v) return;
        p->bplcon3 = v;
        spr_expand_res(a);
        aga_record_change(a, BPLCON3, pf_con3(p));
        break;
    case BPLCON4:
        if (p->bplcon4 == v) return;
        if (p->ecs_mode && (v & 0xFF00)) {       /* a bitplane colour XOR: AGA colours */
            p->aga_colour_seen = 1;
            ecs_says_aga(a);
        }
        p->bplcon4 = v;
        aga_record_change(a, BPLCON4, pf_con4(p));
        break;
    case BPL1MOD:
        pf_sync(a, hpos);
        p->bpl1mod = (int16_t)(v & 0xFFFE);
        break;
    case BPL2MOD:
        pf_sync(a, hpos);
        p->bpl2mod = (int16_t)(v & 0xFFFE);
        break;
    case DIWSTRT:
        p->diwstrt = v; p->diwhigh_written = 0; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWSTRT, v);
        break;
    case DIWSTOP:
        p->diwstop = v; p->diwhigh_written = 0; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWSTOP, v);
        break;
    case DIWHIGH:
        p->diwhigh = v; p->diwhigh_written = 1; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWHIGH, v);
        break;
    case DDFSTRT:
        pf_sync(a, hpos);
        p->ddfstrt = v & 0xFE; calcdiw(p, aga_short_line(a));
        break;
    case DDFSTOP:
        pf_sync(a, hpos);
        p->ddfstop = v & 0xFE; calcdiw(p, aga_short_line(a));
        break;
    case FMODE:
        pf_sync(a, hpos);
        if (v & 0xC00F) {                        /* fetch/sprite width, BSCAN2, SSCAN2 */
            p->aga_fetch_seen = 1;
            ecs_says_aga(a);
        }
        p->fmode = v;
        p->fm = (v & 3) == 3 ? 2 : (v & 3) ? 1 : 0;
        expand_fmodes(p);
        spr_expand_res(a);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
void pf_vsync(aga_t *a)
{
    a->pf.vdiw = 0;
    a->pf.vdiw_ff = 0;
}

void pf_start_line(aga_t *a)
{
    playfield_t *p = &a->pf;
    /* Vertical display window flip-flop, and the blanking that hides it.
     *
     * These are two different things and were one. Vertical blanking
     * inhibits the fetch, but it does not touch the flip-flop: a game
     * whose DIWSTRT starts the window INSIDE the blank - Worms: The
     * Director's Cut sets $0081, line 0 - has its window open from the
     * moment the blank ends. Clearing the flip-flop there instead lost
     * the only line that ever sets it, and the window then stayed shut
     * for the rest of the frame, and every frame after it: eight
     * bitplanes, DMA on, pointers into real graphics, and not one word
     * fetched. A black screen for ever. */
    if (a->vpos == p->diw_vstop) p->vdiw_ff = 0;
    if (a->vpos == p->diw_vstart) p->vdiw_ff = 1;
    p->vdiw = p->vdiw_ff && a->vpos >= a->vblank_end;

    p->fetch_active = 0;
    p->fetch_done = 0;
    p->mod_added = 0;
    p->mod_planes = 0;
    memset(p->slot_used, 0, sizeof p->slot_used);
    p->fetch_cycle = 0;
    p->fetch_last_hpos = 0;
    p->line_plfleft = -1;
    p->line_blocks = 0;
    p->line_planes = 0;
    p->fetch_start_h = -1;
    /* only the blocks the last line wrote: the rest are zero already (every
       line clears what it used, and reset cleared the lot). 8 KB a line
       went through the cache for the few hundred bytes a line fetches. */
    if (p->blocks_dirty > 0) {
        if (p->blocks_dirty > AGA_MAX_BLOCKS) p->blocks_dirty = AGA_MAX_BLOCKS;
        memset(p->blockdata, 0, (size_t)p->blocks_dirty * sizeof p->blockdata[0]);
        p->blocks_dirty = 0;
    }

    if (a->cfg.bright > 100) {
        if (a->bright_dirty) {                   /* a new level, or colours set around pf_color_write */
            for (int i = 0; i < 256; i++) a->color_b[i] = bright_rgb(a, a->color[i]);
            a->bright_dirty = 0;
        }
        memcpy(line_color, a->color_b, sizeof line_color);
        memcpy(line_raw, a->color, sizeof line_raw);
    } else {
        memcpy(line_color, a->color, sizeof line_color);
    }
    for (int i = 0; i < 32; i++) line_ehb[i] = ehb_shown(a, a->color[i]);
    line_bplcon0 = p->bplcon0; line_bplcon1 = p->bplcon1; line_bplcon2 = p->bplcon2;
    line_bplcon3 = pf_con3(p); line_bplcon4 = pf_con4(p);
}

/* OR `nbits` bits of `data` (MSB first, data right-aligned) into plane `plane` at bit position `pos`. */
static inline void put_bits(playfield_t *p, int plane, int pos, uint64_t data, int nbits)
{
    if (pos < 0 || pos + nbits > AGA_PLANE_WORDS * 64) return;
    int w = pos >> 6, b = pos & 63;
    /* place the nbits field so that its MSB lands at bit position `pos` counting from the MSB of word w */
    int shift = 64 - b - nbits;   /* left shift within word w */
    if (shift >= 0) {
        p->bits[plane][w] |= data << shift;
    } else {
        p->bits[plane][w] |= data >> (-shift);
        if (w + 1 < AGA_PLANE_WORDS) p->bits[plane][w + 1] |= data << (64 + shift);
    }
}

static void fetch_plane(aga_t *a, int plane, int block, int hpos)
{
    playfield_t *p = &a->pf;
    uint32_t addr = p->bplpt[plane];
    uint64_t data;
    aga_trace_at(a, hpos, AGA_DMA_BITPLANE, (uint16_t)(BPL1DAT + 2 * plane), addr);
    switch (p->line_fm) {
    case 0:  data = chip_rd16(a, addr); p->bplpt[plane] += 2; break;
    case 1:  data = chip_rd32(a, addr); p->bplpt[plane] += 4; break;
    default: data = chip_rd64(a, addr); p->bplpt[plane] += 8; break;
    }
    if (data) a->pf_data_nonzero = 1;   /* diagnostics: did anything ever fill the bitplanes */
    if (block < AGA_MAX_BLOCKS) {
        p->blockdata[block][plane] = data;
        if (block >= p->blocks_dirty) p->blocks_dirty = block + 1;
    }
}

/* A block is complete once its last slot (plane 1) has passed: the scroll delay
   in effect at that moment applies to the whole block (UAE beginning_of_plane_block). */
static void complete_block(playfield_t *p, int block)
{
    if (block < AGA_MAX_BLOCKS) {
        p->blockdelay1[block] = (int16_t)p->line_delay1;
        p->blockdelay2[block] = (int16_t)p->line_delay2;
    }
}

static void build_bits(playfield_t *p)
{
    int W = 16 << p->line_fm;
    int nb = p->line_blocks < AGA_MAX_BLOCKS ? p->line_blocks : AGA_MAX_BLOCKS;
    memset(p->bits, 0, sizeof p->bits);
    for (int b = 0; b < nb; b++)
        for (int pl = 0; pl < p->line_planes; pl++)
#ifdef SCROLL_MODEL_BLOCK
            put_bits(p, pl, b * W + ((pl & 1) ? p->blockdelay2[b] : p->blockdelay1[b]), p->blockdata[b][pl], W);
#else
            put_bits(p, pl, b * W, p->blockdata[b][pl], W);
#endif
}

static void add_modulos(aga_t *a)
{
    playfield_t *p = &a->pf;
#ifdef PFDBG
    if (getenv("MODDBG") && a->vpos >= 0x40 && a->vpos <= 0x46)
        fprintf(stderr, "v$%03x add_modulos at h=%d: mod1=%d mod2=%d bpl1=$%06x bpl2=$%06x\n",
                a->vpos, p->fetch_last_hpos, p->bpl1mod, p->bpl2mod, p->bplpt[0], p->bplpt[1]);
#endif
    if (p->fmode & 0x4000) {
        /* BSCAN2, AGA bitplane scan-doubling: every plane takes the SAME
           modulo on a line, BPL1MOD and BPL2MOD alternating line by line from
           the parity of DIWSTRT's start line (WinUAE getbplmod). With BPL1MOD
           set to minus one line, each line is fetched twice - a 1x2 chunky
           screen from half the memory and half the conversion. Synergy
           (Sector 7) runs its whole intro like that; taking the odd/even
           plane split instead showed every source line once, so the picture
           came out half height with the next buffer's worth of memory under
           it. */
        int mod = (((p->diwstrt >> 8) ^ (a->vpos ^ 1)) & 1) ? p->bpl1mod : p->bpl2mod;
        for (int i = 0; i < AGA_MAX_PLANES; i++)
            if (p->mod_planes & (1u << i)) p->bplpt[i] += mod;
        return;
    }
    /* only the planes fetched in the line's last block: WinUAE adds a
       plane's modulo with its own fetch there, so a plane a mid-line BPLCON0
       switched off before its slot takes none, and one switched on after its
       slot takes none either */
    for (int i = 0; i < AGA_MAX_PLANES; i++)
        if (p->mod_planes & (1u << i)) p->bplpt[i] += (i & 1) ? p->bpl2mod : p->bpl1mod;
}

/* Add them once per line, when the last bitplane fetch of the line is done and
   with the values in force at that moment - not after the trailing cycles of
   the last fetch block. Reshoot Proxima 3's dialogue box sets the copper
   loose right after the last fetch (it ends at colour clock 195, the copper
   writes at 200): new pointers for planes 1, 3, 5 and 7, and new modulos.
   Adding the modulos afterwards applied the new ones to the freshly written
   pointers, so every line below read from the wrong address - the noisy
   portrait, the doubled text and the column of noise down the screen. */
static void add_modulos_once(aga_t *a)
{
    if (a->pf.mod_added) return;
    a->pf.mod_added = 1;
    add_modulos(a);
}

/* One cycle of the line's fetch: the plane its slot belongs to, if BPLCON0
   lets the DMA have it, and the block's completion at its last slot (plane
   1). in_line is 0 for the cycles finished at the end of the line, which
   really fall in the next line's first clocks: those are not this line's
   slots for the copper. */
static void fetch_step(aga_t *a, int h, int in_line)
{
    playfield_t *p = &a->pf;
    int cyc = p->fetch_cycle & (p->fetchstart - 1);
    int block = p->fetch_cycle >> p->fetchstart_shift;
    if (cyc < p->maxplanes) {
        int plane = slot_plane(p->maxplanes, cyc);
        int last = ((block + 1) << p->fetchstart_shift) >= p->fetch_total;
        if (plane <= fetch_planes(p)) {
            fetch_plane(a, plane - 1, block, h);
            if (in_line && h >= 0 && h < 256) p->slot_used[h >> 6] |= 1ull << (h & 63);
            if (last) p->mod_planes |= 1u << (plane - 1);
        }
        if (cyc == p->maxplanes - 1) {
            complete_block(p, block);
            if (last) add_modulos_once(a);
        }
    }
    p->fetch_cycle++;
}

/* The line's fetch is over: at DDFSTOP's last block, or at the end of the
   line. Fetching may continue into the first cycles of the next line (the
   hardware keeps the DMA slots going); those fetches only advance the
   pointers, the data lands in horizontal blanking, so the remaining slots of
   the last block are completed here at once. */
static void end_fetch(aga_t *a, int h)
{
    playfield_t *p = &a->pf;
    while (p->fetch_cycle < p->fetch_total) fetch_step(a, h, 0);
    p->fetch_active = 0;
    p->fetch_done = 1;
    p->line_blocks = (p->fetch_cycle + p->fetchstart - 1) >> p->fetchstart_shift;
    add_modulos_once(a);
}

void pf_sync(aga_t *a, int hpos)
{
    playfield_t *p = &a->pf;
    if (hpos > a->maxhpos) hpos = a->maxhpos;
    int h = p->fetch_last_hpos;
    if (h >= hpos) return;

    for (; h < hpos; h++) {
        if (!p->fetch_active && !p->fetch_done && h == p->plfstrt + DDF_OFFSET) {
            /* which of the four things a fetching line needs was missing */
            a->dbg_fetch[0]++;
            if (!p->vdiw) a->dbg_fetch[1]++;
            else if (!aga_dma_enabled(a, DMAF_BPLEN)) a->dbg_fetch[2]++;
            else if (p->planes <= 0) a->dbg_fetch[3]++;
            else if (p->planes > p->maxplanes) a->dbg_fetch[4]++;
            else a->dbg_fetch[5]++;
            /* The fetch sequencer starts on the window, BPLEN and DDFSTRT
               alone; BPLCON0's plane count only decides, slot by slot, which
               planes it fetches (WinUAE: bprun without a plane term, then
               bplcon0_planes_limit at each fetch). A line that starts with no
               planes and has them switched on a few clocks later - a copper
               MOVE to BPLCON0 just ahead of the fetch start, or just after it
               - fetches the rest of the line; started only with planes, it
               lost the whole line. */
            if (p->vdiw && aga_dma_enabled(a, DMAF_BPLEN)) {
                /* DDFSTOP matched before the fetch could begin (calcdiw) */
                if (p->plfstop < p->plfstrt) { p->fetch_done = 1; continue; }
                p->fetch_active = 1;
                p->fetch_cycle = 0;
                p->fetch_start_h = h;
                p->line_planes = fetch_planes(p);
                latch_line(p);
            }
        }
        if (p->fetch_active) {
            fetch_step(a, h, 1);
            if (p->fetch_cycle >= p->fetch_total || h + 1 >= a->maxhpos)
                end_fetch(a, h);
        }
    }
    p->fetch_last_hpos = hpos;
}

/* Is colour clock hpos a slot the bitplane DMA took? What the fetch did
   there, not what BPLCON0 says now: a BPLCON0 write reaches the fetch
   BPLCON0_DMA_DELAY clocks later, and those slots have already gone to the
   old plane count. */
int pf_is_bpl_slot(aga_t *a, int hpos)
{
    playfield_t *p = &a->pf;
    pf_sync(a, hpos + 1);
    if (hpos < 0 || hpos >= 256) return 0;
    return (int)((p->slot_used[hpos >> 6] >> (hpos & 63)) & 1);
}

/* -------------------------------------------------------------------------- */
/* planar -> chunky for the whole line, odd and even planes separately (both keep
   their original bit positions: odd planes in bits 0,2,4,6, even in 1,3,5,7) */
/* Eight pixels at a time: a byte of each plane is spread into eight bytes by
   a table (bit 7 of the byte, the leftmost pixel, lands in the first byte)
   and shifted up to its plane's bit. Seven table lookups per eight pixels
   where it was 56 shift-and-mask steps; the same bytes come out.
   The kernel is built BIG-endian (-mbig-endian): the table is built through a
   byte array, so "byte i = pixel i" holds in memory on either byte order, and
   the shifts never carry out of a byte (0/1 moved up at most 7 bits). The first
   version built it numerically, little-endian only: on the Pi every 8-pixel
   group came out mirrored - Banshee sawtoothed and shifted, while the PC,
   little-endian, rendered every regression frame byte-identical. */
static void planar_to_chunky(playfield_t *p, int nbits)
{
    static uint64_t spread[256];
    static int spread_ready;
    if (!spread_ready) {
        for (int b = 0; b < 256; b++) {
            uint8_t px[8];
            for (int i = 0; i < 8; i++) px[i] = (b & (0x80 >> i)) ? 1 : 0;
            memcpy(&spread[b], px, 8);
        }
        spread_ready = 1;
    }
    int planes = p->line_planes;
    int words = (nbits + 63) >> 6;
    for (int w = 0; w < words; w++) {
        for (int k = 0; k < 8; k++) {
            int sh = 56 - 8 * k;                       /* pixels 8k..8k+7 of the word */
            uint64_t vo = 0, ve = 0;
            for (int pl = 0; pl < planes; pl += 2) vo |= spread[(p->bits[pl][w] >> sh) & 0xFF] << pl;
            for (int pl = 1; pl < planes; pl += 2) ve |= spread[(p->bits[pl][w] >> sh) & 0xFF] << pl;
            memcpy(&p->idx_odd[w * 64 + k * 8], &vo, 8);   /* byte i = pixel 8k+i, either byte order */
            memcpy(&p->idx_even[w * 64 + k * 8], &ve, 8);
        }
    }
}

/* The mean of two pixels, channel by channel. Super-hires puts two pixels in
   every output (hires) pixel, and keeping only the first - WinUAE's picture
   with "Filtered low resolution" off - throws half of every dither away:
   Virtual Karting 2 mixes four colours inside one lores pixel for a TV to
   blend, and its ground came out as stripes of two of them. */
static inline uint32_t mix2(uint32_t a, uint32_t b)
{
    return (a & b & 0xFFFFFF) + (((a ^ b) & 0xFEFEFE) >> 1);
}

/* One playfield pixel's colour from its raw bitplane value - dual playfield,
   EHB or plain, with BPLAM - and what the sprite priority needs: the same
   decisions as render_segment's loops, for the super-hires pairs. */
static uint8_t dpf1_tab[256], dpf2_tab[256];
static int dpf_tab_ready;
static void dpf_tab_init(void)
{
    for (unsigned v = 0; v < 256; v++) {
        dpf1_tab[v] = (uint8_t)((v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8));
        dpf2_tab[v] = (uint8_t)(((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8));
    }
    dpf_tab_ready = 1;
}
static inline uint32_t pf_pixel(unsigned v, int dpf, int ehb, int pf2pri, int pf2of, unsigned xorv,
                                int *pfv, int *front_pf)
{
    *front_pf = 2;
    if (dpf) {
        unsigned v1 = dpf1_tab[v & 255], v2 = dpf2_tab[v & 255], idx;
        if (pf2pri) {
            if (v2)      { idx = v2 + pf2of; *pfv = (int)v2; }
            else if (v1) { idx = v1;         *pfv = (int)v1; *front_pf = 1; }
            else         { idx = 0;          *pfv = 0; }
        } else {
            if (v1)      { idx = v1;         *pfv = (int)v1; *front_pf = 1; }
            else if (v2) { idx = v2 + pf2of; *pfv = (int)v2; }
            else         { idx = 0;          *pfv = 0; }
        }
        return line_color[(idx ^ xorv) & 255];
    }
    *pfv = (int)v;
    if (ehb) return (v & 0x20) ? line_ehb[(v & 31) ^ (xorv & 31)] : line_color[(v ^ xorv) & 255];
    return line_color[(v ^ xorv) & 255];
}
/* the sprite pixel at super-hires position sx over colour c, if it wins */
static inline uint32_t spr_pixel(uint32_t c, const uint8_t *spr, int sprlen, int sx, int pfv, int front_pf,
                                 int pf1p, int pf2p, int base_even, int base_odd)
{
    uint8_t sv = (sx >= 0 && sx < sprlen) ? spr[sx] : 0;
    if (!sv) return c;
    int pair = sv >> 6;
    int visible = (pfv == 0) ? 1 : (front_pf == 1 ? pair < pf1p : pair < pf2p);
    if (!visible) return c;
    int cc = sv & 15;
    int idx = (sv & 0x10) ? (base_odd + cc) : (((sv & 0x20) ? base_odd : base_even) + pair * 4 + cc);
    return line_color[idx & 255];
}

/* A playfield pixel's raw value, decoded once per setting instead of at
   every pixel - bits 0-7 the colour index (BPLAM applied), bit 8 EHB's half
   brightness, bits 9-11 the sprite pairs shown in front of it (pair < this).
   The same decisions as render_segment's general loop; built again only when
   the line's BPLCON2/3/4 or the mode change it. From RGB2RTG 0.75-dev18 (by
   the same author): dual playfield's branches at every pixel were a third of
   Jim Power's pixel loop on a Pi 3. */
static uint16_t pfx_tab[256];
static int pfx_key = -1;
static const uint16_t *pfx_table(int dpf, int ehb, int pf2pri, int pf2of, unsigned xorv, int pf1p, int pf2p)
{
    int key = dpf | ehb << 1 | pf2pri << 2 | pf2of << 3 | (int)xorv << 11 | pf1p << 19 | pf2p << 22;
    if (key == pfx_key)
        return pfx_tab;
    if (!dpf_tab_ready) dpf_tab_init();
    int t1 = pf1p < 4 ? pf1p : 4, t2 = pf2p < 4 ? pf2p : 4;
    for (unsigned v = 0; v < 256; v++) {
        unsigned idx, half = 0;
        int thr;
        if (dpf) {
            unsigned v1 = dpf1_tab[v], v2 = dpf2_tab[v];
            if (pf2pri) {
                if (v2)      { idx = v2 + pf2of; thr = t2; }
                else if (v1) { idx = v1;         thr = t1; }
                else         { idx = 0;          thr = 4; }
            } else {
                if (v1)      { idx = v1;         thr = t1; }
                else if (v2) { idx = v2 + pf2of; thr = t2; }
                else         { idx = 0;          thr = 4; }
            }
            idx = (idx ^ xorv) & 255;
        } else {
            thr = v ? t2 : 4;
            if (ehb && (v & 0x20)) { idx = (v & 31) ^ (xorv & 31); half = 1; }
            else                     idx = (v ^ xorv) & 255;
        }
        pfx_tab[v] = (uint16_t)(idx | half << 8 | (unsigned)thr << 9);
    }
    pfx_key = key;
    return pfx_tab;
}

typedef struct segstate {
    uint16_t con0, con1, con2, con3, con4;
    int diw_hstart, diw_hstop;
    uint32_t ham_last;
} segstate_t;

static void apply_change(aga_t *a, segstate_t *s, const linechange_t *c)
{
    if (c->reg >= LC_COLOR && c->reg < LC_COLOR + 256) {
        int i = c->reg - LC_COLOR;
        if (a->cfg.bright > 100) { line_raw[i] = c->value; line_color[i] = bright_rgb(a, c->value); }
        else line_color[i] = c->value;
        if (i < 32) line_ehb[i] = ehb_shown(a, c->value);
        return;
    }
    switch (c->reg) {
    case BPLCON0: s->con0 = (uint16_t)c->value; break;
    case BPLCON1: s->con1 = (uint16_t)c->value; break;
    case BPLCON2: s->con2 = (uint16_t)c->value; break;
    case BPLCON3: s->con3 = (uint16_t)c->value; break;
    case BPLCON4: s->con4 = (uint16_t)c->value; break;
    case DIWSTRT: case DIWSTOP: case DIWHIGH:
        s->diw_hstart = a->pf.diw_hstart; s->diw_hstop = a->pf.diw_hstop;   /* approximation: use final */
        break;
    default: break;
    }
}

static const int pf2of_table[8] = { 0, 2, 4, 8, 16, 32, 64, 128 };

/* Collision detection, after UAE (E-UAE's do_sprite_collisions and
   do_playfield_collisions; WinUAE's denise_collide_sprites, with the CLXCON
   masks as in expand_colmask): worked out on the pixels as they are drawn,
   inside the display window, so it sees the same scroll and window as the
   picture. Sprite groups: group g is sprite 2g, plus sprite 2g+1 if CLXCON
   enables it. */

/* groups present at a pixel (bit g) -> sprite-to-sprite bits 9..14 of CLXDAT
   (E-UAE's sprclx table) */
static const uint16_t clx_sprspr[16] = {
    0, 0, 0, 0x0200, 0, 0x0400, 0x1000, 0x1600,
    0, 0x0800, 0x2000, 0x2A00, 0x4000, 0x4C00, 0x7000, 0x7E00
};

static void render_segment(aga_t *a, segstate_t *s, uint32_t *out, int hx0, int hx1, int have_data, int x0, int nbits, int clx)
{
    playfield_t *p = &a->pf;
    int res = p->line_res;
    int planes = p->line_planes;
    uint16_t con0 = s->con0;
    int ecsena = con0 & BPLCON0_ECSENA;
    int ham = (con0 & BPLCON0_HAM) && (planes == 6 || planes == 8);
    int dpf = (con0 & BPLCON0_DPF) != 0;
    int ehb = (planes == 6) && !ham && !dpf && !(s->con2 & BPLCON2_KILLEHB);
    int brdrblnk = ecsena && (s->con3 & BPLCON3_BRDRBLNK);
    int brdsprt = ecsena && (s->con3 & BPLCON3_BRDSPRT);
    int pf1p = s->con2 & 7, pf2p = (s->con2 >> 3) & 7;
    int pf2pri = s->con2 & BPLCON2_PF2PRI;
    int pf2of = pf2of_table[(s->con3 >> 10) & 7];
    unsigned xorv = (s->con4 >> 8) & 0xFF;
    /* BPLCON4 sprite colour bases, as WinUAE applies them (drawing.cpp): the
       high nibble is the base for a pixel of the lower-numbered (even) sprite of
       a pair, the low nibble for the higher-numbered (odd) one and for an
       attached pair. With the reset value $11 both are 16, which is why only
       games that program BPLCON4 (Turrican 2: $65) tell the two apart. */
    int base_even = ((s->con4 >> 4) & 15) << 4, base_odd = (s->con4 & 15) << 4;
    int vdiw = p->vdiw;
    const uint8_t *spr = a->sprline_valid ? a->sprline : NULL;
    int sprlen = (int)sizeof a->sprline;
    int d1 = 0, d2 = 0;
#ifndef SCROLL_MODEL_BLOCK
    if (have_data) delays_for(p, s->con1, &d1, &d2);
#endif
    /* collisions: enabled planes, the value they must match, sprites taking part */
    const uint8_t *sprm = (clx && a->sprclx_valid) ? a->sprclx : NULL;
    unsigned c_ena = 0, c_match = 0, c_emask = 0;
    uint16_t cx = a->clxdat & 1;                      /* bit 0 already found this frame: skip that search */
    if (clx) {
        c_ena   = ((a->clxcon >> 6) & 0x3F) | (a->clxcon2 & 0xC0);
        c_match = ((a->clxcon & 0x3F) | ((a->clxcon2 & 3) << 6)) & c_ena;
        c_emask = 0x55 | ((a->clxcon >> 11) & 0x02) | ((a->clxcon >> 10) & 0x08)   /* ENSP1, ENSP3 */
                       | ((a->clxcon >> 9) & 0x20) | ((a->clxcon >> 8) & 0x80);    /* ENSP5, ENSP7 */
    }

    /* ---- the fast path: no collisions, no HAM ----------------------------
       The same pixels as the general loop below, with the per-pixel decisions
       hoisted out: the display window is a range of hx rather than a test per
       pixel, a lores pixel is decoded once for the two output pixels it
       covers, and a raw pixel value is decoded by one table lookup
       (pfx_table, 1.114). Reshoot Proxima 3's heavy levels overran the 20 ms
       frame on a CM4 with this loop at 40% of the chipset core's time.
       Byte-identical to the general loop; the host regression frames are the
       guard. */
    if (!clx && !ham) {
        const int L2 = AGA_OUT_LEFT_LORES * 2;
        int w0 = hx1, w1 = hx1;                       /* the window's pixels: [w0, w1) */
        if (vdiw) {
            int A = s->diw_hstart + DIW_HW_OFFSET * 4, B = s->diw_hstop + DIW_HW_OFFSET * 4;
            w0 = (A + 1) / 2 - L2;                    /* 2*hires >= A  <=>  hires >= ceil(A/2) */
            w1 = (B + 1) / 2 - L2;                    /* 2*hires <  B  <=>  hires <  ceil(B/2) */
            if (w0 < hx0) w0 = hx0;
            if (w0 > hx1) w0 = hx1;
            if (w1 > hx1) w1 = hx1;
            if (w1 < w0) w1 = w0;
        }
        uint32_t border = brdrblnk ? 0 : line_color[0];
        for (int side = 0; side < 2; side++) {       /* the border left and right of the window */
            int b0 = side ? w1 : hx0, b1 = side ? hx1 : w0;
            for (int hx = b0; hx < b1; hx++) {
                uint32_t col = border;
                if (spr && brdsprt) {
                    int sx = (L2 + hx) << 1;
                    uint8_t sv = (sx < sprlen) ? spr[sx] : 0;
                    if (sv) {
                        int pair = sv >> 6, c = sv & 15;
                        int idx = (sv & 0x10) ? (base_odd + c) : (((sv & 0x20) ? base_odd : base_even) + pair * 4 + c);
                        col = line_color[idx & 255];
                    }
                }
                out[hx] = col;
            }
        }
        if (res == RES_SHRES && !dpf_tab_ready) dpf_tab_init();
        const uint16_t *pfx = pfx_table(dpf, ehb, pf2pri != 0, pf2of, xorv, pf1p, pf2p);
        /* A sprite pixel lies only where the sprite line was written
           (sprline_lo..sprline_hi), so the window left and right of that is
           drawn with no sprite test and no priority kept (RGB2RTG 0.75-dev13:
           on a Pi 3 the test at every output pixel was half of Jim Power's
           pixel loop). Where the window is cut changes no pixel: a lores pixel
           decodes the same from either of its two output pixels. */
        int s0 = w1, s1 = w1;
        if (res == RES_SHRES) { s0 = w0; s1 = w1; }   /* the super-hires pairs keep their own test */
        else if (spr && a->sprline_hi > a->sprline_lo) {
            s0 = (a->sprline_lo + 1) / 2 - L2;        /* 2*(L2+hx) >= lo  <=>  L2+hx >= ceil(lo/2) */
            s1 = (a->sprline_hi + 1) / 2 - L2;
            if (s0 < w0) s0 = w0;
            if (s0 > w1) s0 = w1;
            if (s1 > w1) s1 = w1;
            if (s1 < s0) s1 = s0;
        }
        for (int part = 0; part < 3; part++) {
            int p0 = part == 0 ? w0 : part == 1 ? s0 : s1;
            int p1 = part == 0 ? s0 : part == 1 ? s1 : w1;
            if (part != 1) {                          /* no sprite here */
                for (int hx = p0; hx < p1; ) {
                    int hires = L2 + hx;
                    unsigned v = 0;
                    if (have_data) {
                        int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : (hires << 1);
                        int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                        if ((unsigned)i1 < (unsigned)nbits) v = p->idx_odd[i1];
                        if ((unsigned)i2 < (unsigned)nbits) v |= p->idx_even[i2];
                    }
                    unsigned e = pfx[v & 255];
                    uint32_t col = (e & 0x100) ? line_ehb[e & 31] : line_color[e & 255];
                    out[hx++] = col;
                    if (res == 0 && !(hires & 1) && hx < p1) out[hx++] = col;
                }
                continue;
            }
            for (int hx = p0; hx < p1; ) {
                int hires = L2 + hx;
                if (res == RES_SHRES) {
                    /* two super-hires pixels to each output pixel: both decoded in
                       full, each with its own sprite pixel, and mixed (mix2) */
                    uint32_t c2[2];
                    for (int sub = 0; sub < 2; sub++) {
                        int sx = (hires << 1) + sub;
                        unsigned vs = 0;
                        if (have_data) {
                            int i1 = sx - x0 - d1, i2 = sx - x0 - d2;
                            if ((unsigned)i1 < (unsigned)nbits) vs = p->idx_odd[i1];
                            if ((unsigned)i2 < (unsigned)nbits) vs |= p->idx_even[i2];
                        }
                        int pv, fr;
                        uint32_t c = pf_pixel(vs, dpf, ehb, pf2pri, pf2of, xorv, &pv, &fr);
                        if (spr) c = spr_pixel(c, spr, sprlen, sx, pv, fr, pf1p, pf2p, base_even, base_odd);
                        c2[sub] = c;
                    }
                    out[hx++] = mix2(c2[0], c2[1]);
                    continue;
                }
                unsigned v = 0;
                if (have_data) {
                    int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : (hires << 1);
                    int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                    if ((unsigned)i1 < (unsigned)nbits) v = p->idx_odd[i1];
                    if ((unsigned)i2 < (unsigned)nbits) v |= p->idx_even[i2];
                }
                unsigned e = pfx[v & 255];
                uint32_t col = (e & 0x100) ? line_ehb[e & 31] : line_color[e & 255];
                int thr = (int)(e >> 9);
                /* a lores pixel covers two output pixels: decode once, overlay twice */
                int n = (res == 0 && !(hires & 1) && hx + 1 < p1) ? 2 : 1;
                for (int k = 0; k < n; k++, hx++) {
                    uint32_t c = col;
                    if (spr) {
                        int sx = (hires + k) << 1;
                        uint8_t sv = (sx < sprlen) ? spr[sx] : 0;
                        if (sv) {
                            int pair = sv >> 6;
                            if (pair < thr) {
                                int cc = sv & 15;
                                int idx = (sv & 0x10) ? (base_odd + cc) : (((sv & 0x20) ? base_odd : base_even) + pair * 4 + cc);
                                c = line_color[idx & 255];
                            }
                        }
                    }
                    out[hx] = c;
                }
            }
        }
        return;
    }

    for (int hx = hx0; hx < hx1; hx++) {
        int hires = AGA_OUT_LEFT_LORES * 2 + hx;      /* hires hardware coordinate (hpos*4 based) */
        /* super-hires: two pixels to each output pixel, decoded one after
           the other (a HAM colour carries from the first into the second)
           and mixed (mix2); the collision search looks at the first, as
           it always has. Lores and hires take one pass, as before. */
        int subs = (res == RES_SHRES) ? 2 : 1;
        uint32_t c2[2];
        for (int sub = 0; sub < subs; sub++) {
            /* DIW register values are offset by DIW_DDF_OFFSET-1 from hardware coordinates (UAE) */
            int sx = (hires << 1) + sub;                  /* superhires hardware coordinate */
            int in_window = vdiw && sx >= s->diw_hstart + DIW_HW_OFFSET * 4
                                 && sx <  s->diw_hstop  + DIW_HW_OFFSET * 4;
            uint32_t col;
            int pfv = 0;             /* playfield pixel value for sprite priority */
            int front_pf = 2;        /* which playfield is in front for priority (1 or 2) */
            unsigned v = 0;          /* raw bitplane value, bit n = plane n+1 */
            int fetched = 0;         /* the pixel lies in fetched bitplane data */

            if (in_window) {
                if (have_data) {
                    int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : sx;
                    int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                    if (i1 >= 0 && i1 < nbits) { v = p->idx_odd[i1]; fetched = 1; }
                    if (i2 >= 0 && i2 < nbits) { v |= p->idx_even[i2]; fetched = 1; }
                }
                if (dpf) {
                    unsigned v1 = (v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8);
                    unsigned v2 = ((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8);
                    /* The colour index is built first - the winning playfield's value,
                       plus PF2OF when that is playfield 2 - and BPLAM (BPLCON4's high
                       byte) is XORed into the whole 8-bit result afterwards, exactly as
                       WinUAE does (decode_pixel_aga, CMODE_DUALPF). It applies to both
                       playfields and to the background, so with BPLAM set the background
                       is colour BPLAM, not colour 0. XORing only the low four bits into
                       playfield 1 left Reshoot Proxima 3's dialogue box drawing from
                       colours 0-15 where the game had put its palette at 160 (BPLAM $A0
                       for the lines the box covers): a noisy portrait, a column of
                       colour down the screen and the wrong ground colours. */
                    unsigned idx;
                    if (pf2pri) {
                        if (v2)      { idx = v2 + pf2of; pfv = v2; front_pf = 2; }
                        else if (v1) { idx = v1;         pfv = v1; front_pf = 1; }
                        else           idx = 0;
                    } else {
                        if (v1)      { idx = v1;         pfv = v1; front_pf = 1; }
                        else if (v2) { idx = v2 + pf2of; pfv = v2; front_pf = 2; }
                        else           idx = 0;
                    }
                    col = line_color[(idx ^ xorv) & 255];
    #ifdef PFDBG
                    {
                        static int v0 = -2, x0dbg = 0, x1dbg = 0;
                        if (v0 == -2) { const char *e = getenv("PFDBG"); v0 = -1; if (e) sscanf(e, "%i,%i,%i", &v0, &x0dbg, &x1dbg); }
                        if (a->vpos == v0 && hx >= x0dbg && hx <= x1dbg)
                            fprintf(stderr, "x%3d v=%02x v1=%x v2=%x idx=%3d col=%06x con1=$%04x d1=%d d2=%d\n",
                                    hx, v, v1, v2, (idx ^ xorv) & 255, col, s->con1, d1, d2);
                    }
    #endif
                } else if (ham) {
                    pfv = v;
                    const uint32_t *hb = a->cfg.bright > 100 ? line_raw : line_color;   /* the real colours */
                    if (planes == 8) {
                        switch (v & 3) {
                        case 0: s->ham_last = hb[(v >> 2) ^ (xorv >> 2)]; break;
                        case 1: s->ham_last = (s->ham_last & 0xFFFF03) | (v & 0xFC); break;
                        case 2: s->ham_last = (s->ham_last & 0x03FFFF) | ((v & 0xFC) << 16); break;
                        default: s->ham_last = (s->ham_last & 0xFF03FF) | ((v & 0xFC) << 8); break;
                        }
                    } else {
                        unsigned n = v & 15;
                        unsigned n8 = n | (n << 4);
                        switch (v & 0x30) {
                        case 0x00: s->ham_last = hb[n ^ (xorv & 15)]; break;
                        case 0x10: s->ham_last = (s->ham_last & 0xFFFF00) | n8; break;
                        case 0x20: s->ham_last = (s->ham_last & 0x00FFFF) | (n8 << 16); break;
                        default:   s->ham_last = (s->ham_last & 0xFF00FF) | (n8 << 8); break;
                        }
                    }
                    col = a->cfg.bright > 100 ? bright_rgb(a, s->ham_last) : s->ham_last;
                } else if (ehb) {
                    pfv = v;
                    if (v & 0x20) col = line_ehb[(v & 31) ^ (xorv & 31)];
                    else          col = line_color[v ^ xorv];
                } else {
                    pfv = v;
                    col = line_color[(v ^ xorv) & 255];
                }
            } else {
                col = brdrblnk ? 0 : line_color[0];
            }

            /* sprites */
            if (spr && (in_window || brdsprt)) {
                uint8_t sv = (sx >= 0 && sx < sprlen) ? spr[sx] : 0;
                if (sv) {
                    int pair = sv >> 6;
                    int visible;
                    if (!in_window || pfv == 0) visible = 1;
                    else if (front_pf == 1)     visible = pair < pf1p;
                    else                        visible = pair < pf2p;
                    if (visible) {
                        int c = sv & 15;
                        int idx = (sv & 0x10) ? (base_odd + c) : (((sv & 0x20) ? base_odd : base_even) + pair * 4 + c);
                        col = line_color[idx & 255];
                    }
                }
            }

            /* once bit 0 is found, only pixels with a sprite on them need looking at */
            unsigned m = (clx && sprm && sub == 0) ? (sprm[sx] & c_emask) : 0;
            if (clx && sub == 0 && in_window && (m || !(cx & 1))) {
                unsigned g = 0;                           /* sprite groups with a pixel here */
                if (m) {
                    m = (m | (m >> 1)) & 0x55;
                    g = (m & 1) | ((m >> 1) & 2) | ((m >> 2) & 4) | ((m >> 3) & 8);
                    cx |= clx_sprspr[g];
                }
                if (fetched) {
                    /* a disabled plane always matches, so no enabled plane = always */
                    unsigned d = (v ^ c_match) & c_ena;
                    unsigned even_ok = !(d & 0xAA), odd_ok = !(d & 0x55);
                    if (even_ok && odd_ok) cx |= 0x0001;  /* even to odd bitplanes */
                    if (!dpf) odd_ok &= even_ok;          /* one playfield: the odd bits need all planes (WinUAE) */
                    if (odd_ok)  cx |= (uint16_t)(g << 1);
                    if (even_ok) cx |= (uint16_t)(g << 5);
                }
            }
            c2[sub] = col;
        }
        out[hx] = (subs == 2) ? mix2(c2[0], c2[1]) : c2[0];
    }
    a->clxdat |= cx;
}

void pf_finish_line(aga_t *a)
{
    playfield_t *p = &a->pf;
    /* A fetch still running here missed its end of line: a VPOSW on a long
       NTSC line made it a short one after the fetch had already gone past
       the new last clock. Its last block and the modulos are due all the
       same. */
    if (p->fetch_active) end_fetch(a, a->maxhpos - 1);
    int y = a->vpos - AGA_OUT_TOP;
    if (!a->cfg.render || y < 0 || y >= AGA_OUT_H) return;

    uint32_t *out = a->linebuf;
    int have_data = p->line_blocks > 0 && p->line_planes > 0;
    if (have_data && p->vdiw) {
        a->display_lines++;
        /* the window in the frame's hires pixels, as the fast path below
           works it out, before any clipping to the frame (Size Auto) */
        int wa = (p->diw_hstart + DIW_HW_OFFSET * 4 + 1) / 2 - AGA_OUT_LEFT_LORES * 2;
        int wb = (p->diw_hstop + DIW_HW_OFFSET * 4 + 1) / 2 - AGA_OUT_LEFT_LORES * 2;
        if (wa < a->win_x0) a->win_x0 = wa;
        if (wb > a->win_x1) a->win_x1 = wb;
        if (y < a->win_y0) a->win_y0 = y;
        if (y > a->win_y1) a->win_y1 = y;
    }
    /* a skipped frame draws nothing - unless collisions switched on during
       it, since CLXDAT is computed by the renderer below */
    if (a->frame_skip && !(a->cfg.collisions || a->clx_frames > 0)) return;
    int nbits = 0, x0 = 0;
    if (have_data) {
        int W = 16 << p->line_fm;
        nbits = p->line_blocks * W + 64;
        if (nbits > AGA_PLANE_WORDS * 64) nbits = AGA_PLANE_WORDS * 64;
        build_bits(p);
        planar_to_chunky(p, nbits);
        x0 = (p->line_plfleft * 2 + DISP_OFFSET) << p->line_res;
    }

    int clx = a->cfg.collisions || a->clx_frames > 0;
    int any_sprite = 0;
    for (int i = 0; i < AGA_MAX_SPRITES; i++) if (a->spr[i].armed) { any_sprite = 1; break; }
    /* a sprite written during the line may have shown and been disarmed since */
    if (any_sprite || a->spr_touched) spr_build_line(a, clx); else a->sprline_valid = a->sprclx_valid = 0;

    segstate_t s;
    s.con0 = line_bplcon0; s.con1 = line_bplcon1; s.con2 = line_bplcon2;
    s.con3 = line_bplcon3; s.con4 = line_bplcon4;
    s.diw_hstart = p->diw_hstart; s.diw_hstop = p->diw_hstop;
    s.ham_last = a->cfg.bright > 100 ? line_raw[0] : line_color[0];

    int hx = 0;
    for (int i = 0; i < a->nchanges; i++) {
        const linechange_t *c = &a->changes[i];
        /* change takes effect at lores pixel 2*hpos plus a small pipeline delay
           (BPLCON1 scroll changes are seen 3 lores pixels after the write, colours 1) */
        int chx = (int)c->hpos * 4 + (c->reg == BPLCON1 ? 6 : 2) - AGA_OUT_LEFT_LORES * 2;
        if (chx > AGA_OUT_W) chx = AGA_OUT_W;
        if (chx > hx) {
            render_segment(a, &s, out, hx, chx, have_data, x0, nbits, clx);
            hx = chx;
        }
        apply_change(a, &s, c);
    }
    if (hx < AGA_OUT_W) render_segment(a, &s, out, hx, AGA_OUT_W, have_data, x0, nbits, clx);
    /* the finished line, in one pass: an external page (uncached HVS memory)
       gets the alpha byte the plane's alpha mode needs, the core's own
       frames stay byte-identical to what the host tests compare */
    uint32_t *row;
    if (a->weave_now)      /* woven: the two pages as one 572-row frame (aga_set_weave) */
        row = a->fb[0] + (2 * y + a->weave_row) * AGA_OUT_W;
    else
        row = a->fb[a->fb_cur] + y * AGA_OUT_W;
    for (int twice = a->weave_now && a->weave_fill; ; twice = 0) {
        if (a->fb_external) { for (int i = 0; i < AGA_OUT_W; i++) row[i] = out[i] | 0xFF000000u; }
        else memcpy(row, out, sizeof a->linebuf);
        if (!twice) break;
        /* the first woven frame: the row between too, not an old picture */
        row = a->fb[0] + (2 * y + (a->weave_row ^ 1)) * AGA_OUT_W;
    }
    /* the line, still in my cache, for whoever gives a damn (the RGB port) */
    if (a->hooks.line_done && !a->weave_now) a->hooks.line_done(a->hooks.user, y, a->fb_cur, out);
}
