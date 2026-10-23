/* AGA-PISTORM - custom chip register file, beam counters, DMA/interrupt control.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Mostly my own. From WinUAE custom.cpp: setclr and the CLXDAT read
 * (copied); a few short lines translated (the DMACON mask, VPOSW and its LOL
 * reset, the frame length with LOF, NTSC's long and short lines, the COPJMP
 * strobe on a read, the copper idle test); the refresh slot positions and the
 * read-back defaults.
 *   WinUAE custom.cpp: Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "internal.h"
#include "tone_tab.h"

/* --------------------------------------------------------------------------
 * logging
 * ------------------------------------------------------------------------ */
#ifdef AGA_PISTORM
extern void vkprintf(const char *format, va_list args);
void aga_logf(aga_t *a, const char *fmt, ...)
{
    va_list ap;
    (void)a;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}
#else
void aga_logf(aga_t *a, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    if (!a->log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    a->log(a->log_user, buf);
}
#endif

void aga_set_log(aga_t *a, void (*log)(void *user, const char *msg), void *user)
{
    a->log = log;
    a->log_user = user;
}

/* --------------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------------ */
aga_t *aga_create(uint8_t *chipram, uint32_t chipram_size, const aga_config_t *cfg)
{
    aga_t *a = (aga_t *)calloc(1, sizeof *a);
    if (!a) return NULL;
    dsk_create(a);
#ifdef AGA_TRACE
    a->trace[0] = (aga_slot_t *)calloc((size_t)AGA_MAX_VPOS * AGA_MAX_HPOS, sizeof(aga_slot_t));
    a->trace[1] = (aga_slot_t *)calloc((size_t)AGA_MAX_VPOS * AGA_MAX_HPOS, sizeof(aga_slot_t));
#endif
    if (cfg) a->cfg = *cfg;
    else { a->cfg.blit_immediate = 1; a->cfg.render = 1; }
    a->chip = chipram;
    a->fb[0] = a->fb_store[0];
    a->fb[1] = a->fb_store[1];
    a->fb[2] = NULL;
    a->fb_count = 2;
    a->fb_cur = 0;
    a->fb_done = 0;
#ifdef AGA_TRACE
    a->trace_cur = 0;
    a->trace_done = 0;
#endif
    /* chipram_size must be a power of two */
    a->chipmask = chipram_size - 1;
    a->ntsc = a->cfg.ntsc;
    aga_reset(a);
    return a;
}

void aga_destroy(aga_t *a)
{
    dsk_destroy(a);
#ifdef AGA_TRACE
    free(a->trace[0]);
    free(a->trace[1]);
#endif
    free(a);
}

void aga_set_hooks(aga_t *a, const aga_hooks_t *hooks)
{
    if (hooks) a->hooks = *hooks;
    else memset(&a->hooks, 0, sizeof a->hooks);
}

/* NTSC lines alternate long (228 colour clocks) and short (227) - VPOSR's
   LOL - line after line and across frames, unless BEAMCON0 LOLDIS (WinUAE
   toggles lol at the end of every line the same way). Measured on an A1200
   in NTSC (RGB2RTG 981a3c7): hpos $E3 only ever on lines with LOL, 263
   lines, and line 1's LOL flipping from one frame to the next. PAL lines are
   all 227. */
static inline int line_toggles(const aga_t *a)
{
    return a->ntsc && !(a->beamcon0 & BEAMCON0_LOLDIS);
}

static void set_line_length(aga_t *a)
{
    if (!line_toggles(a))
        a->lol = 0;
    a->maxhpos = (a->ntsc ? AGA_NTSC_HPOS : AGA_PAL_HPOS) + a->lol;
}

static void set_geometry(aga_t *a)
{
    if (a->ntsc) {
        a->maxvpos = AGA_NTSC_VPOS;
        a->vblank_end = AGA_VBLANK_END_NTSC;
        a->sprite_vblank = AGA_SPRITE_VBLANK_NTSC;
    } else {
        a->maxvpos = AGA_PAL_VPOS;
        a->vblank_end = AGA_VBLANK_END_PAL;
        a->sprite_vblank = AGA_SPRITE_VBLANK_PAL;
    }
    set_line_length(a);
}

/* RGB2RTG's Gamma and Brightness (0.75-dev47, its tools/mktone.py), one
   table: each channel through the gamma (tone_tab.h, in 1/65536), then the
   brightness g - the gain up to a knee at x0 = 2/g - 1 and above it a
   parabola with the same slope there and none at white, ending at white:
   y = g x0 + g d - a d^2, d = x - x0, a = g / (2 (1 - x0)). It rises all the
   way, so near-whites stay apart; straight gain (up to 1.115) clipped every
   value from 255 / g up to white - at 150% from 170. Integer only. */
int aga_tone_build(uint8_t lut[256], int percent, int gamma)
{
    const uint32_t *gt = tone_gamma[gamma >= 0 && gamma < TONE_GAMMAS ? gamma : 0];
    int64_t pct = percent > 100 ? (percent > 200 ? 200 : percent) : 100;
    int64_t G = pct * 65536 / 100;                          /* gain, 1/65536 */
    int64_t X0 = 200 * 65536 / pct - 65536;                 /* the knee: 2/g - 1 */
    int64_t A = X0 < 65536 ? G * 65536 / (2 * (65536 - X0)) : 0;
    int ident = 1;
    for (int v = 0; v < 256; v++) {
        int64_t x = gt[v], y;
        if (x <= X0) y = G * x >> 16;
        else {
            int64_t d = x - X0;
            y = (G * X0 >> 16) + (G * d >> 16) - (A * (d * d >> 16) >> 16);
        }
        int64_t o = (y * 255 + 32768) >> 16;
        lut[v] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o);
        if (lut[v] != v) ident = 0;
    }
    return !ident;
}

/* the table for cfg.bright and cfg.gamma; the palette through it is
   rebuilt at the next line */
static void bright_build(aga_t *a)
{
    a->tone_on = aga_tone_build(a->bright_lut, a->cfg.bright, a->cfg.gamma);
    a->bright_dirty = 1;
}

void aga_reset(aga_t *a)
{
    uint8_t *chip = a->chip;
    uint32_t mask = a->chipmask;
    aga_config_t cfg = a->cfg;
    aga_hooks_t hooks = a->hooks;
    void (*log)(void *, const char *) = a->log;
    void *log_user = a->log_user;
    disk_t *dsk = a->dsk;
    int ecs = a->pf.ecs_mode, ecs_forced = a->pf.ecs_forced;    /* chipset personality: see below */
    int nohold = a->blt.no_hold; /* a test switch set before the entry that resets the chipset */
    uint32_t *fb0 = a->fb[0], *fb1 = a->fb[1], *fb2 = a->fb[2];
    int fbx = a->fb_external, fbc = a->fb_count;
#ifdef AGA_TRACE
    aga_slot_t *tr0 = a->trace[0], *tr1 = a->trace[1];
#endif

    memset(a, 0, sizeof *a);
    a->blt.no_hold = nohold;
    a->dsk = dsk;
    a->fb[0] = fb0 ? fb0 : a->fb_store[0];
    a->fb[1] = fb1 ? fb1 : a->fb_store[1];
    a->fb[2] = fb2;
    a->fb_external = fbx;
    a->fb_count = (fbc == 3 && fb2) ? 3 : 2;
    a->fb_cur = 0;
    a->fb_done = 0;
#ifdef AGA_TRACE
    a->trace[0] = tr0;
    a->trace[1] = tr1;
    a->trace_cur = 0;
    a->trace_done = 0;
#endif
    dsk_reset(a);
    a->chip = chip;
    a->chipmask = mask;
    a->cfg = cfg;
    a->hooks = hooks;
    a->log = log;
    a->log_user = log_user;
    a->ntsc = cfg.ntsc;
    a->ntsc_strap = cfg.ntsc;
    a->beamcon0 = a->ntsc ? 0 : BEAMCON0_DISPLAYPAL;
    a->win_x0 = a->win_y0 = a->last_win_x0 = a->last_win_y0 = 1 << 20;        /* no window yet */
    a->win_x1 = a->win_y1 = a->last_win_x1 = a->last_win_y1 = -(1 << 20);
    set_geometry(a);
    bright_build(a);

    a->lof = 1;
    a->hpos = 0;
    a->vpos = 0;

    /* Which chipset I present is a property of the game about to run, not
       of the chipset state, so it has to outlive the memset above - the
       sandbox picks the mode and THEN enters, and entering resets the whole
       damn thing. pf_reset keeps it across its own memset too, but only reads
       what is here, so this line is what makes the ECS toggle work at all. */
    a->pf.ecs_mode = ecs;
    a->pf.ecs_forced = ecs_forced;

    cop_reset(a);
    blt_reset(a);
    pf_reset(a);
    spr_reset(a);
    aud_reset(a);
    for (int i = 0; i < 256; i++) a->color[i] = 0;
    aga_update_ipl(a);
    pf_start_line(a);
    cop_start_line(a);
}

/* --------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------ */
int aga_dma_enabled(const aga_t *a, uint16_t mask)
{
    return (a->dmacon & DMAF_DMAEN) && (a->dmacon & mask);
}

void aga_record_change(aga_t *a, uint32_t reg, uint32_t value)
{
    if (a->nchanges < AGA_LINE_CHANGES) {
        linechange_t *c = &a->changes[a->nchanges++];
        c->hpos = (uint16_t)a->hpos;
        c->reg = (uint16_t)reg;
        c->value = value;
    }
}

static const uint8_t intlevel[15] = {
    1, 1, 1, 2,        /* TBE DSKBLK SOFT PORTS */
    3, 3, 3,           /* COPER VERTB BLIT */
    4, 4, 4, 4,        /* AUD0..3 */
    5, 5,              /* RBF DSKSYN */
    6,                 /* EXTER */
    0
};

void aga_update_ipl(aga_t *a)
{
    int ipl = 0;
    if (a->intena & INTF_INTEN) {
        uint16_t pending = a->intreq & a->intena & 0x3FFF;
        for (int i = 13; i >= 0; i--) {
            if (pending & (1 << i)) { ipl = intlevel[i]; break; }
        }
    }
    if (ipl != a->ipl) {
        a->ipl = ipl;
        if (a->hooks.ipl_changed) a->hooks.ipl_changed(a->hooks.user, ipl);
    }
}

void aga_intreq_set(aga_t *a, uint16_t bits)
{
    a->intreq |= bits & 0x7FFF;
    aga_update_ipl(a);
}

static void setclr(uint16_t *reg, uint16_t v)
{
    if (v & 0x8000) *reg |= v & 0x7FFF;
    else            *reg &= ~(v & 0x7FFF);
}

/* --------------------------------------------------------------------------
 * register reads
 * ------------------------------------------------------------------------ */
static uint16_t ext_read(aga_t *a, uint32_t reg, uint16_t dflt)
{
    if (a->hooks.ext_read) return a->hooks.ext_read(a->hooks.user, reg);
    return dflt;
}

uint16_t aga_custom_rget(aga_t *a, uint32_t reg)
{
    reg &= 0x1FE;
    switch (reg) {
    case BLTDDAT:  return a->blt.ddat;
    case DMACONR:  return (a->dmacon & 0x07FF) | blt_dmaconr_bits(a);
    case VPOSR: {
        /* The ID is the chip's PAL/NTSC strap, whatever BEAMCON0 has made of
           it: a PAL Alice switched to NTSC still reads $23, an NTSC one $33
           (WinUAE sets the bit from its NTSC setting, not from BEAMCON0) */
        uint16_t v = (a->ntsc_strap ? VPOSR_ID_AGA_NTSC : VPOSR_ID_AGA_PAL);
        v |= (a->lof ? 0x8000 : 0);
        v |= a->lol ? VPOSR_LOL : 0;
        v |= (a->vpos >> 8) & 7;
        return v;
    }
    case VHPOSR:   return (uint16_t)(((a->vpos & 0xFF) << 8) | (a->hpos & 0xFF));
    case DSKDATR: { uint16_t v; if (dsk_read(a, reg, &v)) return v; return ext_read(a, reg, 0); }
    case JOY0DAT:  return ext_read(a, reg, 0);
    case JOY1DAT:  return ext_read(a, reg, 0);
    case CLXDAT: {
        /* read and clear; bit 15 is unused and reads as 1 (WinUAE). A program
           that reads it wants collisions: work them out for the next frames */
        uint16_t v = a->clxdat | 0x8000;
        a->clxdat = 0;
        a->clx_frames = CLX_FRAMES;
        return v;
    }
    case ADKCONR:  return a->adkcon;
    case POT0DAT:  return ext_read(a, reg, 0);
    case POT1DAT:  return ext_read(a, reg, 0);
    case POTGOR:   return ext_read(a, reg, 0xFF00);
    case SERDATR:  return ext_read(a, reg, 0x3000);   /* TBE | TSRE */
    case DSKBYTR: { uint16_t v; if (dsk_read(a, reg, &v)) return v; return ext_read(a, reg, 0); }
    case INTENAR:  return a->intena & 0x7FFF;
    case INTREQR:  return a->intreq & 0x7FFF;
    case DENISEID:
#ifdef FRF_VIRTUAL_AGA
        return a->pf.ecs_mode ? 0xffffu : DENISEID_LISA;
#else
        return DENISEID_LISA;
#endif
    case HHPOSR:   return 0;
    case COPINS:   return 0;
    case COPJMP1:
    case COPJMP2:
        /* The chipset strobes COPJMP on any access, a read as much as a
           write (WinUAE custom_wget_1). Bubble Bobble starts its copper list
           with MOVE.W $DFF088,D0 (found by RGB2RTG bc4c93d). Only the 68k
           gets here: a diagnostic that wants register values without side
           effects reads aga_peek_reg. The value is the last one on the bus;
           0 here, as for the other write-only registers. */
        cop_strobe(a, reg == COPJMP1 ? 1 : 2);
        return 0;
    default:
        /* write-only registers read back as the last value on the data bus; 0 here */
        return 0;
    }
}

uint32_t aga_read(aga_t *a, uint32_t addr, int size)
{
    uint32_t reg = addr & 0x1FF;
    switch (size) {
    case 1: {
        uint16_t w = aga_custom_rget(a, reg & 0x1FE);
        return (reg & 1) ? (w & 0xFF) : (w >> 8);
    }
    case 2:
        return aga_custom_rget(a, reg);
    default:
        return ((uint32_t)aga_custom_rget(a, reg) << 16) | aga_custom_rget(a, reg + 2);
    }
}

/* --------------------------------------------------------------------------
 * register writes
 * ------------------------------------------------------------------------ */
static void DMACON_write(aga_t *a, uint16_t v)
{
    a->dma_log[a->dma_log_head & 7] =
        (uint32_t)v | (a->cop.in_move ? 0x10000u : 0)
        | ((uint32_t)(a->frame & 0x7FFF) << 17);
    a->dma_log_head++;
    uint16_t old = a->dmacon;
    uint16_t nv = old;
    setclr(&nv, v & 0x87FF);
    nv &= 0x07FF;
    a->dmacon = nv;
    if ((old ^ nv) & (DMAF_AUDEN | DMAF_DMAEN))
        aud_dmacon_changed(a, old, nv);
    /* audio DMA bits may be mirrored to a real Paula by the glue */
    if (a->hooks.ext_write && (v & (DMAF_AUDEN | DMAF_DMAEN)))
        a->hooks.ext_write(a->hooks.user, DMACON, v);
    if (!(old & DMAF_COPEN) && (nv & DMAF_COPEN) && (nv & DMAF_DMAEN)) {
        /* copper DMA switched on: it continues from where it is */
    }
    /* BLTPRI has no effect with an immediate blitter */
}

static void INTENA_write(aga_t *a, uint16_t v)
{
    setclr(&a->intena, v);
    /* the glue mirrors the real-hardware sources (PORTS/EXTER) to the real Paula */
    if (a->hooks.ext_write) a->hooks.ext_write(a->hooks.user, INTENA, v);
    aga_update_ipl(a);
}

static void INTREQ_write(aga_t *a, uint16_t v)
{
    if (!(v & 0x8000) && (v & INTF_BLIT)) {
        /* one acknowledge for two finished blits: an interrupt was swallowed */
        if (a->blt.fin_count - a->blt.acked_fin >= 2) { a->blt.lost++; a->blt.lost_frame = (uint32_t)a->frame; }
        a->blt.acked_fin = a->blt.fin_count;
    }
    setclr(&a->intreq, v & 0xBFFF);
    /* A BLIT acknowledge clears whatever completions it finds, as on the
       hardware. Two attempts to protect a program that starts its next blit
       before acknowledging the last (Synergy's chunky-to-planar chain, which
       lost a completion that way on a thermally throttled CM4) both broke
       Banshee within 20 seconds: holding a completion back until the next
       acknowledge, and raising again a completion that came after the
       program's last INTREQR read. Banshee leaves BLIT pending and
       acknowledges in bulk, so either way it got extra BLIT interrupts and
       its background-restore blits went missing - black rectangles over the
       sea. The counters above still say when an acknowledge swallowed one. */
    if (a->hooks.ext_write && !(v & 0x8000) && (v & (INTF_PORTS | INTF_EXTER))) {
        /* forward clears of the mirrored real-hardware interrupt bits */
        a->hooks.ext_write(a->hooks.user, INTREQ, v & (INTF_PORTS | INTF_EXTER));
    }
    aga_update_ipl(a);
}

void aga_custom_wput(aga_t *a, uint32_t reg, uint16_t v)
{
    reg &= 0x1FE;
    a->regs[reg >> 1] = v;

    if (reg >= COLOR00 && reg < COLOR00 + 64) {
        pf_color_write(a, (reg - COLOR00) >> 1, v, a->hpos);
        return;
    }
    if (reg >= BLTCON0 && reg <= BLTADAT) {
        blt_write(a, reg, v);
        return;
    }
    if (reg >= AUD0LCH && reg < AUD0LCH + 4 * 16) {
        aud_write(a, reg, v);
        return;
    }
    if (reg >= SPR0PTH && reg < SPR0POS + 8 * 8) {
        spr_write(a, reg, v);
        return;
    }
    if ((reg >= BPL1PTH && reg < BPL1PTH + 8 * 4) || (reg >= BPLCON0 && reg <= CLXCON2 && reg != CLXCON2)
        || (reg >= BPL1DAT && reg < BPL1DAT + 16) || reg == DIWSTRT || reg == DIWSTOP
        || reg == DDFSTRT || reg == DDFSTOP || reg == DIWHIGH || reg == FMODE) {
        pf_write(a, reg, v, a->hpos);
        return;
    }

    switch (reg) {
    case DSKPTH: case DSKPTL: case DSKLEN: case DSKDAT: case DSKSYNC:
        if (dsk_write(a, reg, v)) break;
        /* fall through: real drives */
    case SERDAT: case SERPER: case POTGO: case JOYTEST:
        if (a->hooks.ext_write) a->hooks.ext_write(a->hooks.user, reg, v);
        break;
    case REFPTR: break;
    case VPOSW:
        a->lof = (v & 0x8000) ? 1 : 0;
        /* A write resets LOL whatever its bit 7 says: this line becomes a
           short one and the lines go on alternating from there (WinUAE VPOSW,
           which has it from every NTSC Agnus and from ECS/AGA in NTSC) */
        if (a->lol) {
            a->lol = 0;
            set_line_length(a);
        }
        a->vpos = (a->vpos & 0xFF) | ((v & 7) << 8);
        break;
    case VHPOSW:
        a->vpos = (a->vpos & 0x700) | (v >> 8);
        break;
    case COPCON:
        a->copcon = v;
        a->cop.danger = (v & 2) ? 1 : 0;
        break;
    case STREQU: case STRVBL: case STRHOR: case STRLONG: break;
    case COP1LCH: cop_write_lc(a, 1, 1, v); break;
    case COP1LCL: cop_write_lc(a, 1, 0, v); break;
    case COP2LCH: cop_write_lc(a, 2, 1, v); break;
    case COP2LCL: cop_write_lc(a, 2, 0, v); break;
    case COPJMP1: cop_strobe(a, 1); break;
    case COPJMP2: cop_strobe(a, 2); break;
    case COPINS: break;
    case DMACON: DMACON_write(a, v); break;
    case CLXCON:
        a->clxcon = v;
        a->clxcon2 &= ~(0x40 | 0x80);  /* writing CLXCON clears CLXCON2 sprite bits */
        break;
    case CLXCON2: a->clxcon2 = v; break;
    case INTENA: INTENA_write(a, v); break;
    case INTREQ: INTREQ_write(a, v); break;
    case ADKCON:
        setclr(&a->adkcon, v);
        if (a->hooks.ext_write) a->hooks.ext_write(a->hooks.user, reg, v);
        break;
    case BEAMCON0:
        a->bc0_log[a->bc0_log_head & 7] =
            (uint32_t)v | (a->cop.in_move ? 0x10000u : 0)
            | ((uint32_t)(a->frame & 0x7FFF) << 17);
        a->bc0_log_head++;
        /* The register takes the value at once, the frame does NOT change size
           until the next one starts: Agnus latches the timing at the top of a
           frame, and WinUAE does the same (init_hz from the vsync handler).
           Worms: The Director's Cut writes BEAMCON0 twice in one frame, first
           with the PAL bit clear and then with it set; changing the geometry on
           the first of those left me in NTSC - 263 lines - while the game went
           on waiting for a PAL raster line that could no longer come round, and
           the screen stayed black for ever. */
        a->beamcon0 = v;
        break;
    case HTOTAL: case HSSTOP: case HBSTRT: case HBSTOP: case VTOTAL: case VSSTOP:
    case VBSTRT: case VBSTOP: case HSSTRT: case VSSTRT: case HCENTER:
    case SPRHSTRT: case SPRHSTOP: case BPLHSTRT: case BPLHSTOP: case HHPOSW:
    case BPLHMOD: case SPRHPTH: case SPRHPTL: case BPLHPTH: case BPLHPTL:
    case NOOP:
        break;
#ifdef FRF_VIRTUAL_AGA
    case 0x1F2: case 0x1F4: case 0x1F6: case 0x1F8: case 0x1FA:
        /* standalone machine: reserved/no-op; $1FC FMODE remains real AGA */
        break;
#else
    case 0x1F2:
        a->aga_ctrl = v;
        break;
    case 0x1F4: a->aga_win[0] = v; break;
    case 0x1F6: a->aga_win[1] = v; break;
    case 0x1F8: a->aga_win[2] = v; break;
    case 0x1FA: a->aga_win[3] = v; break;
#endif
    default:
        break;
    }
}

void aga_write(aga_t *a, uint32_t addr, uint32_t value, int size)
{
    uint32_t reg = addr & 0x1FF;
    switch (size) {
    case 1:
        /* byte writes to custom registers write the byte to both halves (bus behaviour) */
        aga_custom_wput(a, reg & 0x1FE, (uint16_t)((value & 0xFF) | ((value & 0xFF) << 8)));
        break;
    case 2:
        aga_custom_wput(a, reg, (uint16_t)value);
        break;
    default:
        aga_custom_wput(a, reg, (uint16_t)(value >> 16));
        aga_custom_wput(a, reg + 2, (uint16_t)value);
        break;
    }
}

/* --------------------------------------------------------------------------
 * beam
 * ------------------------------------------------------------------------ */
static void end_of_line(aga_t *a)
{
    cop_sync(a, a->maxhpos);
    pf_sync(a, a->maxhpos);
    pf_finish_line(a);
    aud_line_tick(a);
    dsk_line_tick(a);
    a->nchanges = 0;
}

const aga_slot_t *aga_dma_trace(const aga_t *a)
{
#ifdef AGA_TRACE
    return a->trace[a->trace_done];      /* the frame just completed */
#else
    (void)a;
    return 0;
#endif
}

/* at most this many frames skipped in a row, then one is drawn: a machine
   that is behind for seconds still shows 12 frames a second, not a still */
#define SKIP_RUN_MAX 3

static void end_of_frame(aga_t *a)
{
    /* the frame that starts now is the one that gets the new timing */
    {
        int want_ntsc = (a->beamcon0 & BEAMCON0_DISPLAYPAL) ? 0 : 1;
        if (want_ntsc != a->ntsc) { a->ntsc = want_ntsc; set_geometry(a); aud_clock(a); }
    }
    a->frame++;
    a->frame_done_count++;
    a->last_display_lines = a->display_lines;
    a->display_lines = 0;
    a->last_win_x0 = a->win_x0; a->last_win_x1 = a->win_x1;
    a->last_win_y0 = a->win_y0; a->last_win_y1 = a->win_y1;
    a->win_x0 = a->win_y0 = 1 << 20;
    a->win_x1 = a->win_y1 = -(1 << 20);
    a->last_frame_data = a->pf_data_nonzero;
    a->pf_data_nonzero = 0;
    if (a->clx_frames > 0) a->clx_frames--;
    if (a->frame_skip) {
        /* nothing new was drawn: keep the finished page where it is */
        a->frames_skipped++;
    } else {
        if (a->weave_now) {
            /* Interlace still uses contiguous pages 0+1 as one woven image.
               Page 2 is reserved for progressive rendering after weave. */
            a->fb_done = 0;
            a->fb_cur = (a->fb_count == 3) ? 2 : (a->fb_cur ^ 1);
        } else {
            a->fb_done = a->fb_cur;
            a->fb_cur++;
            if (a->fb_cur >= a->fb_count) a->fb_cur = 0;
        }
#ifdef AGA_TRACE
        a->trace_done = a->trace_cur;
        a->trace_cur ^= 1;
        if (a->trace[a->trace_cur])
            memset(a->trace[a->trace_cur], 0, (size_t)AGA_MAX_VPOS * AGA_MAX_HPOS * sizeof(aga_slot_t));
#endif
    }
    if (a->skip_req && a->skip_run < SKIP_RUN_MAX && !(a->cfg.collisions || a->clx_frames > 0)) {
        a->frame_skip = 1;
        a->skip_run++;
    } else {
        a->frame_skip = 0;
        a->skip_run = 0;
    }
    /* The flicker fixer: the finished frame, for the host - then the frame
       that starts now, woven if it is interlaced (LOF is already its own:
       the line loop toggled it before calling this) */
    a->weave_done = a->weave_now;
    a->weave_done_row = a->weave_row;
    {
        int woven = a->cfg.weave && a->cfg.render && (a->pf.bplcon0 & BPLCON0_LACE)
                    && a->fb[1] == a->fb[0] + AGA_OUT_W * AGA_OUT_H;
        a->weave_fill = woven && !a->weave_now;
        a->weave_now = woven;
        a->weave_row = a->lof ? 0 : 1;
    }
    if (a->hooks.frame_done) a->hooks.frame_done(a->hooks.user);
}

/* The longest the vertical blank interrupt waits for the copper (see below);
   0 raises it at the start of line 0 again, the way it was before */
#ifndef VERTB_HOLD_MAX_LINES
#define VERTB_HOLD_MAX_LINES 8
#endif

static void start_of_line(aga_t *a)
{
    if (a->vpos == 0) {
        /* vertical blank start. VERTB is raised here on a real Agnus too, but
           a real 68k then needs far longer than the copper does to get
           anywhere: the copper runs the setup block at the top of its list
           (pointers, modulos, palette) in the first colour clocks of the
           line, long before any interrupt handler can touch that list. Emu68
           answers in microseconds, on another core, while this copper runs at
           the Amiga's pace - so a handler that rewrites the list could change
           it UNDER the copper. Roguecraft DX swaps its screens from its
           vertical blank handler, writing each bitplane pointer as two words:
           the copper read new high words with old low words, fetched five
           planes from the wrong places, and whole frames flickered with
           displaced HUD pieces and scrambled colour (2026-09-28). So VERTB
           waits until the copper is idle - blocked on a WAIT for a later line,
           stopped, off, or waiting for the blitter - and never past
           VERTB_HOLD_MAX_LINES. See cck_step. */
        a->vertb_hold = 1;
        cop_vsync(a);
        spr_vsync(a);
        pf_vsync(a);
    }
    pf_start_line(a);
    cop_start_line(a);
    spr_start_line(a);
    a->sprline_valid = 0;
    a->sprclx_valid = 0;
}

static inline void cck_step(aga_t *a)
{
    int h = a->hpos;
    /* copper: one access every two colour clocks (even cycles). Only when
       it can act: waiting for a later line, stopped, with copper DMA off or
       waiting on a busy blitter, cop_sync would return at once - after noting
       the cycles it was offered, which is done here instead. Those were
       36,000 calls a frame in Reshoot Proxima 3, a fifth of the chipset
       core's work, doing nothing. */
    if (!(h & 1)) {
        copper_t *c = &a->cop;
        int idle = (a->dmacon & (DMAF_DMAEN | DMAF_COPEN)) != (DMAF_DMAEN | DMAF_COPEN)
                || c->state == COP_stop
                || (c->state == COP_wait && (a->vpos & c->vmask) < c->vcmp)
                || (c->state == COP_bltwait && a->blt.busy);
        /* ... and a WAIT on this line before the colour clock it wakes at
           (cop_wait_wake; RGB2RTG 0.75: Jim Power waits for a place on
           nearly every line). Not idle for the VERTB hold below: the copper
           is still in the middle of its line's work there. */
        int skip = idle || (c->state == COP_wait && c->wake_ok && c->wake_seq == a->line_no &&
                            h >= c->wake_from && h < c->wake_h && !c->delay);
        if (skip) { if (c->last_hpos < h + 1) c->last_hpos = h + 1; }
        else cop_sync(a, h + 1);
        if (a->vertb_hold && (idle || a->vpos >= VERTB_HOLD_MAX_LINES)) {
            a->vertb_hold = 0;
            aga_intreq_set(a, INTF_VERTB);
        }
    }
    /* sprite DMA slots 0x15..0x34 */
    if (h >= 3 && h <= 9 && (h & 1)) aga_trace_at(a, h, AGA_DMA_REFRESH, REFPTR, 0);
    if (h >= 0x19 && h < 0x19 + 4 * AGA_MAX_SPRITES) spr_dma_slot(a, h);
    /* audio DMA slots 0x0D,0x0F,0x11,0x13 */
    if (h >= 0x0D && h <= 0x13 && (h & 1)) aud_dma_slot(a, h);
}

void aga_run_cycles(aga_t *a, int cck)
{
    while (cck-- > 0) {
        cck_step(a);
        a->hpos++;
        if (a->hpos >= a->maxhpos) {
            a->frame_cck += (uint32_t)a->maxhpos;
            end_of_line(a);
            a->hpos = 0;
            a->vpos++;
            a->line_no++;
            int lines = a->maxvpos + (a->lof ? 1 : 0);
            if (a->vpos >= lines) {
                a->last_frame_cck = a->frame_cck;
                a->frame_cck = 0;
                a->vpos = 0;
                if (a->pf.bplcon0 & BPLCON0_LACE) a->lof ^= 1;
                else a->lof = 1;
                end_of_frame(a);
            }
            if (line_toggles(a))
                a->lol ^= 1;
            set_line_length(a);
            start_of_line(a);
        }
    }
}

void aga_run_line(aga_t *a)
{
    /* at least one: a VPOSW that ends a long line at its last colour clock
       leaves hpos AT the new end, and the step after it closes the line */
    int left = a->maxhpos - a->hpos;
    aga_run_cycles(a, left > 0 ? left : 1);
}

void aga_run_frame(aga_t *a)
{
    int target = a->frame + 1;
    while (a->frame < target) aga_run_line(a);
}

/* --------------------------------------------------------------------------
 * status
 * ------------------------------------------------------------------------ */
int aga_last_frame_display_lines(const aga_t *a) { return a->last_display_lines; }
int aga_last_frame_had_data(const aga_t *a)      { return a->last_frame_data; }
int aga_last_frame_window(const aga_t *a, int *x0, int *x1, int *y0, int *y1)
{
    *x0 = a->last_win_x0; *x1 = a->last_win_x1;
    *y0 = a->last_win_y0; *y1 = a->last_win_y1;
    return a->last_win_x0 <= a->last_win_x1;
}
uint32_t aga_blit_count(const aga_t *a)          { return a->blit_count; }
int aga_native_active(const aga_t *a)
{
    if (a->aga_ctrl & 0x0002) return 1;               /* windowed: always present the frame */
    if (a->aga_ctrl & 0x8000) return !(a->aga_ctrl & 1);
    return a->last_display_lines > 0;
}
int aga_window_mode(const aga_t *a, int *x, int *y, int *w, int *h)
{
    if (x) *x = a->aga_win[0];
    if (y) *y = a->aga_win[1];
    if (w) *w = a->aga_win[2];
    if (h) *h = a->aga_win[3];
    return (a->aga_ctrl & 0x0002) ? 1 : 0;
}
int aga_get_ipl(const aga_t *a)          { return a->ipl; }
int aga_get_vpos(const aga_t *a)         { return a->vpos; }
int aga_get_hpos(const aga_t *a)         { return a->hpos; }
int aga_frame_count(const aga_t *a)      { return a->frame; }
uint32_t aga_last_frame_cck(const aga_t *a) { return a->last_frame_cck; }
int aga_lines_per_frame(const aga_t *a)  { return a->maxvpos + (a->lof ? 1 : 0); }
int aga_cycles_per_line(const aga_t *a)  { return a->maxhpos; }   /* this line's: 227, or 228 with LOL */
const uint32_t *aga_framebuffer(const aga_t *a)
{
    return a->weave_done ? a->fb[0] : a->fb[a->fb_done];
}
uint32_t *aga_framebuffer_live(aga_t *a)
{
    return a->weave_now ? a->fb[0] : a->fb[a->fb_cur];
}
/* Lock-free beam read.
 *
 * VPOSR and VHPOSR are by a huge margin the hottest registers a real Amiga
 * program reads: every short delay is a beam wait. Measured on hardware with
 * an AGA WHDLoad game loading, 2,173,700 reads per second - 43,000 per frame -
 * from a dozen different two-instruction loops in ROM and in fast RAM at once.
 * Taking the chipset mutex for each of those made the machine spend all its
 * time in the trap path, and fired two million cross-core SEVs a second at the
 * core actually running the chipset.
 *
 * Nothing here needs the lock. vpos, hpos and lof are plain integers the
 * chipset thread advances; the worst a racing reader gets is a value a few
 * colour clocks old, which is indistinguishable from having read the register
 * a moment earlier - and on real hardware the beam moves under you anyway.
 */
int aga_is_beam_reg(uint32_t reg) { reg &= 0x1FE; return reg == VPOSR || reg == VHPOSR; }

/* add_cck: colour clocks elapsed since the chipset thread last stepped.
 *
 * The loop advances the beam CHUNK_CCK at a time, so without this hpos only
 * ever takes values 8 apart. A beam wait tests for an EXACT horizontal
 * position - `move.b $dff007,d0 / cmp.b d1,d0 / bne` - and 7 targets in 8 are
 * never produced. The phase drifts by maxhpos mod CHUNK_CCK each line, so the
 * loop does eventually catch its value, but only once every few lines: every
 * short beam wait in the machine silently becomes several lines long.
 *
 * Stepping the chipset one clock at a time would fix it and cost core 3 eight
 * times the work. Interpolating at the point of the read costs a divide, and
 * the pacing is against the same clock, so the answer is right.
 */
uint32_t aga_peek_beam_at(const aga_t *a, uint32_t reg, int size, uint32_t add_cck)
{
    uint32_t vpos = (uint32_t)a->vpos, hpos = (uint32_t)a->hpos;
    int lol = a->lol;
    if (add_cck) {
        /* line by line: NTSC's alternate 228 and 227 colour clocks, so one
           divide by the line length no longer says where the beam is */
        uint32_t base = a->ntsc ? AGA_NTSC_HPOS : AGA_PAL_HPOS;
        uint32_t maxv = (uint32_t)a->maxvpos + (a->lof ? 1 : 0);
        if (!maxv) maxv = 313;
        uint32_t cur = a->maxhpos > 0 ? (uint32_t)a->maxhpos : base;
        hpos += add_cck;
        if (hpos < cur) {
            /* still on this line */
        } else if (!line_toggles(a)) {
            /* this line first, at its own length: a BEAMCON0 LOLDIS written
               on a long NTSC line leaves it 228 clocks, the lines after it 227 */
            hpos -= cur;
            vpos++;
            lol = 0;
            vpos += hpos / base;
            hpos %= base;
        } else {
            /* whole pairs of lines first (one long, one short: LOL is
               where it was), then at most two single lines */
            uint32_t pairs = hpos / (2 * base + 1);
            hpos -= pairs * (2 * base + 1);
            vpos += 2 * pairs;
            while (hpos >= base + (uint32_t)lol) {
                hpos -= base + (uint32_t)lol;
                vpos++;
                lol ^= 1;
            }
        }
        if (vpos >= maxv) vpos %= maxv;
    }
    uint16_t vposr  = (uint16_t)((a->ntsc_strap ? VPOSR_ID_AGA_NTSC : VPOSR_ID_AGA_PAL)
                                 | (a->lof ? 0x8000 : 0) | (lol ? VPOSR_LOL : 0) | ((vpos >> 8) & 7));
    uint16_t vhposr = (uint16_t)(((vpos & 0xFF) << 8) | (hpos & 0xFF));
    uint16_t w = ((reg & 0x1FE) == VPOSR) ? vposr : vhposr;
    switch (size) {
    case 1:  return (reg & 1) ? (w & 0xFF) : (w >> 8);
    case 4:  return ((uint32_t)vposr << 16) | vhposr;
    default: return w;
    }
}

uint32_t aga_peek_beam(const aga_t *a, uint32_t reg, int size)
{
    uint32_t vpos = (uint32_t)a->vpos, hpos = (uint32_t)a->hpos;
    uint16_t vposr  = (uint16_t)((a->ntsc_strap ? VPOSR_ID_AGA_NTSC : VPOSR_ID_AGA_PAL)
                                 | (a->lof ? 0x8000 : 0) | (a->lol ? VPOSR_LOL : 0) | ((vpos >> 8) & 7));
    uint16_t vhposr = (uint16_t)(((vpos & 0xFF) << 8) | (hpos & 0xFF));
    uint16_t w = ((reg & 0x1FE) == VPOSR) ? vposr : vhposr;
    switch (size) {
    case 1:  return (reg & 1) ? (w & 0xFF) : (w >> 8);
    case 4:  return ((uint32_t)vposr << 16) | vhposr;   /* a longword at $DFF004 */
    default: return w;
    }
}

uint16_t aga_peek_reg(const aga_t *a, uint32_t reg) { return a->regs[(reg & 0x1FE) >> 1]; }
uint16_t aga_peek_dmacon(const aga_t *a) { return a->dmacon; }
uint32_t aga_peek_fetch_stat(const aga_t *a, int i) { return a->dbg_fetch[i & 7]; }
uint32_t aga_peek_audirq(const aga_t *a, int i) { return (unsigned)i < 5 ? a->dbg_audirq[i] : 0; }
/* Diagnostics: one ordered log of the events that decide BPLCON0's fate.
   A = type<<24 | frame; B = value<<16 | vpos<<7 | hpos>>1. Types: 1 CPU
   BPLCON0, 2 CPU COP1LC, 3 CPU COP2LC, 4 CPU COPJMP1, 5 CPU COPJMP2, 6 CPU
   DMACON, 7 copper BPLCON0, 8 copper restart (COP1LC low word), 9 copper
   COPJMP. The first CPU BPLCON0 with bitplanes (trigger) freezes the 16
   events before it and the log then takes 32 more. */
void aga_dbg_event(aga_t *a, int type, uint16_t value, int trigger)
{
    uint32_t ea = ((uint32_t)type << 24) | ((uint32_t)a->frame & 0xFFFFFF);
    uint32_t eb = ((uint32_t)value << 16) | (((uint32_t)a->vpos & 0x1FF) << 7) | (((uint32_t)a->hpos >> 1) & 0x7F);
    if (!a->ev_trig) {
        uint32_t k = a->ev_pre_n++ & 15;
        a->ev_pre[k][0] = ea; a->ev_pre[k][1] = eb;
        if (trigger) {
            uint32_t n = a->ev_pre_n < 16 ? a->ev_pre_n : 16;
            for (uint32_t i = 0; i < n; i++) {           /* oldest first, the trigger last */
                uint32_t j = (a->ev_pre_n - n + i) & 15;
                a->ev_log[i][0] = a->ev_pre[j][0]; a->ev_log[i][1] = a->ev_pre[j][1];
            }
            a->ev_log_n = n;
            a->ev_trig = 1;
        }
    } else if (a->ev_log_n < 48) {
        a->ev_log[a->ev_log_n][0] = ea; a->ev_log[a->ev_log_n][1] = eb;
        a->ev_log_n++;
    }
}
void aga_dbg_event_cpu(aga_t *a, uint32_t reg, uint16_t value)
{
    switch (reg & 0x1FE) {
    case 0x100: aga_dbg_event(a, 1, value, (value & 0x7010) != 0); break;
    case 0x080: case 0x082: aga_dbg_event(a, 2, value, 0); break;
    case 0x084: case 0x086: aga_dbg_event(a, 3, value, 0); break;
    case 0x088: aga_dbg_event(a, 4, value, 0); break;
    case 0x08A: aga_dbg_event(a, 5, value, 0); break;
    case 0x096: aga_dbg_event(a, 6, value, 0); break;
    default: break;
    }
}
uint32_t aga_peek_evlog(const aga_t *a, int i)
{
    if (i == 0) return a->ev_log_n;
    i--;
    return (i >= 0 && i < 96) ? a->ev_log[i >> 1][i & 1] : 0;
}

/* i 0-7: restart addresses, 8-15: their frames, 16: how many, 17-20: copper BPLCON0 n/pc/val/frame */
uint32_t aga_peek_copdbg(const aga_t *a, int i)
{
    if (i < 8)  return a->dbg_coprs_addr[(a->dbg_coprs_n - 1 - (uint32_t)i) & 7];   /* 0 = newest */
    if (i < 16) return a->dbg_coprs_frame[(a->dbg_coprs_n - 1 - (uint32_t)(i - 8)) & 7];
    switch (i) {
    case 16: return a->dbg_coprs_n;
    case 17: return a->dbg_cop_bc0_n;
    case 18: return a->dbg_cop_bc0_pc;
    case 19: return a->dbg_cop_bc0_val;
    case 20: return a->dbg_cop_bc0_frame;
    default: return 0;
    }
}
uint32_t aga_peek_beamcon0_log(const aga_t *a, int i)
{
    uint32_t n = a->bc0_log_head < 8 ? 0 : a->bc0_log_head;
    return a->bc0_log[(n + (unsigned)i) & 7];
}
uint32_t aga_peek_dmacon_log(const aga_t *a, int i)
{
    uint32_t n = a->dma_log_head < 8 ? 0 : a->dma_log_head;
    return a->dma_log[(n + (unsigned)i) & 7];
}
uint32_t aga_peek_color(const aga_t *a, int index) { return a->color[index & 255]; }
uint32_t aga_peek_bplpt(const aga_t *a, int plane) { return a->pf.bplpt[plane & 7]; }
uint32_t aga_peek_copper_pc(const aga_t *a) { return a->cop.pc; }

void aga_set_render(aga_t *a, int on)
{
    a->cfg.render = on ? 1 : 0;
}

/* The flicker fixer, brightness and the starting standard: all kept in cfg,
   which outlives aga_reset (see chipset.h) */
void aga_set_weave(aga_t *a, int on) { a->cfg.weave = on ? 1 : 0; }
int aga_frame_woven(const aga_t *a, int *row)
{
    if (row) *row = a->weave_done_row;
    return a->weave_done;
}
void aga_set_brightness(aga_t *a, int percent)
{
    a->cfg.bright = percent < 100 ? 100 : percent > 200 ? 200 : percent;
    bright_build(a);
}
void aga_set_gamma(aga_t *a, int gamma)
{
    a->cfg.gamma = gamma >= 0 && gamma < TONE_GAMMAS ? gamma : 0;
    bright_build(a);
}
void aga_set_ntsc(aga_t *a, int on) { a->cfg.ntsc = on ? 1 : 0; }
int aga_is_ntsc(const aga_t *a) { return a->ntsc; }

/* Render into caller-owned pages instead of the core's own frames. */
void aga_set_framebuffers(aga_t *a, uint32_t *f0, uint32_t *f1)
{
    a->fb[0] = f0; a->fb[1] = f1; a->fb[2] = NULL;
    a->fb_count = 2; a->fb_cur = 0; a->fb_done = 0; a->fb_external = 1;
}
void aga_set_framebuffers3(aga_t *a, uint32_t *f0, uint32_t *f1, uint32_t *f2)
{
    a->fb[0] = f0; a->fb[1] = f1; a->fb[2] = f2;
    a->fb_count = 3; a->fb_cur = 0; a->fb_done = 0; a->fb_external = 1;
}

/* Ask for frames to be skipped (drawn nothing, shown nothing new) while
   the host is behind real time. Takes effect at the next frame end. */
void aga_set_frame_skip(aga_t *a, int on) { a->skip_req = on ? 1 : 0; }
uint32_t aga_frames_skipped(const aga_t *a) { return a->frames_skipped; }
