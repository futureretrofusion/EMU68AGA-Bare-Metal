/* AGA-PISTORM - Blitter (immediate mode).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The blit engine is UAE's, from WinUAE blitter.cpp and in places from its
 * older form kept in E-UAE's blitter.c: do_fill is the fill step of
 * blitter_dofast (copied, names changed); build_fill_table builds UAE's
 * blit_filltable; blit_normal translates blitter_dofast / blitter_dofast_desc,
 * and blit_line the line mode, its steps and texture rotate in the older form
 * (blitter_line_proc, blitter_nxline) in the order of WinUAE's
 * actually_do_blit (each pixel written to BLTDPT after the step, the SING
 * dot, blitter_line_adat); the BLTBDAT write shifts as WinUAE's
 * blitter_loadbdat; the cycles per word in blit_duration are WinUAE's
 * (blit_cycle_diagram). The minterm function, the other register writes and
 * starting, finishing and timing a blit are mine.
 *   WinUAE blitter.cpp: (c) 1995 Bernd Schmidt, Alessandro Bissacco
 *                       (c) 2002 - 2025 Toni Wilen
 *   E-UAE blitter.c:    (c) 1995 Bernd Schmidt, Alessandro Bissacco
 *                       (c) 2002 - 2005 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include "internal.h"

static uint8_t fill_table[256][4][2];   /* [byte][ife*2 + carry_in] -> {out, carry_out} */
static int fill_table_ready;

static void build_fill_table(void)
{
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < 4; i++) {
            int fc = i & 1;
            int ife = i >> 1;
            int out = 0;
            for (int bit = 0; bit < 8; bit++) {
                int b = (d >> bit) & 1;
                int nfc = fc ^ b;
                if (ife) {
                    /* inclusive fill: bit set if fc or b or new fc */
                    if (fc || b || nfc) out |= 1 << bit;
                } else {
                    /* exclusive fill: set if (fc xor b) */
                    if (fc ^ b) out |= 1 << bit;
                }
                fc = nfc;
            }
            fill_table[d][i][0] = (uint8_t)out;
            fill_table[d][i][1] = (uint8_t)fc;
        }
    }
    fill_table_ready = 1;
}

void blt_reset(aga_t *a)
{
    memset(&a->blt, 0, sizeof a->blt);
    if (!fill_table_ready) build_fill_table();
}

uint16_t blt_dmaconr_bits(const aga_t *a)
{
    return (uint16_t)((a->blt.busy ? DMAF_BBUSY : 0) | (a->blt.zero ? DMAF_BZERO : 0));
}

static inline uint32_t blit_func(uint32_t sa, uint32_t sb, uint32_t sc, uint8_t mt)
{
    uint32_t d = 0;
    if (mt & 0x80) d |=  sa &  sb &  sc;
    if (mt & 0x40) d |=  sa &  sb & ~sc;
    if (mt & 0x20) d |=  sa & ~sb &  sc;
    if (mt & 0x10) d |=  sa & ~sb & ~sc;
    if (mt & 0x08) d |= ~sa &  sb &  sc;
    if (mt & 0x04) d |= ~sa &  sb & ~sc;
    if (mt & 0x02) d |= ~sa & ~sb &  sc;
    if (mt & 0x01) d |= ~sa & ~sb & ~sc;
    return d & 0xFFFF;
}

static inline uint16_t do_fill(const blitter_t *b, uint16_t d, int *fc)
{
    /* IFE and EFE both set fill exclusively - WinUAE's "weird fill mode",
       and weird it is */
    int ife = (b->con1 & (BLTCON1_IFE | BLTCON1_EFE)) == BLTCON1_IFE ? 2 : 0;
    int fc1 = fill_table[d & 255][ife + *fc][1];
    uint16_t out = fill_table[d & 255][ife + *fc][0] | (fill_table[d >> 8][ife + fc1][0] << 8);
    *fc = fill_table[d >> 8][ife + fc1][1];
    return out;
}

static void blit_normal(aga_t *a, int hsize, int vsize)
{
    blitter_t *b = &a->blt;
    uint8_t mt = (uint8_t)(b->con0 & 0xFF);
    int usea = b->con0 & BLTCON0_USEA, useb = b->con0 & BLTCON0_USEB;
    int usec = b->con0 & BLTCON0_USEC, used = b->con0 & BLTCON0_USED;
    int ash = (b->con0 >> 12) & 15, bsh = (b->con1 >> 12) & 15;
    int desc = b->con1 & BLTCON1_DESC;
    /* fill works ascending too, the carry running the other way through the
       words (WinUAE fills it anyway; its debugger only warns "fill without
       desc") */
    int fill = (b->con1 & (BLTCON1_IFE | BLTCON1_EFE)) != 0;
    uint32_t apt = b->apt, bpt = b->bpt, cpt = b->cpt, dpt = b->dpt;
    /* the shifters' previous words, cleared as the blit started (blit_start) */
    uint32_t preva = b->aold, prevb = b->bold;
    uint32_t bhold = b->bhold;
    uint16_t cdat = b->cdat, ddat = b->ddat;
    int zero = 1;
    int dodst = 0;
    uint32_t dstp = 0;

    for (int y = 0; y < vsize; y++) {
        int fc = (b->con1 & BLTCON1_FCI) ? 1 : 0;
        for (int x = 0; x < hsize; x++) {
            uint32_t adat, ahold;
            uint16_t mask = 0xFFFF;
            if (x == 0) mask &= b->afwm;
            if (x == hsize - 1) mask &= b->alwm;

            if (usea) {
                b->adat = chip_rd16(a, apt);
                apt += desc ? -2 : 2;
            }
            adat = b->adat & mask;
            if (desc) {
                ahold = ((adat << 16) | preva) >> (16 - ash);
            } else {
                ahold = ((preva << 16) | adat) >> ash;
            }
            preva = adat;

            if (useb) {
                uint32_t bdat = b->bdat = chip_rd16(a, bpt);
                bpt += desc ? -2 : 2;
                if (desc) bhold = ((bdat << 16) | prevb) >> (16 - bsh);
                else      bhold = ((prevb << 16) | bdat) >> bsh;
                prevb = bdat;
            }
            if (usec) {
                cdat = b->cdat = chip_rd16(a, cpt);
                cpt += desc ? -2 : 2;
            }
            if (dodst) chip_wr16(a, dstp, ddat);
            ddat = (uint16_t)blit_func(ahold, bhold, cdat, mt);
            if (fill) ddat = do_fill(b, ddat, &fc);
            if (ddat) zero = 0;
            if (used) {
                dodst = 1;
                dstp = dpt;
                dpt += desc ? -2 : 2;
            }
        }
        if (usea) apt += desc ? -b->amod : b->amod;
        if (useb) bpt += desc ? -b->bmod : b->bmod;
        if (usec) cpt += desc ? -b->cmod : b->cmod;
        if (used) dpt += desc ? -b->dmod : b->dmod;
    }
    if (dodst) chip_wr16(a, dstp, ddat);

    b->apt = apt; b->bpt = bpt; b->cpt = cpt; b->dpt = dpt;
    b->aold = (uint16_t)preva; b->bold = (uint16_t)prevb;
    b->bhold = (uint16_t)bhold;
    b->ddat = ddat;
    b->zero = zero;
}

/* ---- line mode ----------------------------------------------------------- */
static void blit_line(aga_t *a, int count)
{
    blitter_t *b = &a->blt;
    uint8_t mt = (uint8_t)(b->con0 & 0xFF);
    int shift = (b->con0 >> 12) & 15;
    int bsh = (b->con1 >> 12) & 15;
    int sign = (b->con1 & BLTCON1_SIGN) ? 1 : 0;
    int sing = b->con1 & BLTCON1_SING;
    int usea = b->con0 & BLTCON0_USEA;
    int usec = b->con0 & BLTCON0_USEC;
    uint32_t cpt = b->cpt, dpt = b->dpt;
    int16_t apt = (int16_t)b->apt;   /* error accumulator lives in BLTAPTL */
    uint16_t blineb = (uint16_t)((b->bdat >> bsh) | (b->bdat << (16 - bsh)));
    int zero = 1;
    int onedot = 0;                  /* SING: this row has its dot */

    /* As WinUAE's line mode (actually_do_blit): each pixel is worked out from
       C at BLTCPT and written to BLTDPT only after the step, and D then
       follows C. So the first dot goes wherever the program pointed BLTDPT -
       a filled-vector routine points it at a scratch word to keep each line's
       first dot out of the fill; written at BLTCPT it toggled the fill there
       and shredded the shape - and every other one where C was read. The
       write needs USEC, not USED (C must be on or nothing is drawn), and only
       with USEC does a Y step move C and start a new row for SING. 9 Fingers
       and State of the Art matched the real chip RAM after this in RGB2RTG
       (from RGB2RTG 6429272) - took me long enough. */
    for (int i = 0; i < count; i++) {
        int pixel = !sing || !onedot;
        onedot = 1;
        uint16_t ahold = (uint16_t)((b->adat & b->afwm & 0x8000) >> shift);
        b->aold = (uint16_t)((((uint32_t)b->aold << 16) | (b->adat & b->afwm)) >> shift);   /* blitter_line_adat */
        /* the texture bit is bit 0 of BLTBDAT rotated right by BSH: bit BSH for
           the first pixel, then BSH-1, BSH-2... (WinUAE blitter_line_minterm,
           blitter_nxline). Bit 15 drew every dash one damn pixel early. */
        uint16_t bhold = (blineb & 1) ? 0xFFFF : 0;
        uint16_t cdat = usec ? chip_rd16(a, cpt) : b->cdat;
        uint16_t ddat = (uint16_t)blit_func(ahold, bhold, cdat, mt);
        if (ddat) zero = 0;
        b->ddat = ddat;

        /* step */
        if (usea) apt += sign ? b->bmod : b->amod;
        if (!sign) {
            if (b->con1 & BLTCON1_SUD) {
                if (usec) {
                    if (b->con1 & BLTCON1_SUL) cpt -= b->cmod; else cpt += b->cmod;
                    onedot = 0;
                }
            } else {
                if (b->con1 & BLTCON1_SUL) { if (shift-- == 0) { shift = 15; cpt -= 2; } }
                else                       { if (++shift == 16) { shift = 0; cpt += 2; } }
            }
        }
        if (b->con1 & BLTCON1_SUD) {
            if (b->con1 & BLTCON1_AUL) { if (shift-- == 0) { shift = 15; cpt -= 2; } }
            else                       { if (++shift == 16) { shift = 0; cpt += 2; } }
        } else if (usec) {
            if (b->con1 & BLTCON1_AUL) cpt -= b->cmod; else cpt += b->cmod;
            onedot = 0;
        }
        sign = apt < 0;
        blineb = (uint16_t)((blineb << 1) | (blineb >> 15));
        if (pixel && usec)
            chip_wr16(a, dpt, ddat);
        dpt = cpt;
    }
    b->apt = (b->apt & 0xFFFF0000u) | (uint16_t)apt;
    b->cpt = cpt;
    b->dpt = dpt;
    b->con0 = (uint16_t)((b->con0 & 0x0FFF) | (shift << 12));
    /* BSH steps down once a pixel, as the texture moves along (WinUAE) */
    b->con1 = (uint16_t)((b->con1 & ~(BLTCON1_SIGN | 0xF000)) | (sign ? BLTCON1_SIGN : 0)
                         | (((bsh - count) & 15) << 12));
    b->bhold = (blineb & 1) ? 0xFFFF : 0;     /* B's hold: the texture bit it stopped on */
    b->zero = zero;
}

/* Report a blit finished: drop BBUSY, raise the interrupt, and release a
   copper stuck waiting on BLTDONE. */
static void blit_finish(aga_t *a)
{
    blitter_t *b = &a->blt;
    b->busy = 0;
    b->irq_cck = 0;
    b->fin_count++;
    aga_intreq_set(a, INTF_BLIT);       /* always at once - see INTREQ_write */
    cop_blitter_done(a);
}

/* NOTE ON THE DELAY, which is now the GLUE's job.
 *
 * A blit must not report finished inside its own BLTSIZE write, or a game that
 * arms its wait AFTER starting the blit misses the interrupt and waits for
 * ever. That much has not changed.
 *
 * What changed is the CLOCK the delay is measured against. It used to be
 * colour clocks, decremented from the chipset's own cycle loop, and that is
 * only meaningful while the loop runs those cycles at real-time speed. It does
 * not: presenting a frame costs about 900us, so the loop is always behind, and
 * whenever it is allowed to CATCH UP it replays thousands of colour clocks in
 * a few microseconds - expiring the delay long before the game has armed its
 * wait, which is the exact race the delay exists to prevent. That coupling is
 * why the loop could not be allowed to catch up, and why the audio generated
 * from the same clock ran 4.5% slow - a right shower of shit, both of them.
 *
 * So the core now only says "a blit is outstanding" and the glue, which has a
 * real timer, decides when enough WALL TIME has passed. The two clocks are
 * independent again: the loop may catch up freely, and the blit still cannot
 * finish inside its own write. */

/* How long to hold a blit "running" before reporting it finished.
 *
 * A fixed, short delay - NOT the time the real blitter would have taken.
 *
 * Modelling real duration (one cycle per active channel per word) is more
 * accurate and is the wrong thing here, because the duration becomes
 * load-bearing the moment anything waits on it. A copper list that waits on
 * BLTDONE stalls for the whole blit, and a big one costs frames: with a
 * per-word cost, Banshee drew its title and then crawled, sound stuck and its
 * damn quit key taking minutes to answer.
 *
 * The delay exists for exactly one reason: a blit must not report finished
 * inside its own BLTSIZE write, or a game that arms its wait AFTER starting
 * the blit misses the interrupt and waits for ever. 64 colour clocks is about
 * 9 microseconds - far more than the handful of instructions a game needs to
 * set its flag, and 0.1% of a frame, so nothing that waits on the blitter
 * notices. The blit DATA is finished before this returns either way. */
#define BLIT_DELAY_CCK 64

/* Colour clocks the real blitter spends on a blit: words x cycles per word
   from the channels it uses (WinUAE blit_cycle_diagram: 2 for none/one of
   A and D alone, 3 for three channels, 4 for ABCD; fill mode adds one when D
   runs without C), line mode about 4 a pixel. No bitplane DMA contention, so
   a lower bound - which is all the glue needs it for. */
static uint32_t blit_duration(const blitter_t *b, int hsize, int vsize)
{
    static const uint8_t cyc[16] = { 2,2,2,3, 3,3,3,4, 2,2,2,3, 3,3,3,4 };
    uint32_t n;
    if (b->con1 & BLTCON1_LINE) {
        n = 4;
    } else {
        unsigned ch = (b->con0 >> 8) & 0xF;           /* USEA USEB USEC USED */
        n = cyc[ch];
        if ((b->con1 & 0x18) && (ch & 1) && !(ch & 2)) n++;   /* fill, D without C */
    }
    return n * (uint32_t)hsize * (uint32_t)vsize + 4;
}

static void blit_start(aga_t *a, int hsize, int vsize)
{
    blitter_t *b = &a->blt;
    b->dur_cck = blit_duration(b, hsize, vsize);
    if (!aga_dma_enabled(a, DMAF_BLTEN)) {
        /* Blitter DMA disabled: the blit is latched and would start when
           enabled. For simplicity run it anyway (games always enable the
           damn thing). */
    }
    if (b->busy) blit_finish(a);       /* a new blit while one is outstanding */
    b->busy = 1;
    /* The shifters' previous words start at 0 (WinUAE, blit start: bltaold =
       bltbold = 0). They keep what the blit left in them afterwards, so a
       BLTBDAT written after a blit without B shifts in zeros: $FFFF with
       BSH 12 is the mask $000F, which Arte's rotating logo merges each
       column into its word with. */
    b->aold = b->bold = 0;
    a->blit_count++;
    if (b->con1 & BLTCON1_LINE) blit_line(a, vsize);
    else                        blit_normal(a, hsize, vsize);

    /* The DATA is done now, but the blit must not report finished yet.
     *
     * Completing inside the BLTSIZE write raises INTF_BLIT before the write
     * instruction has even retired, and a game that starts the blit and only
     * then arms its wait - sets its "blit running" flag AFTER writing BLTSIZE -
     * gets the interrupt first. Its handler clears a flag that has not been set,
     * the game sets it a moment later, and then waits for an interrupt that
     * already happened. Banshee sits in exactly that loop for ever:
     *
     *   3618cd9e: move.w $3618b926(pc),d7
     *   3618cda2: bne.w  $3618cd9e            ; wait for the handler to zero it
     *   3618cda6: lea    $dff000,a6
     *   3618cdac: move.w #$0040,$9a(a6)       ; INTENA: done with INTF_BLIT
     *
     * so its main loop never reaches the copper-list writes below it, nothing
     * is ever drawn, and the screen stays blank while the game runs happily.
     *
     * Deferring by the time the real blitter would have taken fixes the
     * handshake and is what the hardware does anyway. It also makes BBUSY in
     * DMACONR and the copper's BLTDONE wait behave, both of which already read
     * b->busy. */
    if (a->cfg.blit_immediate) blit_finish(a);
    else                       b->irq_cck = 1;   /* outstanding; the glue times it */
}

/* Is a blit waiting to be reported finished? */
int aga_blit_pending(aga_t *a)
{
    return a->blt.busy && a->blt.irq_cck;
}

uint32_t aga_blit_duration(aga_t *a)
{
    return a->blt.dur_cck;
}

/* 0: blits finished, 1: acknowledges that cleared two or more completions at
   once (a program that answers every BLIT interrupt lost one), 2: the frame
   of the last such */
void aga_blit_set_nohold(aga_t *a, int on) { a->blt.no_hold = on; }

uint32_t aga_peek_blit_irq(const aga_t *a, int i)
{
    return i == 0 ? a->blt.fin_count : i == 1 ? a->blt.lost : i == 2 ? a->blt.lost_frame : a->blt.held_count;
}

/* Enough wall time has passed: report it. */
void aga_blit_complete(aga_t *a)
{
    if (a->blt.busy && a->blt.irq_cck) blit_finish(a);
}

void blt_write(aga_t *a, uint32_t reg, uint16_t v)
{
    blitter_t *b = &a->blt;
    switch (reg) {
    case BLTCON0:  b->con0 = v; break;
    case BLTCON0L: b->con0 = (uint16_t)((b->con0 & 0xFF00) | (v & 0xFF)); break;
    case BLTCON1:  b->con1 = v; break;
    case BLTAFWM:  b->afwm = v; break;
    case BLTALWM:  b->alwm = v; break;
    case BLTCPTH:  b->cpt = (b->cpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTCPTL:  b->cpt = (b->cpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTBPTH:  b->bpt = (b->bpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTBPTL:  b->bpt = (b->bpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTAPTH:  b->apt = (b->apt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTAPTL:  b->apt = (b->apt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTDPTH:  b->dpt = (b->dpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTDPTL:  b->dpt = (b->dpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTSIZE: {
        int h = v & 0x3F; if (h == 0) h = 64;
        int vv = v >> 6;  if (vv == 0) vv = 1024;
        blit_start(a, h, vv);
        break;
    }
    case BLTSIZV:  b->sizev = v & 0x7FFF; break;
    case BLTSIZH: {
        int h = v & 0x7FF; if (h == 0) h = 2048;
        int vv = b->sizev; if (vv == 0) vv = 32768;
        blit_start(a, h, vv);
        break;
    }
    case BLTCMOD:  b->cmod = (int16_t)(v & 0xFFFE); break;
    case BLTBMOD:  b->bmod = (int16_t)(v & 0xFFFE); break;
    case BLTAMOD:  b->amod = (int16_t)(v & 0xFFFE); break;
    case BLTDMOD:  b->dmod = (int16_t)(v & 0xFFFE); break;
    case BLTCDAT:  b->cdat = v; break;
    case BLTBDAT: {                     /* shifted as it is loaded (HRM; WinUAE blitter_loadbdat) */
        int sh = b->con1 >> 12;
        b->bdat = v;
        b->bhold = (b->con1 & BLTCON1_DESC) ? (uint16_t)((((uint32_t)v << 16) | b->bold) >> (16 - sh))
                                            : (uint16_t)((((uint32_t)b->bold << 16) | v) >> sh);
        b->bold = v;
        break;
    }
    case BLTADAT:  b->adat = v; break;
    default: break;
    }
}
