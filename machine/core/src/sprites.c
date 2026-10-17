/* AGA-PISTORM - sprite DMA and per-line sprite rendering.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * From UAE/WinUAE: spr_startstop is WinUAE's sprstartstop, called around a
 * SPRxPOS/SPRxCTL write as there, and the DMA cycle steps in spr_dma_slot
 * are generate_sprites' (custom.cpp; both copied); the SPRES switch follows
 * update_sprres (drawing.cpp) and the VSTART/VSTOP decode SPRxCTLPOS
 * (custom.cpp); the vblank guard and the slot decode follow do_sprites in
 * E-UAE's custom.c. The sprite vblank lines (25/20) and the +9 position
 * offset (DIW_DDF_OFFSET) are UAE's constants, and the sprite DMA slot
 * position was measured from WinUAE's DMA records. SSCAN2 follows WinUAE:
 * the eight-bit position match is denise_xposmask (drawing.cpp, expand_fmode
 * and matchsprites2_aga), the skipped fetch generate_sprites' dblscan test
 * (custom.cpp). The rendering, the replay of mid-line rewrites and the
 * collision groups are this project's own.
 *   WinUAE custom.cpp:  Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   WinUAE drawing.cpp: Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   E-UAE custom.c:     Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2004 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include "internal.h"

/* Sprite DMA slots: WinUAE records put SPRxPOS at 0x19+4x and SPRxCTL at
   0x1B+4x (sprite 0 at cycles 25 and 27), so sprite 7 ends at cycle 55. */
#define SPR0_HPOS 0x19

/* sprline position (shres) at which a register write made now takes effect:
   the same pipeline delay as a colour change (playfield.c: hpos*4+2 hires) */
#define SPR_NOW(a) ((a)->hpos * 8 + 4)

void spr_reset(aga_t *a)
{
    memset(a->spr, 0, sizeof a->spr);
    a->sprite_width = 16;
    a->sprres = RES_LORES;
}

void spr_expand_res(aga_t *a)
{
    int old_res = a->sprres;
    switch ((pf_con3(&a->pf) >> 6) & 3) {
    case 0:  a->sprres = (a->pf.res == RES_SHRES) ? RES_HIRES : RES_LORES; break;
    case 1:  a->sprres = RES_LORES; break;
    case 2:  a->sprres = RES_HIRES; break;
    default: a->sprres = RES_SHRES; break;
    }
    if (a->sprres != old_res && a->nsprres_ev < 16) {
        a->sprres_ev_sx[a->nsprres_ev] = SPR_NOW(a);
        a->sprres_ev_res[a->nsprres_ev] = (int8_t)a->sprres;
        a->nsprres_ev++;
    }
    switch ((a->pf.fmode >> 2) & 3) {
    case 0:  a->sprite_width = 16; break;
    case 3:  a->sprite_width = 64; break;
    default: a->sprite_width = 32; break;
    }
}

static void spr_update_posctl(aga_t *a, int n)
{
    sprite_t *s = &a->spr[n];
    /* xpos in shres pixels (4 per lores pixel), DIW coordinates */
    s->xpos = (((s->pos & 0xFF) << 1) | (s->ctl & 1)) << 2;
    s->xpos |= (s->ctl >> 3) & 3;
    /* The output is hires: WinUAE maps a sprite's superhires start down to the
       hires pixel it falls in. Kept at an odd superhires position, a sprite
       drew one hires pixel too far right - Reshoot Proxima 3's text shadows
       (SH0/SH1 = 3) came out twice as wide. */
    s->xpos &= ~1;
    s->vstart = (s->pos >> 8) | ((s->ctl & 0x04) ? 0x100 : 0) | ((s->ctl & 0x40) ? 0x200 : 0);
    s->vstop  = (s->ctl >> 8) | ((s->ctl & 0x02) ? 0x100 : 0) | ((s->ctl & 0x20) ? 0x200 : 0);
    s->attach = (s->ctl & 0x80) ? 1 : 0;
}

/* Where a sprite at xpos (shres, DIW coordinates) shows on a line. With
   SSCAN2 (FMODE bit 15) Denise matches only the low eight bits of the lores
   position - SPRxPOS bit 7, the 256-pixel bit, is the sprite's scan-doubling
   flag then - so every sprite shows twice a line, 256 lores pixels apart.
   Castlevania AGA (Scorpion Engine) tiles its far layer that way: four
   64-pixel sprites side by side, repeated. One copy each drew the clouds and
   mountains over the left 128 pixels of the play area and nothing right of
   them. */
static inline int spr_copies(const aga_t *a, int xpos, int x[2])
{
    if (!(a->pf.fmode & 0x8000)) { x[0] = xpos; return 1; }
    x[0] = xpos & ~0x400;
    x[1] = x[0] | 0x400;
    return 2;
}

/* WinUAE sprstartstop(): the start and stop lines are compared with the
   current line whenever SPRxPOS or SPRxCTL is written - by the copper, the
   CPU or the sprite's own DMA - not only in the sprite's DMA slot. So a copper
   list can switch a sprite's DMA off (stop line = this line) or on (start
   line = this line) in the middle of the screen; checked only in the slot, a
   stop written after it never matches and the sprite goes on reading memory
   down to the bottom of the screen. */
static void spr_startstop(aga_t *a, sprite_t *s)
{
    if (a->vpos < a->sprite_vblank) return;
    if (a->vpos == s->vstart) s->dmastate = 1;
    if (a->vpos == s->vstop || a->vpos == a->sprite_vblank) { s->dmastate = 0; s->dmacycle = 0; }
}

/* ---- sprites rewritten while the beam is on the line -----------------------
   A sprite shows where the beam meets its horizontal position while it is
   armed: the data it holds then is loaded into its shifter and shifted out,
   and a later match - the copper has moved it on - loads it again. So one
   sprite can appear several times on a line (Reshoot Proxima 3 moves sprite 6
   three times on every line of two stages). Drawing each sprite once, from
   its state at the end of the line, shows only the last copy.
   The first time the copper or the CPU writes a sprite during a line, its
   state until then is logged, and from then on every change to it, so
   spr_build_line can replay that line. A line nothing writes a sprite on is
   drawn exactly as before. */
static void spr_log(aga_t *a, int n, int sx)
{
    if (a->nsprev >= AGA_SPR_EVENTS) { a->sprev_full = 1; return; }
    const sprite_t *s = &a->spr[n];
    sprev_t *e = &a->sprev[a->nsprev++];
    e->sx = sx;
    e->n = (uint8_t)n;
    e->armed = (uint8_t)(s->armed != 0);
    e->xpos = s->xpos;
    e->data = s->data;
    e->datb = s->datb;
}

static void spr_touch(aga_t *a, int n)
{
    if (a->spr_touched & (1u << n)) return;
    a->spr_touched |= 1u << n;
    spr_log(a, n, -0x40000000);          /* the state it had since the line began */
}

void spr_start_line(aga_t *a)
{
    a->sprres_line0 = a->sprres;
    a->nsprres_ev = 0;
    a->nsprev = 0;
    a->sprev_full = 0;
    a->spr_touched = 0;
}

static inline uint64_t widen16(uint16_t v)
{
    /* a CPU write of 16 bits fills all fetch-width words (UAE behaviour) */
    uint64_t x = v;
    return x | (x << 16) | (x << 32) | (x << 48);
}

void spr_write(aga_t *a, uint32_t reg, uint16_t v)
{
    if (reg >= SPR0PTH && reg < SPR0PTH + 8 * 4) {
        int n = (reg - SPR0PTH) >> 2;
        sprite_t *s = &a->spr[n];
        if ((reg & 2) == 0) s->pt = (s->pt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16);
        else                s->pt = (s->pt & 0xFFFF0000u) | (v & 0xFFFE);
        return;
    }
    if (reg >= SPR0POS && reg < SPR0POS + 8 * 8) {
        int n = (reg - SPR0POS) >> 3;
        sprite_t *s = &a->spr[n];
        spr_touch(a, n);
        switch (reg & 6) {
        case 0: s->pos = v; spr_startstop(a, s); spr_update_posctl(a, n); spr_startstop(a, s); break;
        case 2: s->ctl = v; s->armed = 0; spr_startstop(a, s); spr_update_posctl(a, n); spr_startstop(a, s); break;
        case 4: s->data = widen16(v); s->armed = 1; break;
        case 6: s->datb = widen16(v); break;
        }
        spr_log(a, n, SPR_NOW(a));
    }
}

void spr_vsync(aga_t *a)
{
    for (int i = 0; i < AGA_MAX_SPRITES; i++) a->spr[i].dmastate = a->spr[i].dmacycle = 0;
}

static uint64_t spr_fetch(aga_t *a, uint32_t addr)
{
    switch (a->sprite_width) {
    case 16: return (uint64_t)chip_rd16(a, addr) << 48;
    case 32: return (uint64_t)chip_rd32(a, addr) << 32;
    default: return chip_rd64(a, addr);
    }
}

/* Called at colour clocks 0x19.. for each sprite slot; two accesses per sprite:
   (hpos - 0x19) & 3 == 0 -> first word (POS or DATA), == 2 -> second (CTL or DATB).
   The state machine is WinUAE's (generate_sprites): dmacycle says whether this
   line fetches at all, dmastate whether it fetches data or control words. The
   slot decode and the vblank guard follow E-UAE's do_sprites. */
void spr_dma_slot(aga_t *a, int hpos)
{
    int off = hpos - SPR0_HPOS;
    int n = off >> 2;
    int cycle = off & 3;
    if (n < 0 || n >= AGA_MAX_SPRITES || (cycle & 1)) return;
    cycle >>= 1;
    sprite_t *s = &a->spr[n];
    int vblank_end = a->sprite_vblank;   /* sprites restart on their own line, not the display's */

    if (a->vpos < vblank_end) return;
    /* no sprite DMA on the last line of the frame (WinUAE: Super Stardust fetches
       nothing on line 312 where I fetched eight data words) */
    if (a->vpos >= a->maxvpos + (a->lof ? 1 : 0) - 1) return;

    if (cycle == 0) {
        if (!s->dmacycle && s->dmastate) s->dmacycle = 1;
        if (a->vpos == s->vstart) { s->dmastate = 1; s->dmacycle = 1; }
        if (a->vpos == s->vstop || a->vpos == vblank_end) { s->dmastate = 0; s->dmacycle = 1; }
        s->dmadata = s->dmastate;        /* both words of this line are the same kind */
    }
    /* SSCAN2 scan-doubling: a sprite with SPRxPOS bit 7 set fetches data only
       on lines of its start line's parity, so each data line shows twice */
    int dbl = s->dmadata && (a->pf.fmode & 0x8000) && (s->pos & 0x80) && ((a->vpos ^ s->vstart) & 1);
    /* No sprite slot while the line's bitplane fetch runs (WinUAE do_sprites:
       only if bprun != 1): nothing is fetched and the pointer stays, though
       the state machine below still steps. Brian the Lion's copper sets
       DDFSTRT $30, so sprite 7's slots belong to the bitplanes - I fetched
       it from its stale pointer and armed it with junk (dark-blue dashes;
       from RGB2RTG, EMM #2072). The OCS Agnus's off-by-one and the
       DDFSTOP bplconflict edge are not modelled. */
    if (s->dmacycle && aga_dma_enabled(a, DMAF_SPREN) && !dbl && !pf_bpl_running(a, hpos)) {
        uint32_t bytes = a->sprite_width >> 3;
        if (!s->dmadata) {
            aga_trace_at(a, hpos, AGA_DMA_SPRITE,
                         (uint16_t)(SPR0POS + 8 * n + 2 * cycle), s->pt);
            uint16_t w = chip_rd16(a, s->pt);
            s->pt += bytes;
            if (cycle == 0) { s->pos = w; spr_update_posctl(a, n); }
            else            { s->ctl = w; s->armed = 0; spr_update_posctl(a, n); }
            /* Fetched words reach the registers later in WinUAE's pipeline, after
               this line's second slot is decided: compare the new start/stop
               lines only once both are in. Doing it after POS, while the old
               stop line still matches, would cancel the CTL fetch. */
            if (cycle == 1) spr_startstop(a, s);
        } else {
            aga_trace_at(a, hpos, AGA_DMA_SPRITE,
                         (uint16_t)(SPR0DATA + 8 * n + 2 * cycle), s->pt);
            uint64_t d = spr_fetch(a, s->pt);
            s->pt += bytes;
            if (cycle == 0) { s->data = d; s->armed = 1; }
            else            { s->datb = d; }
        }
        if (a->spr_touched & (1u << n)) spr_log(a, n, hpos * 8 + 4);
    }
    if (s->dmacycle) {
        s->dmacycle++;
        if (s->dmacycle > 2) s->dmacycle = s->dmastate ? 1 : 0;
    }
}

/* note a span of sprline as written, so the next build clears it */
static inline void spr_mark(aga_t *a, int x0, int x1, int linelen)
{
    if (x0 < 0) x0 = 0;
    if (x1 > linelen) x1 = linelen;
    if (x0 < a->sprline_lo) a->sprline_lo = x0;
    if (x1 > a->sprline_hi) a->sprline_hi = x1;
}

/* each event's next one of the same sprite: its time */
static void spr_next_events(const aga_t *a, int *t_after)
{
    int last[AGA_MAX_SPRITES];
    for (int n = 0; n < AGA_MAX_SPRITES; n++) last[n] = 0x40000000;
    for (int i = a->nsprev - 1; i >= 0; i--) {
        t_after[i] = last[a->sprev[i].n];
        last[a->sprev[i].n] = a->sprev[i].sx;
    }
}

/* Where sprite n shows on the replayed line (sprline position, the +9 lores
   offset in), and with which data: its copies in the order they were made,
   at most AGA_SPR_EVENTS. A copy ends where the same sprite's next copy
   starts. Sprites nothing wrote keep their single end-of-line copy (two with
   SSCAN2). */
static int spr_line_copies(const aga_t *a, int n, const int *t_after, int *cx, uint64_t *cda, uint64_t *cdb)
{
    const sprite_t *s = &a->spr[n];
    int w = a->sprite_width;
    const int hwoff = 9 * 4;
    int xs[2], ncp, nc = 0;
    if (!(a->spr_touched & (1u << n))) {
        if (s->armed) {
            ncp = spr_copies(a, s->xpos, xs);
            for (int c = 0; c < ncp; c++, nc++) { cx[nc] = xs[c] + hwoff; cda[nc] = s->data; cdb[nc] = s->datb; }
        }
        return nc;
    }
    for (int i = 0; i < a->nsprev; i++) {
        const sprev_t *e = &a->sprev[i];
        if (e->n != n) continue;
        int t_next = t_after[i];
        /* armed while the beam meets it, with data to load (WinUAE only
           starts the shifter for non-zero data) */
        if (!e->armed || !((e->data | e->datb) >> (64 - w))) continue;
        ncp = spr_copies(a, e->xpos, xs);
        for (int c = 0; c < ncp && nc < AGA_SPR_EVENTS; c++) {
            /* E-UAE decide_sprites: before a write at colour clock h every
               sprite whose comparator position (no display offset) is below
               h*2-6 lores pixels is matched with the old state, and one
               exactly at the bound already sees the write: so >= sx-28 and
               < t_next-28 in sprline units. WinUAE's Denise counter stands
               at 2h-8 (drawing.cpp: 3 refresh offset + 2 pipeline delay) and
               a register write takes effect one colour clock after its slot,
               which puts the bound at 2h-6. Brian the Lion's clouds - sprite
               7 placed by the copper 2-4 times a line, SPR7DATA at the very
               clock its copy starts - showed each chunk with the previous
               chunk's data; ReshootR moves sprite 6 (its rock layer) 4-5
               times a line with every SPR6POS exactly at the bound, and with
               h*2-3 every copy was dropped and the rock came out as flat
               colour bands (from RGB2RTG, EMM #2072 then #2099). */
            if (xs[c] >= e->sx - 28 && xs[c] < t_next - 28) { cx[nc] = xs[c] + hwoff; cda[nc] = e->data; cdb[nc] = e->datb; nc++; }
        }
    }
    return nc;
}

/* One copy of a sprite into its buffer v, transparent pixels too, up to
   limit: returns where it ended. The resolution can change along the line
   (sprres_ev): a sprite pixel is as wide as the resolution the beam is in. */
static int spr_copy_to_buffer(const aga_t *a, uint8_t *v, int x, int limit, uint64_t da, uint64_t db)
{
    int w = a->sprite_width;
    if (!a->nsprres_ev) {
        /* one resolution all line: sprite pixel i covers 1 << sh output
           pixels, so its value is worked out once and written as a run (from
           RGB2RTG a861e51: pixel by pixel this loop was about an eighth of
           Jim Power's chipset time on a PC; the pictures are the same) */
        int sh = 2 - a->sprres_line0;
        int end = x + (w << sh);
        if (end > limit) end = limit;
        if (end < x) end = x;
        int px = x < 0 ? 0 : x;
        while (px < end) {
            int i = (px - x) >> sh, b = 63 - i;
            uint8_t val = (uint8_t)(((da >> b) & 1) | (((db >> b) & 1) << 1));
            int nxt = x + ((i + 1) << sh);
            if (nxt > end) nxt = end;
            do v[px] = val; while (++px < nxt);
        }
        return end;
    }
    /* the resolution in force where this copy starts */
    int res = a->sprres_line0, ev = 0;
    while (ev < a->nsprres_ev && a->sprres_ev_sx[ev] <= x) res = a->sprres_ev_res[ev++];
    int pixw = 4 >> res, step = 0, bit = 0;
    for (; x < limit && bit < w; x++) {
        /* a change of resolution reached by the beam: shift at the new width from here */
        while (ev < a->nsprres_ev && a->sprres_ev_sx[ev] <= x) {
            pixw = 4 >> a->sprres_ev_res[ev++];
            if (step >= pixw) { step = 0; bit++; }
        }
        if (bit >= w) break;
        if (x >= 0) {
            int sh = 63 - bit;
            v[x] = (uint8_t)(((da >> sh) & 1) | (((db >> sh) & 1) << 1));
        }
        if (++step >= pixw) { step = 0; bit++; }
    }
    return x;
}

/* The replay through the sprites' own buffers, for what needs a sprite's own
   pixels: collisions and a resolution change along the line. It looks only
   where the copies are: each copy notes the span of its buffer it wrote, and
   priority is applied, and the buffers cleared, over those spans alone - it
   used to be the whole stretch from the leftmost sprite pixel to the
   rightmost, eight buffers deep, every line. From RGB2RTG 0.75-dev13 (by the
   same author). */
#define SPR_SPANS (2 * AGA_SPR_EVENTS + 2 * AGA_MAX_SPRITES)
static void spr_replay_buffers(aga_t *a, int clx)
{
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    int lo = linelen, hi = 0;
    int16_t sp0[SPR_SPANS], sp1[SPR_SPANS];
    uint8_t spn[SPR_SPANS];
    int nsp = 0;
    int t_after[AGA_SPR_EVENTS];
    spr_next_events(a, t_after);

    for (int n = 0; n < AGA_MAX_SPRITES; n++) {
        int cx[AGA_SPR_EVENTS];
        uint64_t cda[AGA_SPR_EVENTS], cdb[AGA_SPR_EVENTS];
        int nc = spr_line_copies(a, n, t_after, cx, cda, cdb);
        for (int k = 0; k < nc; k++) {
            int limit = (k + 1 < nc) ? cx[k + 1] : linelen;   /* the next copy takes over */
            if (limit > linelen) limit = linelen;
            int xs = cx[k];
            int x = spr_copy_to_buffer(a, a->spv[n], xs, limit, cda[k], cdb[k]);
            if (xs < 0) xs = 0;
            if (xs < lo) lo = xs;
            if (x > hi) hi = x;
            if (x > xs && nsp < SPR_SPANS) {
                sp0[nsp] = (int16_t)xs; sp1[nsp] = (int16_t)x; spn[nsp] = (uint8_t)n; nsp++;
            }
        }
    }

    a->sprclx_valid = clx;
    if (clx) memset(a->sprclx, 0, sizeof a->sprclx);
    /* the spans in order of their start, overlapping ones merged: every
       pixel any copy wrote, once */
    for (int i = 1; i < nsp; i++) {
        int16_t a0 = sp0[i], a1 = sp1[i];
        uint8_t an = spn[i];
        int j = i - 1;
        for (; j >= 0 && sp0[j] > a0; j--) { sp0[j + 1] = sp0[j]; sp1[j + 1] = sp1[j]; spn[j + 1] = spn[j]; }
        sp0[j + 1] = a0; sp1[j + 1] = a1; spn[j + 1] = an;
    }
    int attach[4];
    for (int pair = 0; pair < 4; pair++) attach[pair] = a->spr[pair * 2 + 1].attach;
    for (int i = 0; i < nsp; ) {
        int x0 = sp0[i], x1 = sp1[i];
        for (i++; i < nsp && sp0[i] <= x1; i++)
            if (sp1[i] > x1) x1 = sp1[i];
        for (int x = x0; x < x1; x++) {
            uint8_t v = 0;
            for (int pair = 0; pair < 4; pair++) {
                int e = a->spv[pair * 2][x], o = a->spv[pair * 2 + 1][x];
                if (!(e | o)) continue;
                if (attach[pair]) v = (uint8_t)(e | (o << 2) | 0x30 | (pair << 6));
                else if (e)       v = (uint8_t)(e | (pair << 6));
                else              v = (uint8_t)(o | 0x20 | (pair << 6));
                break;
            }
            line[x] = v;
            if (clx) {
                uint8_t m = 0;
                for (int n = 0; n < AGA_MAX_SPRITES; n++) if (a->spv[n][x]) m |= (uint8_t)(1u << n);
                a->sprclx[x] = m;
            }
        }
    }
    if (lo < a->sprline_lo) a->sprline_lo = lo;
    if (hi > a->sprline_hi) a->sprline_hi = hi;
    /* leave the buffers clear for the next replayed line: each where its copies wrote */
    for (int i = 0; i < nsp; i++)
        memset(&a->spv[spn[i]][sp0[i]], 0, (size_t)(sp1[i] - sp0[i]));
}

/* one copy straight into the sprite line, its transparent pixels leaving
   what is there: pixel px of [x, end) shows bit 63 - ((px - x) >> sh) */
static inline void spr_draw_copy(uint8_t *line, int x, int end, int sh, uint64_t da, uint64_t db, unsigned flags)
{
    for (int px = x < 0 ? 0 : x; px < end; ) {
        int i = (px - x) >> sh;
        int nxt = x + ((i + 1) << sh);
        if (nxt > end) nxt = end;
        unsigned val = (unsigned)((da >> (63 - i)) & 1) | (unsigned)(((db >> (63 - i)) & 1) << 1);
        if (val) {
            uint8_t v = (uint8_t)(val | flags);
            do line[px] = v; while (++px < nxt);
        } else
            px = nxt;
    }
}

/* Replay a line on which the copper or the CPU rewrote sprites. Every copy
   of every sprite is drawn straight into the sprite line, the lowest
   priority first - pair 3 to pair 0, in a pair the odd sprite before the
   even one - each solid pixel over what is there: where sprites meet, the
   lowest-numbered with a pixel shows, as Denise has it. The buffers remain
   for what needs a sprite's own pixels: an attached pair (its colour is both
   sprites' bits), copies of one sprite over each other (the later one's
   transparent pixels hide the earlier's), collisions and a resolution change
   along the line (spr_replay_buffers). The same bytes come out as from the
   buffers: host/tests/test_sprreplay.c draws random lines both ways. From
   RGB2RTG 0.75-dev14 (by the same author): Jim Power's copper repeats
   sprites along its lines, 49 -> 10 us a replayed line on a Pi 3. */
#ifdef AGA_SPR_REPLAY_CHECK
unsigned long aga_spr_replay_checked, aga_spr_replay_mismatch, aga_spr_replay_dirty;
static int spr_buffers_clear(const aga_t *a)
{
    for (int n = 0; n < AGA_MAX_SPRITES; n++)
        for (size_t x = 0; x < sizeof a->spv[n]; x++)
            if (a->spv[n][x]) return 0;
    return 1;
}
#endif

static void spr_replay_line(aga_t *a, int clx)
{
    if (clx || a->nsprres_ev) { spr_replay_buffers(a, clx); return; }
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    int w = a->sprite_width, sh = 2 - a->sprres_line0;
    int lo = linelen, hi = 0;
    int t_after[AGA_SPR_EVENTS];
    spr_next_events(a, t_after);
    int cx[AGA_SPR_EVENTS], ex[AGA_SPR_EVENTS];
    uint64_t cda[AGA_SPR_EVENTS], cdb[AGA_SPR_EVENTS];

    a->sprclx_valid = 0;
    for (int pair = 3; pair >= 0; pair--) {
        int att = a->spr[pair * 2 + 1].attach;
        for (int which = 1; which >= 0; which--) {
            int n = pair * 2 + which;
            int nc = spr_line_copies(a, n, t_after, cx, cda, cdb);
            int overlap = 0, maxend = -0x40000000;
            for (int k = 0; k < nc; k++) {
                int limit = (k + 1 < nc) ? cx[k + 1] : linelen;   /* the next copy takes over */
                if (limit > linelen) limit = linelen;
                int end = cx[k] + (w << sh);
                if (end > limit) end = limit;
                if (end < cx[k]) end = cx[k];
                ex[k] = end;
                int xs = cx[k] < 0 ? 0 : cx[k];
                if (xs < lo) lo = xs;
                if (end > hi) hi = end;
                if (end > xs) {
                    if (xs < maxend) overlap = 1;
                    if (end > maxend) maxend = end;
                }
            }
            if (!att && !overlap) {
                unsigned flags = (unsigned)(pair << 6) | (which ? 0x20u : 0);
                for (int k = 0; k < nc; k++) spr_draw_copy(line, cx[k], ex[k], sh, cda[k], cdb[k], flags);
                continue;
            }
            /* into the sprite's own buffer, the copies in their order */
            for (int k = 0; k < nc; k++) spr_copy_to_buffer(a, a->spv[n], cx[k], ex[k], cda[k], cdb[k]);
            if (att && which)
                continue;                   /* the attached pair: the even sprite next, then both at once */
            const uint8_t *ve = a->spv[pair * 2], *vo = a->spv[pair * 2 + 1];
            if (!att) {
                unsigned flags = (unsigned)(pair << 6) | (which ? 0x20u : 0);
                const uint8_t *v = a->spv[n];
                for (int k = 0; k < nc; k++)
                    for (int px = cx[k] < 0 ? 0 : cx[k]; px < ex[k]; px++)
                        if (v[px]) line[px] = (uint8_t)(v[px] | flags);
                for (int k = 0; k < nc; k++)
                    if (ex[k] > (cx[k] < 0 ? 0 : cx[k]))
                        memset(&a->spv[n][cx[k] < 0 ? 0 : cx[k]], 0, (size_t)(ex[k] - (cx[k] < 0 ? 0 : cx[k])));
                continue;
            }
            /* the attached pair: four bits of colour where either has a
               pixel, over both sprites' copies - the odd sprite's spans are
               found again (its copies were overwritten by the even's) */
            int ox[AGA_SPR_EVENTS], oe[AGA_SPR_EVENTS];
            int no = 0;
            {
                uint64_t da2[AGA_SPR_EVENTS], db2[AGA_SPR_EVENTS];
                int nc2 = spr_line_copies(a, pair * 2 + 1, t_after, ox, da2, db2);
                for (int k = 0; k < nc2; k++) {
                    int limit = (k + 1 < nc2) ? ox[k + 1] : linelen;
                    if (limit > linelen) limit = linelen;
                    int end = ox[k] + (w << sh);
                    if (end > limit) end = limit;
                    if (end < ox[k]) end = ox[k];
                    oe[k] = end;
                }
                no = nc2;
            }
            for (int set = 0; set < 2; set++) {
                const int *sx = set ? ox : cx, *se = set ? oe : ex;
                int ns = set ? no : nc;
                for (int k = 0; k < ns; k++)
                    for (int px = sx[k] < 0 ? 0 : sx[k]; px < se[k]; px++) {
                        int e = ve[px], o = vo[px];
                        if (e | o) line[px] = (uint8_t)(e | (o << 2) | 0x30 | (pair << 6));
                    }
            }
            for (int set = 0; set < 2; set++) {
                const int *sx = set ? ox : cx, *se = set ? oe : ex;
                int ns = set ? no : nc;
                uint8_t *v = a->spv[set ? pair * 2 + 1 : pair * 2];
                for (int k = 0; k < ns; k++) {
                    int xs = sx[k] < 0 ? 0 : sx[k];
                    if (se[k] > xs) memset(&v[xs], 0, (size_t)(se[k] - xs));
                }
            }
        }
    }
    if (lo < a->sprline_lo) a->sprline_lo = lo;
    if (hi > a->sprline_hi) a->sprline_hi = hi;
#ifdef AGA_SPR_REPLAY_CHECK
    {
        static uint8_t direct[sizeof a->sprline];
        if (!spr_buffers_clear(a)) aga_spr_replay_dirty++;
        memcpy(direct, line, sizeof direct);
        memset(line, 0, sizeof a->sprline);
        spr_replay_buffers(a, 0);
        aga_spr_replay_checked++;
        if (memcmp(direct, line, sizeof direct)) aga_spr_replay_mismatch++;
        if (!spr_buffers_clear(a)) aga_spr_replay_dirty++;
    }
#endif
}

/* Render all armed sprites for the current line into a->sprline (shres pixels,
   DIW coordinate origin). Encoding: bits0-3 colour, bit4 attached, bit5 odd base,
   bits6-7 pair, 0 = transparent. With clx, a->sprclx also gets one bit per
   sprite: collisions need every sprite, also the ones another sprite hides. */
void spr_build_line(aga_t *a, int clx)
{
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    /* clear only what the last build wrote: the rest is zero already, and a
       2 KB clear a line was most of what this function cost */
    if (a->sprline_hi > a->sprline_lo) memset(line + a->sprline_lo, 0, (size_t)(a->sprline_hi - a->sprline_lo));
    a->sprline_lo = linelen; a->sprline_hi = 0;
    a->sprline_valid = 1;

    if ((a->spr_touched || a->nsprres_ev) && !a->sprev_full) { spr_replay_line(a, clx); return; }

    int w = a->sprite_width;
    int pixw = 4 >> a->sprres;     /* shres pixels per sprite pixel */
    /* sprite positions map to hardware coordinates with a +9 lores pixel offset (UAE DIW_DDF_OFFSET) */
    const int hwoff = 9 * 4;

    int xs[2], ncp;
    a->sprclx_valid = clx;
    if (clx) {
        memset(a->sprclx, 0, sizeof a->sprclx);
        for (int n = 0; n < AGA_MAX_SPRITES; n++) {
            const sprite_t *s = &a->spr[n];
            if (!s->armed) continue;
            uint64_t solid = (s->data | s->datb) >> (64 - w);   /* 1 = a non-transparent pixel */
            if (!solid) continue;
            ncp = spr_copies(a, s->xpos, xs);
            for (int c = 0; c < ncp; c++)
                for (int k = 0; k < w; k++) {
                    if (!((solid >> (w - 1 - k)) & 1)) continue;
                    int x = xs[c] + hwoff + k * pixw;
                    for (int p = 0; p < pixw; p++)
                        if (x + p >= 0 && x + p < linelen) a->sprclx[x + p] |= (uint8_t)(1u << n);
                }
        }
    }

    /* positions that land on the same pixels (SSCAN2 ignores the 256 bit) */
    int pmask = (a->pf.fmode & 0x8000) ? ~0x400 : ~0;
    /* lower-numbered sprites have priority: draw 7..0 so lower overwrite */
    for (int pair = 3; pair >= 0; pair--) {
        sprite_t *se = &a->spr[pair * 2];
        sprite_t *so = &a->spr[pair * 2 + 1];
        int attached = so->attach;
        if (attached && se->armed && so->armed && (se->xpos & pmask) == (so->xpos & pmask)) {
            ncp = spr_copies(a, se->xpos, xs);
            for (int cp = 0; cp < ncp; cp++) {
                spr_mark(a, xs[cp] + hwoff, xs[cp] + hwoff + w * pixw, linelen);
                for (int k = 0; k < w; k++) {
                    int sh = w - 1 - k;
                    int c = (int)((se->data >> (64 - w + sh)) & 1) | ((int)((se->datb >> (64 - w + sh)) & 1) << 1)
                          | ((int)((so->data >> (64 - w + sh)) & 1) << 2) | ((int)((so->datb >> (64 - w + sh)) & 1) << 3);
                    if (!c) continue;
                    int x = xs[cp] + hwoff + k * pixw;
                    uint8_t v = (uint8_t)(c | 0x10 | 0x20 | (pair << 6));
                    for (int p = 0; p < pixw; p++) if (x + p >= 0 && x + p < linelen) line[x + p] = v;
                }
            }
            continue;
        }
        if (attached && (se->armed || so->armed)) {
            /* the pair is attached by the odd sprite's ATTACH bit alone (WinUAE
               drawing.cpp: dspr[].attached from the odd SPRxCTL), armed or not
               and wherever each one is: a pixel's colour is both sprites' bits,
               one not armed or not there giving none - as the replay has it.
               Drawn as two sprites of their own, the even one took its pair's
               four colours instead of the sixteen: Brian the Lion's glove,
               whose odd half stops 14 lines above the even one, came out light
               at its right edge where it is dark (from RGB2RTG, EMM #2085). */
            int xse[2], xso[2], span = w * pixw;
            ncp = spr_copies(a, se->xpos, xse);
            spr_copies(a, so->xpos, xso);
            for (int cp = 0; cp < ncp; cp++) {
                int be = xse[cp] + hwoff, bo = xso[cp] + hwoff;
                int lo = so->armed && (!se->armed || bo < be) ? bo : be;
                int hi = so->armed && (!se->armed || bo > be) ? bo + span : be + span;
                spr_mark(a, lo, hi, linelen);
                for (int x = lo < 0 ? 0 : lo; x < hi && x < linelen; x++) {
                    int c = 0;
                    if (se->armed && x >= be && x < be + span) {
                        int b = 63 - (x - be) / pixw;
                        c = (int)((se->data >> b) & 1) | ((int)((se->datb >> b) & 1) << 1);
                    }
                    if (so->armed && x >= bo && x < bo + span) {
                        int b = 63 - (x - bo) / pixw;
                        c |= ((int)((so->data >> b) & 1) << 2) | ((int)((so->datb >> b) & 1) << 3);
                    }
                    if (c) line[x] = (uint8_t)(c | 0x10 | 0x20 | (pair << 6));
                }
            }
            continue;
        }
        for (int which = 1; which >= 0; which--) {
            sprite_t *s = which ? so : se;
            if (!s->armed) continue;
            ncp = spr_copies(a, s->xpos, xs);
            for (int cp = 0; cp < ncp; cp++) {
                spr_mark(a, xs[cp] + hwoff, xs[cp] + hwoff + w * pixw, linelen);
                for (int k = 0; k < w; k++) {
                    int sh = 64 - w + (w - 1 - k);
                    int c = (int)((s->data >> sh) & 1) | ((int)((s->datb >> sh) & 1) << 1);
                    if (!c) continue;
                    int x = xs[cp] + hwoff + k * pixw;
                    uint8_t v = (uint8_t)(c | (which ? 0x20 : 0) | (pair << 6));
                    for (int p = 0; p < pixw; p++) if (x + p >= 0 && x + p < linelen) line[x + p] = v;
                }
            }
        }
    }
}
