/* AGA-PISTORM — the game's picture on the Amiga's own RGB port, as HAM6.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Sandbox entry turns the real display DMA off and the picture goes out on
 * HDMI. With agaboot's RGB port option on HAM6, core 1 - idle in Emu68 unless
 * async_log is on - drops a copy on the Amiga's own RGB port too, or there
 * only with HDMI black ($5951):
 *
 *   - agaboot lends exactly the real Chip RAM needed for the chosen HAMAREA
 *     and HAMBUFFER policy ($DFF1F4). SINGLE uses one physical HAM bitmap;
 *     DOUBLE uses a page-flipped pair.
 *   - the display is the whole overscan, lines $1A-$137 and DIW H $51-$1D1:
 *     what HDMI's Full size shows, border and all, where a real Amiga puts
 *     it - or a smaller window around the standard screen, down to 320 x 256
 *     ($599S). I followed the game's own window once and Banshee fucked me
 *     twice: its high-score screen is taller than the window it leaves set,
 *     and its in-game HUD is sprites in the border (BRDSPRT)
 *   - a new picture as often as one gets through, or a steady 25 a second
 *     ($59A1): each then stays up for two vertical blanks
 *   - every frame core 3 presents is halved to lores and HAM6-encoded: 16
 *     base colours (weighted k-means, kept until a new set clearly wins, so a
 *     still picture stays still), and per pixel the best of the nearest base
 *     colour and the three one-channel modifies, one pixel of lookahead
 *   - only the words that differ from what the hidden buffer already holds
 *     cross the bus; then the real COP1LC points at that buffer's list and
 *     the copper takes it at the next vertical blank - a frame never goes up
 *     half written
 *   - agaboot's Position slides the picture on the monitor ($54nn right,
 *     $55nn down); its calibration frame goes through this same path ($5953)
 *     outside the sandbox, so what's set there is what games get
 *
 * A classic PiStorm shifts about 1.9 MB/s with a HAM6 display running
 * (agaboot BUSTEST HAM6, Pi 3 A+, 2026-09-28) and a game changes about
 * 4.6 KB of HAM6 a frame on average, so the bus isn't the fucking limit -
 * the Pi 3's reading and encoding are.
 */
#include <stdint.h>
#include <string.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#include "ps_protocol.h"

extern void kprintf(const char *fmt, ...);

/* FRF_RUN_HIRES_FIT_V0_7: RUN is an OS/application screen, not a game
 * overscan policy. The virtual AGA framebuffer remains full hires; only
 * the physical HAM6 presentation session is forced to the standard 320x256
 * lowres canvas when the RUN child is currently HIRES/SHRES. */
extern volatile int aga_program_mode;
extern uint16_t aga_glue_peek_reg(uint32_t reg);
#define FRF_RGBOUT_BPLCON0_HIRES 0x8000u
#define FRF_RGBOUT_BPLCON0_SHRES 0x0040u

/* biggest picture the buffers and the lent block are sized for */
#define MAX_WORDS   24                      /* 384 lores pixels a row */
#define MAX_W       (MAX_WORDS * 16)
#define MAX_H       AGA_OUT_H               /* 286: every line the frame has */
#define FRAME_WORDS (6 * MAX_WORDS * MAX_H) /* 41184 */
#define FRAME_BYTES (FRAME_WORDS * 2)       /* 82368 */
#define LIST_BYTES  256
/* This frame against the real display: column 0 is DIW H 72 - two hires
   pixels a lores one - and row 0 is line $1A. I measured six games'
   snapshots: Banshee's $2C81 starts at column 114, Castle's $2C90 at 144. */
#define FRAME_H0    72
#define FRAME_V0    AGA_OUT_TOP

/* Picture size (agaboot RGB port, $599S): the real display window and the
   fetch for it. Every size keeps the standard screen ($2C81) where a real
   Amiga has it; the fetch starts in whole 16-pixel units from $81, and the
   wider ones reach as far right as DDFSTOP $D8 fetches. Smaller picture =
   fewer bytes to encode and send, and fits a monitor that can't show the
   whole damn overscan. */
static const struct { uint16_t w, h, h0, v0; } sizes[4] = {
    { 384, 286,  81, 26 },                  /* 0: whole overscan (default) */
    { 368, 280,  97, 32 },
    { 352, 272, 113, 36 },
    { 320, 256, 129, 44 },                  /* 3: standard screen */
};
static int g_w, g_h, g_words, g_h0, g_v0, g_plane_words, g_frame_words;   /* this session's */

static uint32_t rgb_block_bytes_for(int sz, int dbl)
{
    if (sz < 0 || sz > 3) sz = 0;
    uint32_t frame = 6u * (uint32_t)(sizes[sz].w / 16u) * (uint32_t)sizes[sz].h * 2u;
    uint32_t n = dbl ? 2u : 1u;
    return 511u + n * LIST_BYTES + n * frame;
}

_Static_assert(AGA_RGB_BLOCK_BYTES_MAX >= 2 * FRAME_BYTES + 2 * LIST_BYTES + 511, "RGB block max too small");
_Static_assert(81 + 384 == 0x1D1 && (81 - 17) / 2 + 8 * (MAX_WORDS - 1) == 0xD8, "fetch window");

/* ---- the bus ----------------------------------------------------------------
   Classic PiStorm: I hold the bus lock across a batch and the write goes
   straight to the GPIO. No blitter check (not a fucking register) and no
   soft-ICACHE invalidate - in the sandbox the 68k sees virtual chip RAM, so
   what lands in the real one is never code it could have cached. */
#ifdef PISTORM_CLASSIC
extern void ps_bus_acquire(void);
extern void ps_bus_release(void);
#define BUS_LOCK()    ps_bus_acquire()
#define BUS_UNLOCK()  ps_bus_release()
#define WR16(a, v)    ps_write_16_int((a), (v))
#define WR32(a, v)    ps_write_32_int((a), (v))
#else
/* PiStorm16/32 hide the bus lock in ps_protocol.c: every write takes it */
#define BUS_LOCK()    do { } while (0)
#define BUS_UNLOCK()  do { } while (0)
#define WR16(a, v)    ps_write_16((a), (v))
#define WR32(a, v)    ps_write_32((a), (v))
#endif

/* ---- state shared with the other cores --------------------------------------- */
static volatile uint32_t rgb_block;         /* the lent chip RAM, 0 = none (option off) */
static volatile uint32_t rgb_gen;           /* bumped by every start and stop */
static volatile int      rgb_run;           /* core 1 may drive the real display */
static volatile int      rgb_busy;          /* core 1 is inside a session */
static volatile int      rgb_core;          /* core 1 got here (it does not with async_log) */
static volatile int      rgb_calib;         /* this session draws agaboot's Position frame */
static volatile int      rgb_hdmi_req;      /* $5951: the next game on the RGB port only */
static volatile int      rgb_hdmi_off;      /* ...in force for this one */
static volatile int      rgb_off_x, rgb_off_y;   /* the picture right / down: lores pixels, lines */
static volatile uint32_t rgb_off_seq;       /* bumped when they change */
static volatile int      rgb_size;          /* $599S: sizes[] */
static volatile int      rgb_auto;         /* 1.115 adaptive 25/50 frame rate */
static volatile int      rgb_fps25;         /* $59A1: a new picture every second vblank, steady as hell */
static volatile int      rgb_doublebuf;     /* $59A3/$59A2 control word: DOUBLE / SINGLE physical HAM bitmap policy */
static volatile int      rgb_dither;        /* $59A5/$59A4: Smooth ordered dither / Exact rounded colours */
static const uint32_t * volatile rgb_frame; /* the last frame core 3 presented */
static volatile int      rgb_pitch = AGA_OUT_W;   /* ...its rows this many words apart */
static volatile uint32_t rgb_seq;           /* frames presented so far */
static volatile int      rgb_bright = 100;  /* RTG Image's brightness, in percent: taken back out */

static inline uint64_t now(void)  { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static inline uint64_t freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f; }
static inline uint32_t us(uint64_t ticks) { return (uint32_t)((ticks * 1000000u) / freq()); }
static inline int alive(uint32_t gen) { return rgb_run && rgb_gen == gen; }

/* ---- core 1's working set ---------------------------------------------------- */
static uint16_t tgt[MAX_H][MAX_W];          /* the picture in 12-bit colour, lores, as the RGB port shows it */
static uint64_t row_buf[MAX_W] __attribute__((aligned(64)));   /* one row of hires pairs, cached */
static uint16_t planes_new[FRAME_WORDS] __attribute__((aligned(64)));
static uint16_t shadow[2][FRAME_WORDS] __attribute__((aligned(64)));   /* what each real frame holds */
static uint16_t list_new[LIST_BYTES / 2];
static uint16_t list_img[2][LIST_BYTES / 2];                          /* what each real list holds */
static int      list_words;

static uint32_t hist[4096];                 /* edge-weighted colour counts of the picture */
static uint16_t ucol[4096];
static uint32_t uwt[4096];
static uint32_t mind[4096];
static uint8_t  bestpal[4096];              /* nearest base colour for each 12-bit colour, and its error */
static uint16_t besterr[4096];
static uint8_t  pal_r[16], pal_g[16], pal_b[16];
static uint16_t pal16[16];                  /* the same, $0RGB */
static int      have_pal, since_kmeans;
static uint64_t fit_at_choice;              /* the palette's error per unit of weight when chosen, x256 */

/* weight x d^2, d = -15..15 on one channel: green hurts most, blue least */
static int16_t  e_r[31], e_g[31], e_b[31];
#define ER(d) e_r[(d) + 15]
#define EG(d) e_g[(d) + 15]
#define EB(d) e_b[(d) + 15]
static uint8_t  q34[528];                   /* (a + b + 17) / 34: two 8-bit channels averaged, in 4 bits */
/* Same but with RTG Image's brightness taken back out first - it's there for
   HDMI's scanlines and the RGB port has none. A channel the brightness pushed
   past 255 comes back a bit dark: it was clipped there. */
static uint8_t  q34b[528];
/* 1.114 Smooth colours, retained without importing 1.114's RGBPORT allocator.
   Ordered Bayer 4x4 quantisation is anchored to physical HAM pixels, so a
   still image keeps the same pattern and the diff writer remains effective. */
static uint8_t  qd[16][528];
static const uint8_t bayer4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

static void qd_build(int percent)
{
    for (int t = 0; t < 16; t++)
        for (int i = 0; i < 528; i++) {
            int ss = (i * 100 + percent / 2) / percent;
            int v = (ss * 16 + t * 34 + 17) / (34 * 16);
            qd[t][i] = (uint8_t)(v > 15 ? 15 : v);
        }
}

void aga_rgbout_set_bright(int percent)
{
    if (percent < 100) percent = 100;
    for (int i = 0; i < 528; i++) {
        int v = (i * 100 + percent / 2) / percent;       /* the sum before the brightness */
        q34b[i] = (uint8_t)((v + 17) / 34 > 15 ? 15 : (v + 17) / 34);
    }
    qd_build(percent);
    rgb_bright = percent;
}

void aga_rgbout_set_dither(int on) { rgb_dither = on ? 1 : 0; }

static void tables_init(void)
{
    for (int d = -15; d <= 15; d++) {
        e_r[d + 15] = (int16_t)(3 * d * d);
        e_g[d + 15] = (int16_t)(4 * d * d);
        e_b[d + 15] = (int16_t)(2 * d * d);
    }
    for (int i = 0; i < 528; i++) q34[i] = (uint8_t)((i + 17) / 34 > 15 ? 15 : (i + 17) / 34);
    qd_build(rgb_bright);
}

static inline int err12(int a, int b)
{
    return ER((a >> 8) - (b >> 8)) + EG(((a >> 4) & 15) - ((b >> 4) & 15)) + EB((a & 15) - (b & 15));
}

/* Histogram of tgt. A colour counts six times where two or more channels jump
   against the pixel on its left: HAM can't reach those with one modify, so
   the base colours should be damn well sitting there. */
static inline void hist_row(const uint16_t *d)
{
    int pr = -99, pg = -99, pb = -99;               /* the line start counts as an edge */
    for (int x = 0; x < g_w; x++) {
        int c = d[x], r = c >> 8, g = (c >> 4) & 15, b = c & 15;
        int jumps = ((unsigned)(r - pr + 1) > 2) + ((unsigned)(g - pg + 1) > 2) + ((unsigned)(b - pb + 1) > 2);
        hist[c] += jumps >= 2 ? 6 : 1;
        pr = r; pg = g; pb = b;
    }
}

/* ---- 1a. a game's frame, grabbed while core 3 draws the next one
   Core 3 only starts over this page after presenting the next frame, and then
   at beam speed, 64 us a line. A row here is read in a few microseconds, so a
   read started before the next present stays ahead of it. The page is
   uncached, and one 64-bit load at a time stalled the in-order A53 on every
   damn one - 13 ms a frame, measured 2026-09-28. Rows go out eight loads at a
   time, all in flight before any is used. Position moves the picture; what
   falls outside the frame is black. */
static void row_copy(uint64_t *d, const uint64_t *s, int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
        uint64_t a4 = s[i + 4], a5 = s[i + 5], a6 = s[i + 6], a7 = s[i + 7];
        d[i] = a0; d[i + 1] = a1; d[i + 2] = a2; d[i + 3] = a3;
        d[i + 4] = a4; d[i + 5] = a5; d[i + 6] = a6; d[i + 7] = a7;
    }
    for (; i < n; i++) d[i] = s[i];
}

/* One row of tgt black. Not inline: in the row loop gcc sees tgt's size but
   cannot prove the row index, and Ubuntu's default _FORTIFY_SOURCE turns the
   memset into __memset_chk - and this kernel has no libc, so tough shit. */
static void __attribute__((noinline)) row_clear(uint16_t *d)
{
    memset(d, 0, MAX_W * sizeof(uint16_t));
}

static int snapshot(uint32_t gen, const uint32_t *frame, int pitch)
{
    const int ox = rgb_off_x, oy = rgb_off_y;
    const uint8_t *q = rgb_bright > 100 ? q34b : q34;
    const int dither = rgb_dither;
    /* display column c shows frame lores column (g_h0 - FRAME_H0) + c - ox,
       display row y frame row (g_v0 - FRAME_V0) + y - oy */
    const int f0 = g_h0 - FRAME_H0 - ox, r0 = g_v0 - FRAME_V0 - oy;
    int c_lo = f0 < 0 ? -f0 : 0, c_hi = AGA_OUT_W / 2 - f0;
    if (c_hi > g_w) c_hi = g_w;
    memset(hist, 0, sizeof hist);
    for (int y = 0; y < g_h; y++) {
        if ((y & 63) == 0 && !alive(gen)) return 0;
        uint16_t *d = tgt[y];
        int fy = r0 + y;
        row_clear(d);                                   /* black first; the frame's part next */
        if (fy >= 0 && fy < AGA_OUT_H && c_lo < c_hi) {
            row_copy(row_buf, (const uint64_t *)(frame + fy * pitch + 2 * (f0 + c_lo)), c_hi - c_lo);
            const uint8_t *brow = bayer4[y & 3];
            for (int x = c_lo; x < c_hi; x++) {
                uint64_t v = row_buf[x - c_lo];         /* two hires pixels, the left one high (big-endian) */
                uint32_t a = (uint32_t)(v >> 32), b = (uint32_t)v;
                const uint8_t *qq = dither ? qd[brow[x & 3]] : q;
                d[x] = (uint16_t)((qq[((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)] << 8)
                                | (qq[((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)] << 4)
                                |  qq[(a & 0xFF) + (b & 0xFF)]);
            }
        }
        hist_row(d);
    }
    return 1;
}

/* ---- 1b. agaboot's Position frame ---------------------------------------------
   The standard 320 x 256 screen ($2C81): white border, grey grid, red centre
   cross, offsets in green, moved the same way a game's picture will be. The
   edge of what the RGB port can show is dark grey. */
static void box(int x0, int y0, int w, int h, uint16_t c)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= g_h) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < g_w) tgt[y][x] = c;
    }
}

static void seven(int x, int y, int v, uint16_t c)   /* one digit, 12 x 20 */
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint8_t s = v < 0 ? 0x40 : seg[v % 10];          /* v < 0: a minus sign */
    if (s & 0x01) box(x, y, 12, 2, c);
    if (s & 0x02) box(x + 10, y, 2, 10, c);
    if (s & 0x04) box(x + 10, y + 10, 2, 10, c);
    if (s & 0x08) box(x, y + 18, 12, 2, c);
    if (s & 0x10) box(x, y + 10, 2, 10, c);
    if (s & 0x20) box(x, y, 2, 10, c);
    if (s & 0x40) box(x, y + 9, 12, 2, c);
}

static void number(int x, int y, int v, uint16_t c)  /* sign and up to three digits */
{
    int a = v < 0 ? -v : v;
    if (v < 0) seven(x, y, -1, c);
    seven(x + 16, y, a / 100, c);
    seven(x + 32, y, (a / 10) % 10, c);
    seven(x + 48, y, a % 10, c);
}

static void pattern(void)
{
    const int ox = rgb_off_x, oy = rgb_off_y;
    const uint16_t white = 0xFFF, grey = 0x666, dark = 0x333, red = 0xF00, green = 0x0F0;
    memset(tgt, 0, sizeof tgt);
    box(0, 0, g_w, 1, dark); box(0, g_h - 1, g_w, 1, dark); box(0, 0, 1, g_h, dark); box(g_w - 1, 0, 1, g_h, dark);
    int sx = 0x81 - g_h0 + ox, sy = 0x2C - g_v0 + oy;              /* the standard screen's corner */
    for (int i = 32; i < 320; i += 32) box(sx + i, sy, 1, 256, grey);
    for (int i = 32; i < 256; i += 32) box(sx, sy + i, 320, 1, grey);
    box(sx, sy, 320, 2, white); box(sx, sy + 254, 320, 2, white);
    box(sx, sy, 2, 256, white); box(sx + 318, sy, 2, 256, white);
    box(sx + 160 - 12, sy + 127, 24, 2, red); box(sx + 159, sy + 116, 2, 24, red);
    number(sx + 40, sy + 60, ox, green);
    number(sx + 216, sy + 60, oy, green);
    memset(hist, 0, sizeof hist);
    for (int y = 0; y < g_h; y++) hist_row(tgt[y]);
}

/* ---- 2. sixteen base colours --------------------------------------------------
   Colour 0 stays black: it's the border and every HAM line starts there. The
   other fifteen: weighted k-means over the picture's colours, starting at the
   heaviest and then each next farthest from those taken. Only runs when the
   colours in use have drifted from what the palette fits (or every 50
   frames), and a new set only replaces the old when it fits clearly better -
   so a still picture keeps its colours and its bitplanes, and those then cost
   no goddamn bus time at all. */
static int gather(uint64_t *total)
{
    int n = 0;
    uint64_t t = 0;
    for (int c = 0; c < 4096; c++)
        if (hist[c]) { ucol[n] = (uint16_t)c; uwt[n] = hist[c]; t += hist[c]; n++; }
    *total = t;
    return n;
}

static void kmeans(int n, uint16_t *cen)
{
    int k = n < 15 ? n : 15;
    int heavy = 0;
    for (int i = 1; i < n; i++) if (uwt[i] > uwt[heavy]) heavy = i;
    if (k) cen[0] = ucol[heavy];
    for (int i = 0; i < n; i++) mind[i] = (uint32_t)err12(ucol[i], cen[0]);
    for (int j = 1; j < k; j++) {
        int far = 0;
        uint64_t best = 0;
        for (int i = 0; i < n; i++) {
            uint64_t s = (uint64_t)uwt[i] * mind[i];
            if (s > best) { best = s; far = i; }
        }
        cen[j] = ucol[far];
        for (int i = 0; i < n; i++) {
            uint32_t e = (uint32_t)err12(ucol[i], cen[j]);
            if (e < mind[i]) mind[i] = e;
        }
    }
    for (int it = 0; it < 6; it++) {
        uint64_t sr[15] = {0}, sg[15] = {0}, sb[15] = {0}, sw[15] = {0};
        for (int i = 0; i < n; i++) {
            int c = ucol[i], bj = 0, be = 1 << 30;
            for (int j = 0; j < k; j++) {
                int e = err12(c, cen[j]);
                if (e < be) { be = e; bj = j; }
            }
            uint64_t w = uwt[i];
            sr[bj] += w * (uint64_t)(c >> 8); sg[bj] += w * (uint64_t)((c >> 4) & 15); sb[bj] += w * (uint64_t)(c & 15);
            sw[bj] += w;
        }
        for (int j = 0; j < k; j++) {
            if (!sw[j]) continue;
            int r = (int)((sr[j] * 2 + sw[j]) / (sw[j] * 2));
            int g = (int)((sg[j] * 2 + sw[j]) / (sw[j] * 2));
            int b = (int)((sb[j] * 2 + sw[j]) / (sw[j] * 2));
            cen[j] = (uint16_t)((r << 8) | (g << 4) | b);
        }
    }
    for (int j = k; j < 15; j++) cen[j] = 0;
}

/* how badly a palette fits: weighted error to nearest entry, per colour */
static uint64_t pal_cost(int n, const uint16_t *pal)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        int c = ucol[i], be = 1 << 30;
        for (int j = 0; j < 16; j++) {
            int e = err12(c, pal[j]);
            if (e < be) be = e;
        }
        sum += (uint64_t)uwt[i] * (uint32_t)be;
    }
    return sum;
}

/* the same for the palette in use, from its table */
static uint64_t cur_cost(int n)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (uint64_t)uwt[i] * besterr[ucol[i]];
    return sum;
}

#ifndef AGA_PI3
static uint64_t enc_row_hash[MAX_H];
static uint8_t enc_valid[MAX_H];
#endif

/* 1.115: Keep existing HAM6 colour slots where possible. Palette reordering
   otherwise makes unchanged pixels rewrite almost the entire RGB frame. */
static void frf_keep_palette_slots(uint16_t *cand)
{
    uint16_t out[16] = { 0 };
    uint8_t used_old[16] = { 0 }, used_new[16] = { 0 };
    for (int round = 1; round < 16; round++) {
        int bi = 1, bj = 1, be = 1 << 30;
        for (int i = 1; i < 16; i++) {
            if (used_old[i]) continue;
            for (int j = 1; j < 16; j++) {
                if (used_new[j]) continue;
                int e = err12(pal16[i], cand[j]);
                if (e < be) { be = e; bi = i; bj = j; }
            }
        }
        out[bi] = cand[bj];
        used_old[bi] = used_new[bj] = 1;
    }
    memcpy(cand, out, sizeof out);
}

static void choose_palette(void)
{
    uint64_t total;
    int n = gather(&total);
    if (!total) total = 1;
    if (have_pal) {
        uint64_t fit = cur_cost(n) * 256 / total;
        if (++since_kmeans < 50 && fit * 4 <= fit_at_choice * 5 + 256) return;   /* still fits: no k-means */
    }
    since_kmeans = 0;
    aga_diag_inc(AGA_D_RGB_KMEANS);
    uint16_t cand[16];
    cand[0] = 0;
    kmeans(n, cand + 1);
    uint64_t newc = pal_cost(n, cand);
    if (have_pal) {
        uint64_t curc = cur_cost(n);
        if (newc * 4 >= curc * 3) { fit_at_choice = curc * 256 / total; return; }   /* not clearly better */
    }
    if (have_pal) frf_keep_palette_slots(cand);
    /* New palette codes invalidate all prior encoded rows. */
#ifndef AGA_PI3
    memset(enc_valid, 0, sizeof enc_valid);
#endif
    memcpy(pal16, cand, sizeof pal16);
    for (int j = 0; j < 16; j++) { pal_r[j] = pal16[j] >> 8; pal_g[j] = (pal16[j] >> 4) & 15; pal_b[j] = pal16[j] & 15; }
    for (int c = 0; c < 4096; c++) {
        int bj = 0, be = 1 << 30;
        for (int j = 0; j < 16; j++) {
            int e = err12(c, pal16[j]);
            if (e < be) { be = e; bj = j; }
        }
        bestpal[c] = (uint8_t)bj;
        besterr[c] = (uint16_t)be;
    }
    have_pal = 1;
    fit_at_choice = newc * 256 / total;
    aga_diag_inc(AGA_D_RGB_PALETTES);
}

/* ---- 3. HAM6 ------------------------------------------------------------------
   Four real choices per pixel: nearest base colour, or keep the colour so far
   and set red, green or blue to the target. Each scores by its own error plus
   0.7 of the best the NEXT pixel could then do - that's what keeps a modify
   that looks fine here from fucking the next pixel two channels deep. Code
   bits 5-4: 00 base colour, 01 blue, 10 red, 11 green. */
static inline int look(int r, int g, int b, int ur, int ug, int ub, int be)
{
    int dr = ER(r - ur), dg = EG(g - ug), db = EB(b - ub), m = be;
    if (dg + db < m) m = dg + db;                     /* then set red */
    if (dr + db < m) m = dr + db;                     /* then set green */
    if (dr + dg < m) m = dr + dg;                     /* then set blue */
    return m;
}

/* FRF 1.115: on CM4, a repeat source scanline with the same palette need
   not be re-encoded. The original bus diff writer still decides which words
   cross into Chip RAM, preserving SINGLE's beam-synchronised path. */
#ifndef AGA_PI3
static uint64_t frf_row_fingerprint(const uint16_t *s, int w)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < w; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}
#endif

static int encode(uint32_t gen)
{
    const int P = g_plane_words;
    for (int y = 0; y < g_h; y++) {
        if ((y & 31) == 0 && !alive(gen)) return 0;
#ifndef AGA_PI3
        uint64_t hash = frf_row_fingerprint(tgt[y], g_w);
        if (enc_valid[y] && enc_row_hash[y] == hash) continue;
        enc_row_hash[y] = hash;
        enc_valid[y] = 1;
#endif
        const uint16_t *t = tgt[y];
        uint16_t *o = planes_new + y * g_words;
        int cr = pal_r[0], cg = pal_g[0], cb = pal_b[0];   /* every line starts from colour 0 */
        uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;
        for (int x = 0; x < g_w; x++) {
            int c = t[x], tr = c >> 8, tg = (c >> 4) & 15, tb = c & 15;
            int bp = bestpal[c];
            int e0 = besterr[c];
            int e1 = EG(cg - tg) + EB(cb - tb);           /* set red */
            int e2 = ER(cr - tr) + EB(cb - tb);           /* set green */
            int e3 = ER(cr - tr) + EG(cg - tg);           /* set blue */
            if (x + 1 < g_w) {
                int c2 = t[x + 1], ur = c2 >> 8, ug = (c2 >> 4) & 15, ub = c2 & 15, be = besterr[c2];
                e0 = 10 * e0 + 7 * look(pal_r[bp], pal_g[bp], pal_b[bp], ur, ug, ub, be);
                e1 = 10 * e1 + 7 * look(tr, cg, cb, ur, ug, ub, be);
                e2 = 10 * e2 + 7 * look(cr, tg, cb, ur, ug, ub, be);
                e3 = 10 * e3 + 7 * look(cr, cg, tb, ur, ug, ub, be);
            }
            int code;
            if (e1 < e0 && e1 <= e2 && e1 <= e3)  { code = 0x20 | tr; cr = tr; }
            else if (e2 < e0 && e2 <= e3)         { code = 0x30 | tg; cg = tg; }
            else if (e3 < e0)                     { code = 0x10 | tb; cb = tb; }
            else                                  { code = bp; cr = pal_r[bp]; cg = pal_g[bp]; cb = pal_b[bp]; }
            a0 = (a0 << 1) | (code & 1);
            a1 = (a1 << 1) | ((code >> 1) & 1);
            a2 = (a2 << 1) | ((code >> 2) & 1);
            a3 = (a3 << 1) | ((code >> 3) & 1);
            a4 = (a4 << 1) | ((code >> 4) & 1);
            a5 = (a5 << 1) | ((code >> 5) & 1);
            if ((x & 15) == 15) {
                int gx = x >> 4;
                o[gx]                   = (uint16_t)a0;
                o[P + gx]               = (uint16_t)a1;
                o[2 * P + gx]           = (uint16_t)a2;
                o[3 * P + gx]           = (uint16_t)a3;
                o[4 * P + gx]           = (uint16_t)a4;
                o[5 * P + gx]           = (uint16_t)a5;
            }
        }
    }
    return 1;
}

/* ---- 4. onto the real bus -----------------------------------------------------
   A few words at a time under the bus lock, and check INSIDE the lock that
   the session is still on: aga_rgbout_stop clears rgb_run and then takes the
   same lock, so once it returns no write of mine can land. (The other boards
   take the lock per write; there the stop waits for core 1 to leave the
   session.) */
static int put16(uint32_t gen, uint32_t addr, uint16_t v)
{
    BUS_LOCK();
    int ok = alive(gen);
    if (ok) WR16(addr, v);
    BUS_UNLOCK();
    return ok;
}

/* 68k and housekeeper wait on this lock too: let the bastards in a moment */
static inline void breathe(void)
{
    uint64_t t = now() + freq() / 1000000 + 1;
    while (now() < t) __asm__ volatile("yield");
}

#define BATCH 16          /* longwords per hold of the lock, about 30 us */

/* Diff planes_new against what real frame b holds and write that into it.
   Returns the words written, or -1 when the session died meanwhile. */
static int put_frame(uint32_t gen, uint32_t base, int b)
{
    const uint32_t *n = (const uint32_t *)planes_new;     /* big-endian: word 2i high, as the 68k sees it */
    uint32_t *s = (uint32_t *)shadow[b];
    uint32_t i = 0, total = (uint32_t)g_frame_words / 2;
    int words = 0;
    while (i < total) {
        while (i < total && n[i] == s[i]) i++;
        if (i == total) break;
        BUS_LOCK();
        if (!alive(gen)) { BUS_UNLOCK(); return -1; }
        for (int k = 0; k < BATCH && i < total; i++) {
            uint32_t v = n[i], d = v ^ s[i];
            if (!d) continue;
            uint32_t a = base + i * 4;
            if ((d >> 16) && (d & 0xFFFF)) { WR32(a, v); words += 2; }
            else if (d >> 16)              { WR16(a, v >> 16); words++; }
            else                           { WR16(a + 2, v & 0xFFFF); words++; }
            s[i] = v;
            k++;
        }
        BUS_UNLOCK();
        breathe();
    }
    return words;
}

/* Copper list of one frame: whole overscan window, sixteen colours, six plane
   pointers. Built every frame, written only where it differs from the real
   list - after the first frame that's just colours. */
static void build_list(uint16_t *l, uint32_t frame)
{
    int n = 0;
#define MOVE(r, v) do { l[n++] = (uint16_t)(r); l[n++] = (uint16_t)(v); } while (0)
    MOVE(0x08E, ((g_v0 & 0xFF) << 8) | (g_h0 & 0xFF));                   /* DIWSTRT: $1A51 for the whole frame */
    MOVE(0x090, (((g_v0 + g_h) & 0xFF) << 8) | ((g_h0 + g_w) & 0xFF));   /* DIWSTOP: OCS takes V8 = !V7, H8 set */
    MOVE(0x092, (g_h0 - 17) / 2);                                        /* DDFSTRT */
    MOVE(0x094, (g_h0 - 17) / 2 + 8 * (g_words - 1));                    /* DDFSTOP, $D8 at most */
    MOVE(0x102, 0x0000); MOVE(0x104, 0x0000);         /* BPLCON1 no scroll, BPLCON2 */
    MOVE(0x106, 0x0000);                              /* BPLCON3 on an ECS Denise */
    MOVE(0x108, 0x0000); MOVE(0x10A, 0x0000);         /* BPL1MOD / BPL2MOD */
    for (int s = 0; s < 8; s++) MOVE(0x142 + 8 * s, 0);   /* SPRxCTL: every sprite disarmed */
    for (int c = 0; c < 16; c++) MOVE(0x180 + 2 * c, pal16[c]);
    for (int p = 0; p < 6; p++) {
        uint32_t a = frame + (uint32_t)p * (uint32_t)g_plane_words * 2;
        MOVE(0x0E0 + 4 * p, a >> 16); MOVE(0x0E2 + 4 * p, a & 0xFFFF);
    }
    MOVE(0x100, 0x6A00);                              /* BPLCON0: six planes, HAM, colour */
    l[n++] = 0xFFFF; l[n++] = 0xFFFE;                 /* wait for a line that never comes */
#undef MOVE
    list_words = n;
}

static int put_list(uint32_t gen, uint32_t list, int b)
{
    for (int i = 0; i < list_words; i++) {
        if (list_img[b][i] == list_new[i]) continue;
        if (!put16(gen, list + 2 * (uint32_t)i, list_new[i])) return 0;
        list_img[b][i] = list_new[i];
    }
    return 1;
}

/* The real beam's line, 0-312: VPOSR's V8 read between two VHPOSRs that agree */
static int beam_line(void)
{
    for (int tries = 0; tries < 4; tries++) {
        uint32_t h1 = ps_read_16(0xDFF006), p = ps_read_16(0xDFF004), h2 = ps_read_16(0xDFF006);
        if ((h1 >> 8) == (h2 >> 8)) return (int)(((p & 1) << 8) | (h2 >> 8));
    }
    return 0;                                         /* reads failed: wait a frame */
}

/* ---- a session: a game, or agaboot's Position frame ------------------------------ */
static void session(uint32_t gen, uint32_t block, int calib)
{
    const uint64_t f = freq();
    const uint64_t frame_ticks = f * (313u * 64u) / 1000000u;   /* one PAL frame */

    /* this session's size, and its pace.
     * FRF_RUN_HIRES_FIT_V0_7: the framebuffer remains genuine HIRES. The
     * established RGB tap already consumes two source pixels per HAM6 pixel;
     * only select the standard 320x256 physical sink for a PROGRAM/RUN child
     * whose live BPLCON0 says HIRES/SHRES. */
    uint16_t run_bplcon0 = aga_glue_peek_reg(0x100u);
    int run_hires_fit = aga_program_mode &&
        (run_bplcon0 & (FRF_RGBOUT_BPLCON0_HIRES | FRF_RGBOUT_BPLCON0_SHRES));
    int sz = run_hires_fit ? 3 : rgb_size;
    if (sz < 0 || sz > 3) sz = 0;
    g_w = sizes[sz].w; g_h = sizes[sz].h; g_h0 = sizes[sz].h0; g_v0 = sizes[sz].v0;
    if (run_hires_fit)
        kprintf("[AGA] PROGRAM RUN: HAM6 HIRES fit 640x256 -> 320x256 (FRF 1.1.0 2:1 RGB tap)\n");
    g_words = g_w / 16;
    g_plane_words = g_words * g_h;
    g_frame_words = 6 * g_plane_words;
    const int fps25 = rgb_fps25 && !calib;
    const int nbuf = rgb_doublebuf ? 2 : 1;
    const uint32_t this_frame_bytes = (uint32_t)g_frame_words * 2u;
    /* The list area remains 512-aligned. SINGLE needs only one list + frame. */
    uint32_t lists = (block + 511u) & ~511u;
    uint32_t list_at[2]  = { lists, lists + LIST_BYTES };
    uint32_t frame_base = lists + (uint32_t)nbuf * LIST_BYTES;
    uint32_t frame_at[2] = { frame_base, frame_base + this_frame_bytes };

#ifndef AGA_PI3
    memset(enc_valid, 0, sizeof enc_valid);
#endif
    /* agaboot allocated the block cleared: physical frames are colour 0 all over */
    memset(shadow, 0, sizeof shadow);
    memset(list_img, 0, sizeof list_img);
    have_pal = 0;
    since_kmeans = 0;
    memset(pal16, 0, sizeof pal16);
    for (int b = 0; b < nbuf; b++) {
        build_list(list_new, frame_at[b]);
        if (!put_list(gen, list_at[b], b)) return;
    }
    /* list 0 at once: frame 0, black until the first picture */
    if (!put16(gen, 0xDFF080, (uint16_t)(lists >> 16)) || !put16(gen, 0xDFF082, (uint16_t)list_at[0])
        || !put16(gen, 0xDFF088, 0) || !put16(gen, 0xDFF096, 0x8380))   /* DMAEN | BPLEN | COPEN */
        return;
    aga_diag_set(AGA_D_RGB_STATE, calib ? 4 : 1);
    aga_diag_set(AGA_D_RGB_WIN_SIZE, ((uint32_t)g_w << 16) | (uint32_t)g_h);

    int front = 0;
    int auto_steady = 0, auto_misses = 0, auto_fits = 0;
    uint64_t auto_at = 0;
    uint64_t flip_due = 0, sec_t0 = now(), drawn_at = 0;
    uint32_t last = rgb_seq, sec_frames = 0, off_seen = rgb_off_seq - 1;
    while (alive(gen)) {
        uint64_t t0, t1;
        if (calib) {
            /* redraw when the offsets change, and a few times a second so
               both damn frames get it */
            while (alive(gen) && rgb_off_seq == off_seen && now() - drawn_at < f / 8) __asm__ volatile("yield");
            if (!alive(gen)) break;
            off_seen = rgb_off_seq;
            drawn_at = now();
            t0 = now();
            pattern();
#ifndef AGA_PI3
            memset(enc_valid, 0, sizeof enc_valid);
#endif
            t1 = now();
        } else {
            /* the newest presented frame */
            while (rgb_seq == last && alive(gen)) __asm__ volatile("wfe");
            if (!alive(gen)) break;
            uint32_t s = __atomic_load_n(&rgb_seq, __ATOMIC_ACQUIRE);
            const uint32_t *fr = rgb_frame;
            int pitch = rgb_pitch;
            if (s - last > 1) aga_diag_ctr[AGA_D_RGB_SKIPPED] += s - last - 1;
            last = s;
            t0 = now();
            if (!snapshot(gen, fr, pitch)) break;
            if (rgb_seq - s > 1) { aga_diag_inc(AGA_D_RGB_TORN); continue; }   /* drawn over mid-read */
            t1 = now();
        }
        choose_palette();
        if (!encode(gen)) break;
        uint64_t t2 = now();
        /* DOUBLE keeps the proven hidden-page flip. SINGLE deliberately uses
           the 1.11-style low-Chip policy: wait for the physical frame wrap,
           update the active list first, then copy only changed HAM words into
           the one bitmap. The visible window begins 26-44 PAL lines later,
           giving the diff writer useful headroom while retaining one bitmap. */
        while (now() < flip_due && alive(gen)) __asm__ volatile("yield");
        uint64_t t3 = now();
        int back = nbuf == 2 ? (front ^ 1) : 0;
        int words;
        if (nbuf == 1) {
            int line = beam_line();
            if (line < 300) {
                do { line = beam_line(); if (!alive(gen)) break; __asm__ volatile("yield"); } while (line < 300);
            }
            if (!alive(gen)) break;
            do { line = beam_line(); if (!alive(gen)) break; __asm__ volatile("yield"); } while (line >= 300);
            if (!alive(gen)) break;
            build_list(list_new, frame_at[0]);
            if (!put_list(gen, list_at[0], 0)) break;
            words = put_frame(gen, frame_at[0], 0);
            if (words < 0) break;
        } else {
            int w = put_frame(gen, frame_at[back], back);
            if (w < 0) break;
            words = w;
            build_list(list_new, frame_at[back]);
            if (!put_list(gen, list_at[back], back)) break;
        }
        uint64_t t4 = now();
        if (rgb_auto && !fps25 && !calib) {
            if (t4 - t0 > frame_ticks * 7 / 8) {
                if (auto_misses < 32) auto_misses++;
                auto_fits = 0;
            } else {
                if (auto_misses) auto_misses--;
                if (auto_fits < 128) auto_fits++;
            }
            if (!auto_steady && auto_misses >= 4) {
                auto_steady = 1;
                auto_at = t4;
            } else if (auto_steady && auto_fits >= 64 && t4 - auto_at >= 2 * f) {
                auto_steady = auto_misses = auto_fits = 0;
            }
        }
        if ((fps25 || auto_steady) && nbuf == 2)
            while (now() < flip_due + frame_ticks && alive(gen)) __asm__ volatile("yield");
        if (nbuf == 2) {
            if (!put16(gen, 0xDFF082, (uint16_t)list_at[back])) break;
            int v0 = beam_line();
            flip_due = now() + (uint64_t)(313 - v0) * 64 * f / 1000000 + f / 5000;
            front = back;
        } else {
            /* Same COP1LC remains installed. VARIABLE waits only for the next
               physical frame wrap (~50 Hz). FIXED25 holds one extra PAL frame
               before the next wrap, giving the old steady 25 Hz cadence. */
            flip_due = (fps25 || auto_steady) ? now() + frame_ticks : 0;
        }

        aga_diag_inc(AGA_D_RGB_FRAMES);
        aga_diag_set(AGA_D_RGB_WORDS_LAST, (uint32_t)words);
        aga_diag_ctr[AGA_D_RGB_KB_TOTAL] += (uint32_t)(words >> 9);
        aga_diag_set(AGA_D_RGB_SNAP_US, us(t1 - t0));
        aga_diag_set(AGA_D_RGB_ENCODE_US, us(t2 - t1));
        aga_diag_set(AGA_D_RGB_WAIT_US, us(t3 - t2));
        aga_diag_set(AGA_D_RGB_WRITE_US, us(t4 - t3));
        aga_diag_set(AGA_D_RGB_WIN_POS, ((uint32_t)(uint16_t)rgb_off_x << 16) | (uint16_t)rgb_off_y);
        sec_frames++;
        uint64_t el = now() - sec_t0;
        if (el >= f) {
            aga_diag_set(AGA_D_RGB_FPS10, (uint32_t)((uint64_t)sec_frames * 10 * f / el));
            sec_frames = 0;
            sec_t0 = now();
        }
    }
}

/* Core 1 from boot. Sits on its ass until a game starts with a block lent,
   or Position. */
void aga_rgbout_main(void)
{
    tables_init();
    rgb_core = 1;
    for (;;) {
        __asm__ volatile("dmb ish" ::: "memory");
        rgb_busy = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        while (!rgb_run) __asm__ volatile("wfe");
        uint32_t gen = rgb_gen, block = rgb_block;
        int calib = rgb_calib;
        rgb_busy = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        if (alive(gen) && block) session(gen, block, calib);
        while (alive(gen)) __asm__ volatile("wfe");     /* ended early: wait for the stop */
    }
}

/* ---- the other cores ------------------------------------------------------------- */
/* $DFF1F4 (long), outside the sandbox: agaboot's chip RAM, or 0 for no HAM6 */
/* 1.114 can publish GfxBase->copinit at $DFF1F8. FRF's proven HAM6
   lifecycle does not consume it; keep the ABI without changing ownership. */
void aga_rgbout_set_os_list(uint32_t addr) { (void)addr; }

void aga_rgbout_set_block(uint32_t addr)
{
    if (aga_sandbox_active || rgb_run) return;
    uint32_t need = rgb_block_bytes_for(rgb_size, rgb_doublebuf);
    if (addr && ((addr & 3) || addr < 0x1000 || addr + need > 0x200000)) {
        kprintf("[AGA] RGB port: rejecting block at %08x need=%u\n", addr, need);
        aga_diag_set(AGA_D_RGB_STATE, 3);
        addr = 0;
    }
    rgb_block = addr;
}

/* $5950 / $5951, outside the sandbox: HDMI as usual, or black while the
   next game is on the RGB port */
void aga_rgbout_set_hdmi_off(int off)
{
    if (!aga_sandbox_active) rgb_hdmi_req = off;
}

/* $599S: picture size (sizes[]). $59A1 / $59A0: steady 25 frames a second or
   as many as get through. Both latched when a game or the Position frame
   starts. */
void aga_rgbout_set_size(int s) { rgb_size = (s >= 0 && s <= 3) ? s : 0; }
void aga_rgbout_set_fps25(int on) { rgb_fps25 = on ? 1 : 0; }
void aga_rgbout_set_doublebuf(int on) { if (!aga_sandbox_active && !rgb_run) rgb_doublebuf = on ? 1 : 0; }

/* $54nn / $55nn, any time: the picture right / down by a signed byte (lores
   pixels, lines) */
void aga_rgbout_set_offset(int axis, int v)
{
    if (axis) rgb_off_y = v; else rgb_off_x = v;
    __atomic_add_fetch(&rgb_off_seq, 1, __ATOMIC_RELEASE);
}

/* core 3, per frame: black HDMI for this game? Only while the RGB port is
   actually being fed */
int aga_rgbout_hdmi_off(void)
{
    return rgb_hdmi_off && rgb_run && !rgb_calib;
}

static void go(void)
{
    rgb_gen++;
    __asm__ volatile("dmb ish" ::: "memory");
    rgb_run = 1;
    __asm__ volatile("dmb ish; sev" ::: "memory");
}

/* Sandbox entry, with the chipset loop confirmed running */
void aga_rgbout_start(uint32_t chip_real)
{
#ifdef FRF_VIRTUAL_AGA
    (void)chip_real;
    aga_diag_set(AGA_D_RGB_STATE, 0);
    return;
#endif
    uint32_t b = rgb_block;
    rgb_hdmi_off = 0;
    if (rgb_run) aga_rgbout_stop();                   /* a Position frame left up */
    rgb_block = b;
    if (!b) { aga_diag_set(AGA_D_RGB_STATE, 0); return; }
    if (!rgb_core) {
        aga_diag_set(AGA_D_RGB_STATE, 2);
        aga_diag_log("[AGA] RGB port: core 1 is not ours (async_log?) - no HAM6");
        return;
    }
    if (b + rgb_block_bytes_for(rgb_size, rgb_doublebuf) > chip_real) { /* must be real DMA-visible Chip RAM */
        aga_diag_set(AGA_D_RGB_STATE, 3);
        aga_diag_log("[AGA] RGB port: block beyond the real chip RAM - no HAM6");
        return;
    }
    rgb_calib = 0;
    rgb_hdmi_off = rgb_hdmi_req;
    aga_diag_set(AGA_D_RGB_HDMI_OFF, (uint32_t)rgb_hdmi_off);
    go();
    if (rgb_doublebuf)
        aga_diag_log(rgb_hdmi_off ? "[AGA] RGB port: HAM6 DOUBLE on, HDMI black" : "[AGA] RGB port: HAM6 DOUBLE on");
    else
        aga_diag_log(rgb_hdmi_off ? "[AGA] RGB port: HAM6 SINGLE on, HDMI black" : "[AGA] RGB port: HAM6 SINGLE on");
}

/* $5953 / $5952, outside the sandbox: agaboot's Position frame on the RGB
   port, on the block it just lent, or off again. agaboot puts the desktop's
   own display back afterwards (copper list and DMA, then RethinkDisplay). */
void aga_rgbout_calibrate(int on)
{
#ifdef FRF_VIRTUAL_AGA
    (void)on;
    return;
#endif
    if (aga_sandbox_active) return;
    if (on) {
        if (rgb_run || !rgb_block || !rgb_core) return;
        rgb_calib = 1;
        rgb_hdmi_off = 0;
        go();
    } else if (rgb_calib) {
        aga_rgbout_stop();
    }
}

/* Sandbox leave, BEFORE chip RAM is copied back: that copy goes over these
   lists, and a copper still running them would execute whatever the hell
   lands there. Also the end of a Position frame. */
void aga_rgbout_stop(void)
{
#ifdef FRF_VIRTUAL_AGA
    rgb_run = 0;
    rgb_calib = 0;
    rgb_hdmi_off = 0;
    rgb_block = 0;
    return;
#endif
    int was = rgb_run;
    rgb_run = 0;
    rgb_gen++;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    if (was) {
        uint64_t deadline = now() + freq() / 20;      /* 50 ms; it answers within a batch */
        while (rgb_busy && now() < deadline) __asm__ volatile("sev; yield");
        if (rgb_busy) aga_diag_log("[AGA] RGB port: core 1 slow to stop");
        ps_write_16(0xDFF096, 0x0180);                /* real copper and planes off */
        aga_diag_set(AGA_D_RGB_STATE, 0);
        aga_diag_log(rgb_calib ? "[AGA] RGB port: Position frame off" : "[AGA] RGB port: HAM6 off");
    }
    rgb_calib = 0;
    rgb_hdmi_off = 0;
    aga_diag_set(AGA_D_RGB_HDMI_OFF, 0);
    rgb_block = 0;                                    /* agaboot frees it */
}

/* Core 3, for every frame it presents. A woven frame's own field is every
   other row, and stays put while core 3 draws the next field between them. */
void aga_rgbout_frame(const uint32_t *frame, int pitch, int page)
{
    (void)page; /* legacy encoder uses source pixels, not page fingerprint hints */
    if (!rgb_run || rgb_calib) return;
    rgb_frame = frame;
    rgb_pitch = pitch;
    __atomic_store_n(&rgb_seq, rgb_seq + 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
}

/* 1.115 upstream hooks: legacy RGB engine remains self-contained on core 1.
   New async bus transfer and per-line cache are deliberately not enabled for
   HAMBUFFER SINGLE, where the physical Copper reads the same bitmap. */
void aga_rgbout_line(int y, int page, const uint32_t *line)
{ (void)y; (void)page; (void)line; }
void aga_rgbout_bus_service(void) { }
void aga_rgbout_set_auto(int on) { rgb_auto = on ? 1 : 0; }
void aga_rgbout_set_c32(int on) { (void)on; /* HAM6-only legacy path */ }
