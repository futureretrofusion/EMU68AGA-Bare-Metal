/* AGA-PISTORM - the game's picture on the Amiga's own RGB port, as HAM6.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * In the sandbox the real chipset shows nothing: entry turns its display DMA
 * off and the picture goes out on HDMI. With agaboot's RGB port option set to
 * HAM6, core 1 - idle in Emu68 unless async_log is on - puts a copy on the
 * Amiga's own RGB port as well (or instead, with HDMI black: $5951):
 *
 *   - before entry agaboot lends AGA_RGB_BLOCK_BYTES of real chip RAM
 *     ($DFF1F4, 32-bit, like the audio ring): two 384 x 286 six-plane
 *     frames and a copper list for each
 *   - the real display is the whole overscan area, lines $1A-$137 and DIW H
 *     $51-$1D1: the frame HDMI's Full size shows, border and all, where a
 *     real Amiga puts it - or a smaller window around the standard screen,
 *     down to 320 x 256 ($599S). (Following the game's display window instead
 *     cut Banshee twice: its high-score screen is taller than the window it
 *     leaves set, and its in-game HUD is sprites in the border, BRDSPRT.)
 *   - a new picture as often as one gets through, or a steady 25 a second
 *     ($59A1): every picture then stays up for two vertical blanks
 *   - or, chosen in agaboot's RGB port window ($59AD, 1.115), 32 colours
 *     instead of HAM6: five planes, each pixel its nearest colour - about a
 *     fifth fewer words a frame (Jim Power 21.5 -> 16.8 KB), no HAM fringes
 *   - every frame core 3 presents is halved to lores and encoded as HAM6:
 *     16 base colours (weighted k-means, kept until a new set is clearly
 *     better, so a still picture stays still) and, per pixel, the best of the
 *     nearest base colour and the three one-channel modifies, looking one
 *     pixel ahead
 *   - only the words that differ from what the hidden buffer already holds
 *     cross the bus; then the real COP1LC is pointed at that buffer's list,
 *     and the copper takes it at the next vertical blank - a frame is never
 *     shown half written
 *   - each buffer is a vertical ring (1.115): a picture that moved up or
 *     down - a pinball table, a vertical shooter - starts at another row of
 *     it, the copper jumping back to the ring's top where it wraps, so only
 *     the rows that came into view are sent (choose_scroll)
 *   - agaboot's RGB port Position moves the picture on the monitor ($54nn
 *     right, $55nn down): it draws its calibration frame through this same
 *     path ($5953), outside the sandbox, so what is set there is what games get
 *   - in a WHDLoad game only the game is shown: while the OS's own display is
 *     up - before WHDLoad takes the machine, and after it hands it back until
 *     S:WHDLoad-Cleanup leaves the sandbox - the RGB port is black (os_display),
 *     and with HDMI black that display goes to HDMI instead
 *
 * The classic PiStorm's bus carries about 1.9 MB/s with a HAM6 display
 * running (agaboot BUSTEST HAM6, Pi 3 A+, 2026-09-28) - and a busy game
 * changes far more than the 4.6 KB a frame I first measured: Jim Power
 * about 19.6 KB, 12.5 ms of bus a frame (2026-10-11). So on a classic
 * PiStorm the housekeeper core writes each frame (section 5) while core 1
 * reads and encodes the next one. */
#include <stdint.h>
#include <string.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#include "ps_protocol.h"

extern void kprintf(const char *fmt, ...);

/* The largest picture, which the buffers and the lent block are sized for */
#define MAX_WORDS   24                      /* 384 lores pixels a row */
#define MAX_W       (MAX_WORDS * 16)
#define MAX_H       AGA_OUT_H               /* 286: every line the frame has */
#define FRAME_WORDS (6 * MAX_WORDS * MAX_H) /* 41184 */
#define FRAME_BYTES (FRAME_WORDS * 2)       /* 82368 */
#define LIST_BYTES  256
/* My frame against the real display: column 0 is DIW H 72, two hires pixels
   a lores one, row 0 is line $1A. (Measured on six games' snapshots:
   Banshee's $2C81 starts at column 114, Castle's $2C90 at 144.) */
#define FRAME_H0    72
#define FRAME_V0    AGA_OUT_TOP

/* The picture's size (agaboot RGB port, $599S): the real display window and
   its fetch. Each keeps the standard screen ($2C81) where a real Amiga has
   it; the fetch starts in whole 16-pixel units from $81, and the wider ones
   reach as far right as DDFSTOP $D8 fetches. A smaller picture is fewer
   bytes to encode and to send, and fits a monitor that cannot show the
   whole overscan. */
static const struct { uint16_t w, h, h0, v0; } sizes[4] = {
    { 384, 286,  81, 26 },                  /* 0: the whole overscan frame (the default) */
    { 368, 280,  97, 32 },
    { 352, 272, 113, 36 },
    { 320, 256, 129, 44 },                  /* 3: the standard screen */
};
static int g_w, g_h, g_words, g_h0, g_v0, g_plane_words, g_frame_words;   /* this session's */

_Static_assert(AGA_RGB_BLOCK_BYTES >= 2 * FRAME_BYTES + 2 * LIST_BYTES + 511, "RGB block too small");
_Static_assert(81 + 384 == 0x1D1 && (81 - 17) / 2 + 8 * (MAX_WORDS - 1) == 0xD8, "fetch window");

/* ---- the bus ----------------------------------------------------------------
   On the classic PiStorm the lock is mine to hold across a batch, and the
   write goes straight to the GPIO: no blitter check (not a register) and no
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
/* PiStorm16/32 keep their bus lock inside ps_protocol.c: every write takes it */
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
static volatile int      rgb_fps25;         /* $59A1: a new picture every second vertical blank, steadily */
static volatile int      rgb_auto;          /* $59A9: Frame rate Auto - as many as get through, a steady 25 while 50 does not */
static volatile int      rgb_c32;           /* $59AD: 32 colours, five planes, instead of HAM6 */
static const uint32_t * volatile rgb_frame; /* the last frame core 3 presented */
static volatile int      rgb_pitch = AGA_OUT_W;   /* ...its rows this many words apart */
static volatile int      rgb_page = -1;     /* ...the page it is, -1 a woven frame (no lsum copy) */
/* Core 3's own copy of each page for the RGB port (1.115), made as it draws
   the lines (aga_rgbout_line): per lores pixel the sums of its two hires
   pixels' channels, red << 18 | green << 9 | blue - just what snapshot's
   tables take. Read from here, in the cache, a frame costs core 1 a fraction
   of the 3 ms it took to read the uncached page back. A Pi 4 only: on a Pi 3
   it cut those 8 ms to 2.8, but its stores - 450 KB a frame through the
   cluster's small shared cache - slowed core 3 itself: Jim Power skipped 902
   frames instead of 19 and HDMI went dark for a moment in the intro (Pi 3
   A+, 2026-10-11). lsum_whole[p]: page p's copy has every line of the frame
   drawn into it since its first line, while the RGB port was on - otherwise
   snapshot reads the page as before. */
static uint32_t lsum[2][AGA_OUT_H][AGA_OUT_W / 2] __attribute__((aligned(64)));
static volatile uint8_t lsum_whole[2];
/* Every drawn line's fingerprint, per page, from core 3 on a Pi 4 (1.115):
   two sums of the pixels times odd weights, one per column - change one
   pixel of a line and both sums move. Reads only what core 3 just drew,
   still in its cache, and stores one word a line - yet on a Pi 3, whose
   core 3 is already at its limit, even its 0.5 ms a frame cost Jim Power
   457 skipped frames instead of 19, so it is not installed there
   (aga_glue.c). Core 1 keeps tgt's rows from frame to frame and makes again
   only those whose line's fingerprint changed - and the colour histogram
   row by row with them, taking the old row's colours out (hist_row_sub).
   src_hash/src_ok: what tgt's row y was made from; src_key: the settings it
   was made with (Position, brightness, dither, size); in_hist: row y's
   colours are in hist. */
static uint64_t lhash[2][AGA_OUT_H];
static volatile uint8_t lhash_whole[2];
static uint32_t fpw[2][AGA_OUT_W];          /* the odd weights, one pair per column */
static uint64_t src_hash[MAX_H];
static uint8_t  src_ok[MAX_H], in_hist[MAX_H];
static uint64_t src_key;
#define SRC_OUTSIDE 0x0BADF00D0BADF00Dull   /* a row the frame does not reach: black */
static volatile uint32_t rgb_seq;           /* frames presented so far */
static volatile int      rgb_bright = 100;  /* RTG Image's brightness, in percent: taken back out */
static volatile int      rgb_dither;        /* $59A5 / $59A4: the colours dithered into 4 bits, or rounded */
static volatile uint32_t rgb_os_list;       /* graphics.library's copinit, from agaboot ($DFF1F8), 0 = not told */
static volatile uint32_t rgb_os_held;       /* core 1 is holding the OS's own display back (its session's gen): HDMI shows it */

static inline uint64_t now(void)  { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static inline uint64_t freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f; }
static inline uint32_t us(uint64_t ticks) { return (uint32_t)((ticks * 1000000u) / freq()); }
static inline int alive(uint32_t gen) { return rgb_run && rgb_gen == gen; }

/* ---- core 1's working set ---------------------------------------------------- */
static uint16_t tgt[MAX_H][MAX_W] __attribute__((aligned(64)));   /* the picture in 12-bit colour, lores, as the RGB port shows it */
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
static uint8_t  pal_r[32], pal_g[32], pal_b[32];
static uint16_t pal[32];                    /* the same, $0RGB */
/* This session's picture: HAM6 (6 planes, 16 base colours) or 32 colours
   (5 planes, the nearest of 32 for every pixel). Colour 0 is black in both -
   it is the border, and every HAM line starts from it. */
static int      g_c32, g_planes = 6, g_ncol = 16;
static int      have_pal, since_kmeans;
static uint64_t fit_at_choice;              /* the palette's error per unit of weight when chosen, x256 */
/* Rows encoded already (1.115): the encoder is deterministic - every row
   starts from colour 0, left to right - so a row of tgt that is the same as
   when it was last encoded, with the same palette, gives the same bits, and
   planes_new still holds them. row_hash is tgt's row now (snapshot),
   enc_hash what the row's bits were made from, enc_ok cleared by a new
   palette and a new session. The bus write already sends only what changed;
   this saves the encoding of the rows that did not. */
static uint64_t row_hash[MAX_H], enc_hash[MAX_H];
static uint8_t  enc_ok[MAX_H];

static inline uint64_t hash_row(const uint16_t *d, int w)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < w / 4; i++) {          /* w is a multiple of 16: four pixels a load */
        uint64_t v;
        memcpy(&v, d + 4 * i, sizeof v);        /* one load, and no aliasing to break */
        h ^= v;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    }
    return h;
}

/* weight x d^2 for a difference d of -15..15 in one channel: green counts most, blue least */
static int16_t  e_r[31], e_g[31], e_b[31];
#define ER(d) e_r[(d) + 15]
#define EG(d) e_g[(d) + 15]
#define EB(d) e_b[(d) + 15]
/* Each gun of a lores pixel into 4 bits: the sum s of its two hires pixels'
   8-bit channels, rounded, (s + 17) / 34 - with RTG Image's brightness taken
   back out first, (100 s + p / 2) / p at p %: that is there for HDMI's
   scanlines, and the RGB port has none. A channel the brightness pushed
   past 255 comes back a little dark - it was clipped there.
   Smooth colours (agaboot RGB port, Colours, 1.114): each gun into 4 bits
   with an ordered dither instead of rounding - a Bayer 4 x 4 threshold,
   fixed to the RGB port's own pixels, so a still picture keeps its pattern
   and costs no bus time. A 24-bit AGA colour between two 4-bit levels then
   comes out as the right mix of both: its hue kept, a gradient without
   steps. Rounding each gun on its own turned Banshee's dark brown
   (42, 26, 17) olive, red and green both to 2. Measured on the PC with this
   encoder on 14 AGA frames (scratch hamlab, 2026-10-11): the error after a
   3 x 3 blur, what a monitor and an eye mix, 22.8 rounded, 8.8 dithered
   (Banshee's intro 125.7 -> 5.4); the pixels themselves differ more, which
   is the pattern. A colour exactly on a level - every OCS/ECS one - is
   never moved: level + (t + 0.5) / 16, down, at threshold t of 16.
   Worked out, not looked up as up to 1.114 (q34, q34b, qd[16]), so the
   compiler can do a row sixteen pixels at a time (1.115): a 4-bit level is
   min(15, (16 s + tadd) / 544), tadd 272 rounded or 34 t + 17 dithered, the
   brightness through a reciprocal. Checked against those tables for every
   sum, threshold and brightness 100-200 % (877,387 cases, the session's
   scratch convcheck.c, 2026-10-11). */
static const uint8_t bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
static uint32_t tadd[4][MAX_W] __attribute__((aligned(64)));   /* per pixel, for the row's y & 3 */
static int      tadd_dither = -1;           /* what tadd was made for */
static uint64_t bright_m = ((uint64_t)1 << 32) / 100 + 1;    /* 2^32 / p, up: the brightness out */
static uint32_t row_sums[MAX_W] __attribute__((aligned(64)));  /* the uncached path's packed sums */

void aga_rgbout_set_bright(int percent)
{
    if (percent < 100) percent = 100;
    bright_m = ((uint64_t)1 << 32) / (uint64_t)percent + 1;
    rgb_bright = percent;
}

/* tadd's four rows, for rounded (272) or dithered (34 t + 17) levels */
static void tadd_build(int dither)
{
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < MAX_W; x++)
            tadd[r][x] = dither ? 34u * bayer4[r][x & 3] + 17u : 272u;
    tadd_dither = dither;
}

/* n pixels of packed channel sums (red << 18 | green << 9 | blue) to 12-bit
   colours - what q34 / q34b / qd used to give, sixteen at a time */
static void conv_row(uint16_t *d, const uint32_t *s, const uint32_t *ta, int n)
{
    const int br = rgb_bright > 100;
    const uint32_t hp = (uint32_t)rgb_bright / 2;
    const uint64_t m = bright_m;
    for (int x = 0; x < n; x++) {
        uint32_t v = s[x], r = v >> 18, g = (v >> 9) & 511, b = v & 511;
        if (br) {
            r = (uint32_t)(((uint64_t)(r * 100 + hp) * m) >> 32);
            g = (uint32_t)(((uint64_t)(g * 100 + hp) * m) >> 32);
            b = (uint32_t)(((uint64_t)(b * 100 + hp) * m) >> 32);
        }
        r = (16 * r + ta[x]) * 7711u >> 22;
        g = (16 * g + ta[x]) * 7711u >> 22;
        b = (16 * b + ta[x]) * 7711u >> 22;
        r = r > 15 ? 15 : r;
        g = g > 15 ? 15 : g;
        b = b > 15 ? 15 : b;
        d[x] = (uint16_t)((r << 8) | (g << 4) | b);
    }
}

void aga_rgbout_set_dither(int on) { rgb_dither = on ? 1 : 0; }

static void tables_init(void)
{
    uint64_t s = 0x2545F4914F6CDD1Dull;          /* the fingerprint's weights: odd, fixed */
    for (int k = 0; k < 2; k++)
        for (int x = 0; x < AGA_OUT_W; x++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            fpw[k][x] = (uint32_t)(s >> 16) | 1u;
        }
    for (int d = -15; d <= 15; d++) {
        e_r[d + 15] = (int16_t)(3 * d * d);
        e_g[d + 15] = (int16_t)(4 * d * d);
        e_b[d + 15] = (int16_t)(2 * d * d);
    }
}

static inline int err12(int a, int b)
{
    return ER((a >> 8) - (b >> 8)) + EG(((a >> 4) & 15) - ((b >> 4) & 15)) + EB((a & 15) - (b & 15));
}

/* The colour histogram of tgt, counting a colour six times where two or more
   channels jump against the pixel to its left: those are the pixels HAM
   cannot reach with one modify, so the base colours had better be there. */
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

/* the same row's colours taken back out, before I make the row again */
static inline void hist_row_sub(const uint16_t *d)
{
    int pr = -99, pg = -99, pb = -99;
    for (int x = 0; x < g_w; x++) {
        int c = d[x], r = c >> 8, g = (c >> 4) & 15, b = c & 15;
        int jumps = ((unsigned)(r - pr + 1) > 2) + ((unsigned)(g - pg + 1) > 2) + ((unsigned)(b - pb + 1) > 2);
        hist[c] -= jumps >= 2 ? 6 : 1;
        pr = r; pg = g; pb = b;
    }
}

/* ---- 1a. a game's frame, taken while core 3 draws the next one into the other page
   Core 3 starts over this page only after it has presented the next frame,
   and then at the speed of the beam, 64 us a line; a row is read here in a
   few    microseconds, so a read started before the next present stays ahead of
   that present. The page is uncached, and one 64-bit load at a time stalled
   in-order A53 on every one of them (13 ms a frame, measured 2026-09-28), so
   rows come out eight loads at a time, all in flight before any is used.
   Position moves the picture: whatever falls outside the frame is black. */
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
   memset into __memset_chk - which this kernel, with no libc, does not have. */
static void __attribute__((noinline)) row_clear(uint16_t *d)
{
    memset(d, 0, MAX_W * sizeof(uint16_t));
}

/* Core 3, every line it draws (aga_glue.c hook_line): the line's
   fingerprint, and on a Pi 4 its sums into the page's copy. Only while a
   game has the RGB port on - otherwise one compare and out. Both are whole
   from the frame's first line on. */
void aga_rgbout_line(int y, int page, const uint32_t *line)
{
    static int sums = -1;                       /* lsum: a Pi 4 only (see lsum) */
    if (sums < 0) sums = !aga_is_pi3();
    if ((unsigned)page > 1) return;
    if (!rgb_run || rgb_calib) { lsum_whole[page] = lhash_whole[page] = 0; return; }
    if (y == 0) { lhash_whole[page] = 1; lsum_whole[page] = (uint8_t)sums; }
    uint32_t h1 = 0, h2 = 0;
    for (int x = 0; x < AGA_OUT_W; x++) {       /* vectorised: plain multiply-adds */
        uint32_t p = line[x] & 0xFFFFFF;
        h1 += p * fpw[0][x];
        h2 += p * fpw[1][x];
    }
    lhash[page][y] = ((uint64_t)h1 << 32) | h2;
    if (!sums) return;
    uint32_t *d = lsum[page][y];
    for (int x = 0; x < AGA_OUT_W / 2; x++) {
        uint32_t a = line[2 * x], b = line[2 * x + 1];
        d[x] = ((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) << 18)
             | ((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) << 9)
             |  ((a & 0xFF) + (b & 0xFF));
    }
}

static int snapshot(uint32_t gen, const uint32_t *frame, int pitch, int page)
{
    const int ox = rgb_off_x, oy = rgb_off_y;
    const int dither = rgb_dither;
    /* core 3's cached copy of this page, when it is a whole frame (aga_rgbout_line) */
    const int cached = (unsigned)page <= 1 && lsum_whole[page];
    /* ...and its lines' fingerprints: a row whose line did not change is kept */
    const int fp = (unsigned)page <= 1 && lhash_whole[page];
    /* display column c shows frame lores column (g_h0 - FRAME_H0) + c - ox,
       display row y frame row (g_v0 - FRAME_V0) + y - oy */
    const int f0 = g_h0 - FRAME_H0 - ox, r0 = g_v0 - FRAME_V0 - oy;
    int c_lo = f0 < 0 ? -f0 : 0, c_hi = AGA_OUT_W / 2 - f0;
    if (c_hi > g_w) c_hi = g_w;
    /* new settings: every row again, and the histogram from nothing */
    uint64_t key = ((uint64_t)(uint16_t)ox << 48) | ((uint64_t)(uint16_t)oy << 32)
                 | ((uint64_t)(uint16_t)rgb_bright << 16) | ((uint64_t)dither << 8) | 1;
    if (key != src_key) {
        src_key = key;
        memset(src_ok, 0, sizeof src_ok);
        memset(in_hist, 0, sizeof in_hist);
        memset(hist, 0, sizeof hist);
    }
    if (tadd_dither != dither) tadd_build(dither);
    for (int y = 0; y < g_h; y++) {
        if ((y & 63) == 0 && !alive(gen)) { src_key = 0; return 0; }
        uint16_t *d = tgt[y];
        int fy = r0 + y;
        int inside = fy >= 0 && fy < AGA_OUT_H && c_lo < c_hi;
        uint64_t from = inside ? (fp ? lhash[page][fy] : 0) : SRC_OUTSIDE;
        if (src_ok[y] && src_hash[y] == from && (fp || !inside)) continue;   /* the same line: tgt, hist and row_hash stand */
        if (in_hist[y]) hist_row_sub(d);                /* the old row's colours out */
        row_clear(d);                                   /* black first; the frame's part next */
        if (inside && cached) {
            /* the channel sums of two hires pixels, from core 3's copy */
            conv_row(d + c_lo, &lsum[page][fy][f0 + c_lo], tadd[y & 3] + c_lo, c_hi - c_lo);
        } else if (inside) {
            int n = c_hi - c_lo;
            /* The row through Emu68's cached 1:1 map of physical RAM (the
               pages are mapped 1:1, so VA = PA), with its lines invalidated
               first: core 3 wrote them uncached, past any copy the cache
               kept from two frames ago (1.115). Read uncached it was most of
               the Pi 3's snapshot - 9.2 ms, 4.8 ms this way - and the
               emulation skipped no more frames for it (Jim Power, Pi 3 A+,
               2026-10-11): only core 1 reads, unlike lsum, where core 3
               stored. */
            uintptr_t src = (uintptr_t)(frame + fy * pitch + 2 * (f0 + c_lo)) + 0xffffff9000000000ull;
            for (uintptr_t l = src & ~(uintptr_t)63; l < src + (uintptr_t)n * 8; l += 64)
                __asm__ volatile("dc ivac, %0" :: "r"(l) : "memory");
            __asm__ volatile("dsb ish" ::: "memory");
            row_copy(row_buf, (const uint64_t *)src, n);
            for (int x = 0; x < n; x++) {
                uint64_t v = row_buf[x];                /* two hires pixels, the left one high (big-endian) */
                uint32_t a = (uint32_t)(v >> 32), b = (uint32_t)v;
                row_sums[x] = ((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) << 18)
                            | ((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) << 9)
                            |  ((a & 0xFF) + (b & 0xFF));
            }
            conv_row(d + c_lo, row_sums, tadd[y & 3] + c_lo, n);
        }
        hist_row(d);
        in_hist[y] = 1;
        row_hash[y] = hash_row(d, g_w);
        src_hash[y] = from;
        src_ok[y] = 1;
    }
    return 1;
}

/* ---- 1b. agaboot's Position frame ---------------------------------------------
   The standard 320 x 256 screen ($2C81) as a white border with a grey grid,
   a red centre cross and the offsets in green, moved exactly as a game's
   picture will be; the edge of what the RGB port can show in dark grey. */
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
   Colour 0 stays black: it is the border, and every HAM line starts from it.
   The other fifteen: weighted k-means over the picture's colours, started
   from the heaviest and then each next farthest from those taken. It runs
   only when the colours in use have drifted from what the palette fits (or
   every 50 frames), and a new set replaces the current one only when it fits
   clearly better - so a still picture keeps its colours and its bitplanes,
   which then cost no bus time at all, and a scrolling one keeps the rows the
   ring (section 6) can move instead of sending them. */
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
    const int kmax = g_ncol - 1;
    int k = n < kmax ? n : kmax;
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
        uint64_t sr[31] = {0}, sg[31] = {0}, sb[31] = {0}, sw[31] = {0};
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
    for (int j = k; j < kmax; j++) cen[j] = 0;
}

/* how badly a palette fits the picture's colours: weighted error to the nearest entry */
static uint64_t pal_cost(int n, const uint16_t *cand)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        int c = ucol[i], be = 1 << 30;
        for (int j = 0; j < g_ncol; j++) {
            int e = err12(c, cand[j]);
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

/* A new set of base colours, each put in the slot of the old colour nearest
   to it (1.115). k-means hands them over in its own order, so a new palette
   used to renumber nearly every base colour, and every pixel drawn with one
   changed its bits: one palette change rewrote the whole frame over the bus,
   up to 28,000 words (Jim Power on a CM4, 2026-10-11). Put back in its old
   slot, a colour that stayed keeps its index and its pixels their bits.
   Greedy, closest pair first; slot 0 stays black. */
static void keep_slots(uint16_t *cand)
{
    uint16_t out[32] = { 0 };
    uint8_t used_old[32] = { 0 }, used_new[32] = { 0 };
    out[0] = 0;
    for (int round = 1; round < g_ncol; round++) {
        int bi = 1, bj = 1, be = 1 << 30;
        for (int i = 1; i < g_ncol; i++) {
            if (used_old[i]) continue;
            for (int j = 1; j < g_ncol; j++) {
                if (used_new[j]) continue;
                int e = err12(pal[i], cand[j]);
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
    uint16_t cand[32] = { 0 };
    kmeans(n, cand + 1);
    uint64_t newc = pal_cost(n, cand);
    if (have_pal) {
        uint64_t curc = cur_cost(n);
        /* a quarter better at least (1.115; an eighth before): a new palette
           changes every row's codes, so both buffers are written whole -
           about 27,000 words each, two pictures late. A scrolling pinball
           table changed it every two seconds (Pinball Fantasies AGA, Pi 3
           A+, 2026-10-11). */
        if (newc * 4 >= curc * 3) { fit_at_choice = curc * 256 / total; return; }   /* not clearly better */
        keep_slots(cand);
    }
    memcpy(pal, cand, sizeof pal);
    memset(enc_ok, 0, sizeof enc_ok);       /* every row again, with these colours */
    for (int j = 0; j < g_ncol; j++) { pal_r[j] = pal[j] >> 8; pal_g[j] = (pal[j] >> 4) & 15; pal_b[j] = pal[j] & 15; }
    for (int c = 0; c < 4096; c++) {
        int bj = 0, be = 1 << 30;
        for (int j = 0; j < g_ncol; j++) {
            int e = err12(c, pal[j]);
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
   Per pixel the four real choices: the nearest base colour, or keep the
   colour so far and set red, green or blue to the target. Each is scored by
   its own error plus 0.7 of the best the NEXT pixel could then do, which is
   what stops a modify that looks fine here from leaving the next pixel two
   channels off. Code bits 5-4: 00 base colour, 01 blue, 10 red, 11 green. */
static inline int look(int r, int g, int b, int ur, int ug, int ub, int be)
{
    int dr = ER(r - ur), dg = EG(g - ug), db = EB(b - ub), m = be;
    if (dg + db < m) m = dg + db;                     /* then set red */
    if (dr + db < m) m = dr + db;                     /* then set green */
    if (dr + dg < m) m = dr + dg;                     /* then set blue */
    return m;
}

static int encode(uint32_t gen)
{
    const int P = g_plane_words;
    for (int y = 0; y < g_h; y++) {
        if ((y & 31) == 0 && !alive(gen)) return 0;
        if (enc_ok[y] && enc_hash[y] == row_hash[y]) continue;   /* its bits are in planes_new already */
        enc_hash[y] = row_hash[y];
        enc_ok[y] = 1;
        const uint16_t *t = tgt[y];
        uint16_t *o = planes_new + y * g_words;
        if (g_c32) {
            /* 32 colours: every pixel the nearest, five planes */
            uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
            for (int x = 0; x < g_w; x++) {
                int code = bestpal[t[x]];
                a0 = (a0 << 1) | (code & 1);
                a1 = (a1 << 1) | ((code >> 1) & 1);
                a2 = (a2 << 1) | ((code >> 2) & 1);
                a3 = (a3 << 1) | ((code >> 3) & 1);
                a4 = (a4 << 1) | ((code >> 4) & 1);
                if ((x & 15) == 15) {
                    int gx = x >> 4;
                    o[gx]         = (uint16_t)a0;
                    o[P + gx]     = (uint16_t)a1;
                    o[2 * P + gx] = (uint16_t)a2;
                    o[3 * P + gx] = (uint16_t)a3;
                    o[4 * P + gx] = (uint16_t)a4;
                }
            }
            continue;
        }
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
   A few words at a time under the bus lock, testing INSIDE it that the
   session is still on: aga_rgbout_stop clears rgb_run and then takes the
   same lock, so once it returns, no write of mine can land. (The other
   boards take the lock per write; there the stop waits for core 1 to leave
   its session.) */
static int put16(uint32_t gen, uint32_t addr, uint16_t v)
{
    BUS_LOCK();
    int ok = alive(gen);
    if (ok) WR16(addr, v);
    BUS_UNLOCK();
    return ok;
}

/* the 68k and the housekeeper wait on the lock too: let go for a moment */
static inline void breathe(void)
{
    uint64_t t = now() + freq() / 1000000 + 1;
    while (now() < t) __asm__ volatile("yield");
}

#define BATCH 16          /* longwords per hold of the lock, about 30 us */

/* The window, the fetch and BPLCON1-3: the same for a whole session. A
   32-colour list has no room left for them beside the ring's wrap (1.115),
   so they are written once, after the copper has been switched to my list;
   a HAM6 list - the Position frame's among them - still carries them,
   since there the OS's display is live until then and a LoadView without a
   copper list writes them straight to the chips (graphics' defras; found
   by review, 2026-10-11). In a game nothing else writes them. */
static int g_const_in_list;                 /* this session's lists carry them */
static int put_const(uint32_t gen)
{
    return put16(gen, 0xDFF08E, (uint16_t)(((g_v0 & 0xFF) << 8) | (g_h0 & 0xFF)))            /* DIWSTRT: $1A51 for the whole frame */
        && put16(gen, 0xDFF090, (uint16_t)((((g_v0 + g_h) & 0xFF) << 8) | ((g_h0 + g_w) & 0xFF)))   /* DIWSTOP: OCS takes V8 = !V7, H8 set */
        && put16(gen, 0xDFF092, (uint16_t)((g_h0 - 17) / 2))                                  /* DDFSTRT */
        && put16(gen, 0xDFF094, (uint16_t)((g_h0 - 17) / 2 + 8 * (g_words - 1)))              /* DDFSTOP, $D8 at most */
        && put16(gen, 0xDFF102, 0) && put16(gen, 0xDFF104, 0)                                /* BPLCON1 no scroll, BPLCON2 */
        && put16(gen, 0xDFF106, 0);                                                           /* BPLCON3 on an ECS Denise */
}

/* The words a list can take, and what a frame's list needs at most: modulos,
   sprites, colours, plane pointers, BPLCON0, the wrap (two waits, the $FFDF
   one, four moves) and the end. 94 words for HAM6, 122 with 32 colours. */
#define LIST_MAX_WORDS (LIST_BYTES / 2)
#define LIST_CONST_WORDS 14
static inline int list_need(void) { return 4 + 16 + 2 * g_ncol + 4 * g_planes + 2 + 14 + 2; }

/* The copper list of one frame: sixteen colours and six plane pointers (32
   and five with 32 colours), the pointers at ring row s. Built every frame
   and written where it differs from what the real list holds. Dark: no
   planes and every colour black, the RGB port showing nothing at all (a HAM
   picture cannot be blacked out by its palette: the modify pixels carry
   their own colour).
   The ring: screen row y shows ring row (s + y) mod g_h. Where ring row
   g_h - 1 has been fetched, a plane's pointer would run on into the next
   plane, so on that line the copper sets both modulos to minus a whole
   plane - added at the end of the line's fetch, they take every pointer
   back to its plane's first row - and on the next line back to 0. Each
   wait is at the start of its line (horizontal $06), before the fetch
   begins; past line 255 the $FFDF wait comes first, as the copper's
   compare has no V8. */
static void build_list(uint16_t *l, uint32_t frame, int s, int dark)
{
    int n = 0, past = 0;
#define MOVE(r, v) do { l[n++] = (uint16_t)(r); l[n++] = (uint16_t)(v); } while (0)
#define WAITL(line) do { int L_ = (line);                                                   \
        if (L_ >= 256 && !past) { l[n++] = 0xFFDF; l[n++] = 0xFFFE; past = 1; }             \
        l[n++] = (uint16_t)(((L_ & 0xFF) << 8) | 0x07); l[n++] = 0xFFFE; } while (0)
    if (g_const_in_list) {
        MOVE(0x08E, ((g_v0 & 0xFF) << 8) | (g_h0 & 0xFF));                   /* DIWSTRT, as put_const */
        MOVE(0x090, (((g_v0 + g_h) & 0xFF) << 8) | ((g_h0 + g_w) & 0xFF));   /* DIWSTOP */
        MOVE(0x092, (g_h0 - 17) / 2);                                        /* DDFSTRT */
        MOVE(0x094, (g_h0 - 17) / 2 + 8 * (g_words - 1));                    /* DDFSTOP */
        MOVE(0x102, 0x0000); MOVE(0x104, 0x0000); MOVE(0x106, 0x0000);       /* BPLCON1-3 */
    }
    MOVE(0x108, 0x0000); MOVE(0x10A, 0x0000);         /* BPL1MOD / BPL2MOD */
    for (int i = 0; i < 8; i++) MOVE(0x142 + 8 * i, 0);   /* SPRxCTL: every sprite disarmed */
    for (int c = 0; c < g_ncol; c++) MOVE(0x180 + 2 * c, dark ? 0 : pal[c]);
    for (int p = 0; p < g_planes; p++) {
        uint32_t a = frame + (uint32_t)p * (uint32_t)g_plane_words * 2 + (uint32_t)s * (uint32_t)g_words * 2;
        MOVE(0x0E0 + 4 * p, a >> 16); MOVE(0x0E2 + 4 * p, a & 0xFFFF);
    }
    MOVE(0x100, dark ? 0x0200 : g_c32 ? 0x5200 : 0x6A00);   /* BPLCON0: six planes, HAM, colour; five; or none */
    if (s && !dark) {
        uint16_t back = (uint16_t)-(int)(g_plane_words * 2);    /* minus a plane: -13728 for the full frame */
        int line = g_v0 + (g_h - s) - 1;                        /* the line that shows ring row g_h - 1 */
        WAITL(line);     MOVE(0x108, back); MOVE(0x10A, back);
        WAITL(line + 1); MOVE(0x108, 0x0000); MOVE(0x10A, 0x0000);
    }
    l[n++] = 0xFFFF; l[n++] = 0xFFFE;                 /* wait for a line that never comes */
#undef WAITL
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
    return 0;                                         /* as if at the top: then a whole frame is waited out */
}

/* ---- 5. the writes on the housekeeper core (1.115) ------------------------------------
   A word costs about 1.3 us on the bus, and a Jim Power frame changes some
   10,000 of them - 12.5 ms of the 22.5 ms core 1 spent on a frame on a Pi 3
   A+, after snapshot 4.8 and encode 5.1 (2026-10-11). That is the PiStorm's
   own time for a write - the CPLD's 68000 bus cycle and its hand-over - not
   the Pi's and not the display's DMA: a batch that left the data pins as
   outputs changed nothing, to the microsecond, and 32 colours (five planes
   instead of six) cost the same per word (657 against 659 us a KB). So on a
   classic PiStorm core 2, the housekeeper - already the audio ring's bus
   writer - writes the frame a few words each time round its loop
   (aga_rgbout_bus_service), while core 1 reads and encodes the next
   picture. Core 1 hands over a job: the encoded frame copied to planes_out,
   the list to list_out, which real buffer, from when (the last flip must
   have happened: the buffer is hidden) and until when the flip is held
   (Fixed 25, Auto's steady 25). The job writes what differs from the
   buffer's shadow, then the list, then COP1LC's low word, and leaves the
   next flip_due behind. On the other boards core 1 runs the same job
   itself, at once, as before. */
enum { JOB_IDLE, JOB_POSTED, JOB_FRAME, JOB_LIST, JOB_HOLD, JOB_DONE };
#ifdef PISTORM_CLASSIC
#define JOB_ON_CORE2 1
#else
#define JOB_ON_CORE2 0
#endif
#define JOB_WORDS 4       /* bus words a step on core 2: the housekeeper samples IPL between steps */
#define JOB_SCAN  1024    /* words compared a step at most */
static struct {
    volatile int state;
    uint32_t gen, base, list;
    int b, words, failed;
    int s;                /* the ring row the buffer's picture starts at */
    int seg;              /* plane * 2 + 0: screen rows 0.. at ring row s; + 1: the rest, at ring row 0 */
    uint32_t i;           /* the next word of that stretch */
    int li;               /* the next word of the list */
    uint64_t start_after, hold_until;
    uint64_t t_start, t_written, flip_due;   /* results: the writes began, the list was in, the next flip */
} job;
static uint16_t planes_out[FRAME_WORDS] __attribute__((aligned(64)));
static uint16_t list_out[LIST_BYTES / 2];
static int      list_out_words;

/* Not inline, for the same reason as row_clear: gcc knows planes_out's size,
   not n, and the fortified memcpy would be __memcpy_chk */
static void __attribute__((noinline)) copy_words(uint16_t *d, const uint16_t *s, size_t n)
{
    memcpy(d, s, n * sizeof(uint16_t));
}

static int job_end(int failed)
{
    job.failed = failed;
    __atomic_store_n(&job.state, JOB_DONE, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    return 0;
}

/* One step: at most max_words bus words, max_scan longwords compared. 0 when
   the job is done or the session ended. */
static int job_step(int max_words, uint32_t max_scan)
{
    if (!alive(job.gen)) return job_end(1);
    switch (job.state) {
    case JOB_POSTED:
        if (now() < job.start_after) return 1;
        job.t_start = now();
        job.state = JOB_FRAME;
        /* fall through */
    case JOB_FRAME: {
        /* Each plane in two stretches, contiguous in both: screen rows 0 to
           g_h - s - 1 into ring rows s to g_h - 1, then the rest into ring
           rows 0 to s - 1. The bus takes 16 bits a write either way. */
        const uint16_t *n = planes_out;
        uint16_t *sh = shadow[job.b];
        const uint32_t W = (uint32_t)g_words, P = (uint32_t)g_plane_words, H = (uint32_t)g_h, S = (uint32_t)job.s;
        uint32_t scanned = 0;
        int w = 0, locked = 0;
        while (job.seg < 2 * g_planes) {
            uint32_t p = (uint32_t)job.seg >> 1, k = (uint32_t)job.seg & 1;
            uint32_t len = (k ? S : H - S) * W;
            uint32_t src = p * P + (k ? (H - S) * W : 0);
            uint32_t dst = p * P + (k ? 0 : S * W);
            uint32_t i = job.i;
            while (i < len && scanned < max_scan && w < max_words) {
                uint16_t v = n[src + i];
                if (v != sh[dst + i]) {
                    if (!locked) {
                        BUS_LOCK();
                        if (!alive(job.gen)) { BUS_UNLOCK(); return job_end(1); }
                        locked = 1;
                    }
                    WR16(job.base + (dst + i) * 2, v);
                    sh[dst + i] = v;
                    w++;
                }
                i++;
                scanned++;
            }
            if (i < len) { job.i = i; break; }      /* this step's share is used up */
            job.seg++;
            job.i = 0;
        }
        if (locked) BUS_UNLOCK();
        job.words += w;
        if (job.seg < 2 * g_planes) return 1;
        job.state = JOB_LIST;
        return 1;
    }
    case JOB_LIST:
        for (int w = 0; w < max_words && job.li < list_out_words; job.li++) {
            int k = job.li;
            if (list_img[job.b][k] == list_out[k]) continue;
            if (!put16(job.gen, job.list + 2 * (uint32_t)k, list_out[k])) return job_end(1);
            list_img[job.b][k] = list_out[k];
            w++;
        }
        if (job.li < list_out_words) return 1;
        job.t_written = now();
        job.state = JOB_HOLD;
        /* fall through */
    case JOB_HOLD: {
        if (now() < job.hold_until) return 1;
        /* shown from the next vertical blank, where the copper reloads COP1LC.
           Until then the old frame is on screen: the next job waits to the
           end of this one, at 64 us a line (PAL; an NTSC frame ends sooner),
           plus a margin. */
        if (!put16(job.gen, 0xDFF082, (uint16_t)job.list)) return job_end(1);
        const uint64_t f = freq();
        int v0 = beam_line();
        job.flip_due = now() + (uint64_t)(313 - v0) * 64 * f / 1000000 + f / 5000;
        return job_end(0);
    }
    default:
        return 0;
    }
}

/* Core 1: the encoded frame to real buffer b, shown from the blank after
   start_after, held until hold_until. On a classic PiStorm core 2 takes it
   from here; elsewhere it is written now. */
static void job_post(uint32_t gen, uint32_t base, uint32_t list, int b, int s,
                     uint64_t start_after, uint64_t hold_until)
{
    copy_words(planes_out, planes_new, (size_t)g_frame_words);
    copy_words(list_out, list_new, (size_t)list_words);
    list_out_words = list_words;
    job.gen = gen; job.base = base; job.list = list; job.b = b;
    job.s = s; job.seg = 0;
    job.i = 0; job.li = 0; job.words = 0; job.failed = 0;
    job.start_after = start_after; job.hold_until = hold_until;
    job.t_start = job.t_written = 0;
    __atomic_store_n(&job.state, JOB_POSTED, __ATOMIC_RELEASE);
    if (JOB_ON_CORE2) {
        __asm__ volatile("sev");
        return;
    }
    while (job_step(2 * BATCH, 0xFFFFFFFFu)) {
        if (job.state == JOB_POSTED) aga_nap_before(job.start_after);      /* with aga.sleep: asleep */
        else if (job.state == JOB_HOLD) aga_nap_before(job.hold_until);
        else breathe();
    }
}

/* Core 1: wait for the job to be done. 1 = it was (or there was none), 0 =
   the session ended. Core 2 drops a job within a step once the session ends. */
static int job_wait(uint32_t gen)
{
    for (;;) {
        int st = __atomic_load_n(&job.state, __ATOMIC_ACQUIRE);
        if (st == JOB_IDLE) return alive(gen);
        if (st == JOB_DONE) return !job.failed && alive(gen);
        if (!alive(gen)) return 0;
        aga_nap();
    }
}

/* Core 1, at a session's start and end: no job of core 2's left running.
   A session that has ended loses its job within a step; one still alive
   finishes it - a whole frame's words and a held flip, well inside 200 ms. */
static void job_settle(void)
{
    uint64_t deadline = now() + freq() / 5;
    while (now() < deadline) {
        int st = __atomic_load_n(&job.state, __ATOMIC_ACQUIRE);
        if (st == JOB_IDLE || st == JOB_DONE) break;
        __asm__ volatile("yield");
    }
    if (job.state != JOB_IDLE && job.state != JOB_DONE)
        aga_diag_log("[AGA] RGB port: the housekeeper did not finish its job");
    __atomic_store_n(&job.state, JOB_IDLE, __ATOMIC_RELEASE);
}

/* Core 2, the housekeeper, every time round its loop (aga_audio_bus_service) */
void aga_rgbout_bus_service(void)
{
#ifdef PISTORM_CLASSIC
    int st = __atomic_load_n(&job.state, __ATOMIC_ACQUIRE);
    if (st == JOB_IDLE || st == JOB_DONE) return;
    job_step(JOB_WORDS, JOB_SCAN);
#endif
}

/* ---- 6. the ring: a picture that moved up or down (1.115) ---------------------------
   A pinball table or a vertical shooter moves every row of the picture when
   it scrolls: up to 27,000 words for a frame that cannot take 15,000
   (Pinball Fantasies AGA, Pi 3 A+, 2026-10-11). But an encoded row depends
   only on its own pixels and the palette - every line starts from colour
   0 - so a row that moved is a row the hidden buffer already holds, at
   another address. Each buffer is a ring (s_buf: the ring row its picture
   starts at); the new picture is matched against what the buffer holds at
   shifts of up to SCROLL_MAX rows either way, by a hash of each encoded
   row (all planes), and the shift that leaves the most rows where they
   already are wins - if it beats no shift by SCROLL_GAIN rows. The write
   then sends only what differs, word by word as always, so a hash that
   happens to agree costs time, never a wrong picture. Called with core 2
   idle (after job_wait). */
#define SCROLL_MAX  200                     /* rows either way: a buffer is two pictures behind, and a fast table moves far */
#define SCROLL_GAIN 8
static int      s_buf[2];                   /* each real buffer's ring start */
static int      scroll_ok;                  /* this session's lists have room for the wrap */
static uint64_t new_h[MAX_H];               /* the new picture's rows */
static uint64_t held_h[2][MAX_H];           /* each buffer's ring rows, as its last job left them */

static inline uint64_t enc_row_hash(const uint16_t *f, int y)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int p = 0; p < g_planes; p++) {
        const uint16_t *r = f + p * g_plane_words + y * g_words;
        for (int x = 0; x < g_words; x++) { h ^= r[x]; h *= 0xBF58476D1CE4E5B9ull; }
        h ^= h >> 29;
    }
    return h;
}

/* both buffers cleared (agaboot lends the block cleared): every ring row the hash of a black row */
static void held_init(void)
{
    uint64_t z = enc_row_hash(shadow[0], 0);        /* shadow is all 0 here */
    for (int b = 0; b < 2; b++)
        for (int y = 0; y < MAX_H; y++) held_h[b][y] = z;
}

/* The held rows' hashes are not read back from shadow: the job writes every
   word that differs, so once it is done buffer b's ring row (s + y) holds
   the new row y exactly - job_held records that at the post. Called before
   job_wait: the job in flight writes the OTHER buffer. */
static int choose_scroll(int b)
{
    const int H = g_h, s = s_buf[b];
    const uint64_t *held = held_h[b];
    for (int y = 0; y < H; y++) new_h[y] = enc_row_hash(planes_new, y);
    int m0 = 0;
    for (int y = 0, r = s; y < H; y++) {
        if (new_h[y] == held[r]) m0++;
        if (++r == H) r = 0;
    }
    if (m0 == H) return s;                          /* nothing moved */
    int best = 0, best_m = m0;
    for (int d = -SCROLL_MAX; d <= SCROLL_MAX; d++) {
        if (!d) continue;
        int r = (s + d) % H;
        if (r < 0) r += H;
        int m = 0;
        for (int y = 0; y < H; y++) {
            if (new_h[y] == held[r]) m++;
            if (++r == H) r = 0;
        }
        if (m > best_m) { best_m = m; best = d; }
    }
    if (best_m < m0 + SCROLL_GAIN) best = 0;
    int ns = (s + best) % H;
    return ns < 0 ? ns + H : ns;
}

/* at the post: what buffer b's ring will hold once the job is done */
static void job_held(int b, int s)
{
    for (int y = 0, r = s; y < g_h; y++) {
        held_h[b][r] = new_h[y];
        if (++r == g_h) r = 0;
    }
}

/* ---- the OS's own display in a WHDLoad game -----------------------------------------
   The sandbox is entered from S:WHDLoad-Startup and left from
   S:WHDLoad-Cleanup, which WHDLoad runs only after the game has quit and the
   OS is back. Before the takeover and after the hand-back the OS runs on the
   emulated chipset, and its display - a launcher's or Workbench's hires
   screen, a desktop's colours and the pointer, squeezed into lores HAM6 -
   went out on the RGB port at every quit (reported from AGS as a moment of
   garbage on the RGB monitor). That display is the one graphics.library
   runs from copinit: COP1LC points there whenever the OS's display is up -
   agaboot puts it there at entry, and it is there again once WHDLoad has
   given the OS its display back - while a game points it at a list of its
   own. agaboot hands copinit over before entry, and while COP1LC is copinit
   with the copper running the RGB port is black. WHDLoad game mode only:
   with System wide and agaboot RUN the OS's display IS the program.
   The address alone is not enough: WHDLoad gives the game the low chip RAM,
   where copinit lives, and a game's own list may sit at that very address.
   So the first word of each of copinit's first OS_SIG_N instructions - the
   register a MOVE writes, the position a WAIT waits for, never the data the
   OS rewrites - is kept at the start, and only a list that still begins
   with them is the OS's. WHDLoad puts the OS's chip RAM back before it gives
   the display back, so after a game they match again.
    While held with HDMI black ("RGB port only"), HDMI shows the emulated
    native display, as it would without the RGB port: on a native Workbench
    (a plain 3.x install) a WHDLoad requester waiting for a click (no
    Kickstart image, out of memory) is there. One on an RTG screen is seen
    only while the native display has no planes - my plane replaces the
    whole HVS list - and then no frame reaches core 1 at all, so nothing is
    held: the RGB port keeps the game's last picture until the leave.
   A safety net: a stretch longer than OS_HOLD_S is shown after all, counted
   from the takeover. In "RGB port only" it applies before the takeover too:
   a machine with no HDMI monitor has a native desktop, and a requester there
   must reach the RGB port after a while. A slave that boots a whole
   Kickstart (a kick31 BOOTDOS wrapper) runs a graphics.library of its own,
   and should its copinit land at the very address the real one has, with
   the same first instructions, that game would otherwise stay black. Such a
   game shows its quit too: its stretch began at the takeover and is long
   past the net when WHDLoad hands back. From the hand-back to the leave is
   only as long as WHDLoad takes to run S:WHDLoad-Cleanup's agaboot. A frame
   or two of another list in between (lists flipped every frame) does not
   start the count again; half a second of them does.
   The list is the one COP1LC holds when core 1 takes the frame, not the one
   the frame was drawn from: a switch with the copper left running can show
   one frame of the OS's display, or drop one of the game's. */
#define OS_HOLD_S 10
#define OS_SIG_N  8

static struct {
    uint32_t list;                  /* copinit, 0 = nothing held this session */
    uint16_t sig[OS_SIG_N];         /* the first word of each of its first instructions */
    int      cap;                   /* "RGB port only": the net applies before the takeover too */
    int      taken;                 /* WHDLoad's takeover, as last seen */
    uint64_t since, seen;           /* this stretch's first frame, its last */
    uint32_t other_sig;             /* frames with COP1LC at copinit but another list there */
    int      ever;                  /* held at least once this session */
} os;

static void os_begin(uint32_t list, int cap)
{
    memset(&os, 0, sizeof os);
    os.list = list;
    os.cap = cap;
    for (int i = 0; list && i < OS_SIG_N; i++) os.sig[i] = aga_glue_chip_word(list + 4 * (uint32_t)i);
}

static int os_sig_ok(void)
{
    for (int i = 0; i < OS_SIG_N; i++)
        if (aga_glue_chip_word(os.list + 4 * (uint32_t)i) != os.sig[i]) return 0;
    return 1;
}

/* core 1, per frame: 0 not the OS's display, 1 the OS's display - hold it
   back, 2 the OS's display past the safety net - show it */
static int os_display(void)
{
    uint64_t t = now();
    int taken = aga_glue_game_taken();
    if (taken != os.taken) { os.taken = taken; os.since = 0; }
    if (aga_glue_copper_list() != os.list) return 0;
    if (!os_sig_ok()) { os.other_sig++; return 0; }
    if (!os.since || t - os.seen > freq() / 2) os.since = t;
    os.seen = t;
    return (!(taken || os.cap) || t - os.since < (uint64_t)OS_HOLD_S * freq()) ? 1 : 2;
}

/* ---- a session: a game, or agaboot's Position frame ------------------------------ */
static void session(uint32_t gen, uint32_t block, int calib)
{
    /* both lists in one 512-byte block, so in one 64 KB page: a flip is one word */
    uint32_t lists = (block + 511) & ~511u;
    uint32_t list_at[2]  = { lists, lists + LIST_BYTES };
    uint32_t frame_at[2] = { lists + 2 * LIST_BYTES, lists + 2 * LIST_BYTES + FRAME_BYTES };
    const uint64_t f = freq();
    const uint64_t frame_ticks = f * (313u * 64u) / 1000000u;   /* one PAL frame */

    job_settle();                           /* core 2 out of the last session's job before the sizes change */
    /* this session's size, and its pace */
    int sz = rgb_size;
    if (sz < 0 || sz > 3) sz = 0;
    g_w = sizes[sz].w; g_h = sizes[sz].h; g_h0 = sizes[sz].h0; g_v0 = sizes[sz].v0;
    g_words = g_w / 16;
    g_plane_words = g_words * g_h;
    /* 32 colours or HAM6; agaboot's Position frame is always HAM6 */
    g_c32 = rgb_c32 && !calib;
    g_planes = g_c32 ? 5 : 6;
    g_ncol = g_c32 ? 32 : 16;
    g_frame_words = g_planes * g_plane_words;
    const int fps25 = rgb_fps25 && !calib;
    /* Frame rate Auto (1.115): a picture that misses the blank after the last
       one leaves that one up for two blanks, and a run of 50 and 25 reads as
       judder - players called it jerky. With four misses in the last 32
       pictures every picture is held for two blanks, a steady 25, until the
       work on each (snapshot, encode, bus) has fit in 7/8 of a frame for 62
       of the last 64 and two seconds have gone by. */
    const int autop = rgb_auto && !fps25 && !calib;
    int steady = 0;
    uint64_t miss_bits = 0, fit_bits = 0, steady_at = 0;
    os_begin((calib || !aga_game_mode) ? 0 : rgb_os_list & 0x1FFFFE, rgb_hdmi_off);

    /* agaboot allocated the block cleared: both frames are colour 0 all over */
    memset(shadow, 0, sizeof shadow);
    memset(list_img, 0, sizeof list_img);
    have_pal = 0;
    since_kmeans = 0;
    memset(pal, 0, sizeof pal);
    memset(enc_ok, 0, sizeof enc_ok);       /* planes_new is the last session's */
    src_key = 0;                            /* ...and tgt and hist: snapshot makes them all again */
    s_buf[0] = s_buf[1] = 0;
    scroll_ok = list_need() <= LIST_MAX_WORDS;
    g_const_in_list = list_need() + LIST_CONST_WORDS <= LIST_MAX_WORDS;
    held_init();
    for (int b = 0; b < 2; b++) {
        build_list(list_new, frame_at[b], 0, 0);
        if (!put_list(gen, list_at[b], b)) return;
    }
    /* list 0 at once: frame 0, black until the first picture */
    if (!put16(gen, 0xDFF080, (uint16_t)(lists >> 16)) || !put16(gen, 0xDFF082, (uint16_t)list_at[0])
        || !put16(gen, 0xDFF088, 0) || !put16(gen, 0xDFF096, 0x8380))   /* DMAEN | BPLEN | COPEN */
        return;
    if (!put_const(gen)) return;            /* after the switch: the OS's list no longer runs */
    aga_diag_set(AGA_D_RGB_STATE, calib ? 4 : 1);
    aga_diag_set(AGA_D_RGB_WIN_SIZE, ((uint32_t)g_w << 16) | (uint32_t)g_h);

    int front = 0;
    int dark = 1;                   /* the RGB port is black: no picture yet, or the OS's display held back */
    int held = 0, held_logs = 0;
    uint64_t flip_due = 0, sec_t0 = now(), drawn_at = 0;
    uint32_t last = rgb_seq, sec_frames = 0, off_seen = rgb_off_seq - 1;
    while (alive(gen)) {
        uint64_t t0, t1;
        if (calib) {
            /* redraw when the offsets change, and a few times a second so both frames get it */
            while (alive(gen) && rgb_off_seq == off_seen && now() - drawn_at < f / 8) aga_nap();
            if (!alive(gen)) break;
            off_seen = rgb_off_seq;
            drawn_at = now();
            t0 = now();
            pattern();
            memset(enc_ok, 0, sizeof enc_ok);   /* drawn without row hashes: every row again */
            t1 = now();
        } else {
            /* the newest presented frame */
            while (rgb_seq == last && alive(gen)) __asm__ volatile("wfe");
            if (!alive(gen)) break;
            uint32_t s = __atomic_load_n(&rgb_seq, __ATOMIC_ACQUIRE);
            const uint32_t *fr = rgb_frame;
            int pitch = rgb_pitch;
            int page = rgb_page;
            if (s - last > 1) aga_diag_ctr[AGA_D_RGB_SKIPPED] += s - last - 1;
            last = s;
            /* the OS's own display: not encoded. The hidden list goes dark and
               is flipped to - a few words, black from the next vertical
               blank - and stays up until the game's display is back. */
            int o = os.list ? os_display() : 0;
            if (o == 1) {
                if (!held) {
                    held = 1;
                    os.ever = 1;
                    __atomic_store_n(&rgb_os_held, gen, __ATOMIC_RELEASE);
                    aga_diag_set(AGA_D_RGB_STATE, 5);
                    if (held_logs++ < 4) aga_diag_log("[AGA] RGB port: the OS's own display - black until the game's");
                }
                if (!dark) {
                    /* the last picture's job done, its flip in */
                    if (!job_wait(gen)) break;
                    if (job.state == JOB_DONE) { flip_due = job.flip_due; job.state = JOB_IDLE; }
                    while (now() < flip_due && alive(gen)) aga_nap_before(flip_due);
                    int back = front ^ 1;
                    build_list(list_new, frame_at[back], s_buf[back], 1);
                    if (!put_list(gen, list_at[back], back) || !put16(gen, 0xDFF082, (uint16_t)list_at[back])) break;
                    int v0 = beam_line();
                    flip_due = now() + (uint64_t)(313 - v0) * 64 * f / 1000000 + f / 5000;
                    front = back;
                    dark = 1;
                }
                continue;
            }
            if (held) {
                held = 0;
                rgb_os_held = 0;
                aga_diag_set(AGA_D_RGB_STATE, 1);
                if (o == 2) aga_diag_log("[AGA] RGB port: the OS's own display shown after all (10 s safety net)");
                else if (held_logs++ < 4) aga_diag_log("[AGA] RGB port: the game's display again");
                sec_frames = 0;             /* the held seconds were no frames a second */
                sec_t0 = now();
            }
            t0 = now();
            if (!snapshot(gen, fr, pitch, page)) break;
            if (rgb_seq - s > 1) {                      /* drawn over under me */
                aga_diag_inc(AGA_D_RGB_TORN);
                src_key = 0;                            /* rows kept from it may be the next frame's: all again */
                continue;
            }
            t1 = now();
        }
        choose_palette();
        if (!encode(gen)) break;
        /* where the hidden buffer's ring should start for this picture -
           while core 2 still writes the other one */
        int back = front ^ 1;
        int ring = (scroll_ok && !calib) ? choose_scroll(back) : 0;
        uint64_t t2 = now();
        /* the last picture's job done (core 2's, on a classic PiStorm): its
           flip is in, and the buffer it left is the next one's */
        if (!job_wait(gen)) break;
        uint64_t t3 = now();
        if (job.state == JOB_DONE) {
            flip_due = job.flip_due;
            uint64_t wr = job.t_written - job.t_start;
            if (autop) {
                /* fits: on a classic PiStorm core 1's work and core 2's writes
                   run side by side, elsewhere one after the other */
                uint64_t lim = frame_ticks * 7 / 8;
                fit_bits = (fit_bits << 1) | (JOB_ON_CORE2 ? (t2 - t0 < lim && wr < lim) : (t2 - t0 + wr < lim));
                if (!steady) {
                    /* too late for the blank after the flip before it: that one stayed up two */
                    miss_bits = (miss_bits << 1) | (job.t_written > job.start_after + frame_ticks - f / 2500);
                    if (__builtin_popcountll(miss_bits & 0xFFFFFFFFull) >= 4) {
                        steady = 1;
                        steady_at = job.t_written;
                        aga_diag_set(AGA_D_RGB_STATE, 6);
                    }
                } else if (job.t_written - steady_at > 2 * f && __builtin_popcountll(fit_bits) >= 62) {
                    steady = 0;
                    miss_bits = 0;
                    aga_diag_set(AGA_D_RGB_STATE, 1);
                }
            }
            aga_diag_inc(AGA_D_RGB_FRAMES);
            aga_diag_set(AGA_D_RGB_WORDS_LAST, (uint32_t)job.words);
            aga_diag_ctr[AGA_D_RGB_KB_TOTAL] += (uint32_t)(job.words >> 9);
            aga_diag_set(AGA_D_RGB_WRITE_US, us(wr));
            sec_frames++;
            job.state = JOB_IDLE;
        }
        /* Fixed 25: every picture stays up for two vertical blanks, so motion
           moves in one steady step instead of 26 to 45 uneven ones a second.
           The swap goes in only after the blank that followed the last one.
           Auto does the same while it holds a steady 25. */
        s_buf[back] = ring;
        if (scroll_ok && !calib) job_held(back, ring);
        build_list(list_new, frame_at[back], s_buf[back], 0);
        job_post(gen, frame_at[back], list_at[back], back, s_buf[back], flip_due,
                 (fps25 || steady) ? flip_due + frame_ticks : 0);
        front = back;
        dark = 0;

        aga_diag_set(AGA_D_RGB_SNAP_US, us(t1 - t0));
        aga_diag_set(AGA_D_RGB_ENCODE_US, us(t2 - t1));
        aga_diag_set(AGA_D_RGB_WAIT_US, us(t3 - t2));
        aga_diag_set(AGA_D_RGB_WIN_POS, ((uint32_t)(uint16_t)rgb_off_x << 16) | (uint16_t)rgb_off_y);
        uint64_t el = now() - sec_t0;
        if (el >= f) {
            aga_diag_set(AGA_D_RGB_FPS10, (uint32_t)((uint64_t)sec_frames * 10 * f / el));
            sec_frames = 0;
            sec_t0 = now();
        }
    }
    job_settle();                   /* core 2 out of the shadows before core 1 is */
    /* a WHDLoad game in which the OS's display was never held back: say why */
    if (os.list && !os.ever)
        aga_diag_log(os.other_sig ? "[AGA] RGB port: COP1LC was at copinit, but not the OS's list - nothing held"
                   : "[AGA] RGB port: no frame of the OS's own display reached core 1 (no planes, or COP1LC never copinit)");
}

/* Core 1, from boot. Parked until a game starts with a block lent, or Position. */
void aga_rgbout_main(void)
{
    aga_evstream_on();              /* with aga.sleep, core 1's frame waits sleep (aga_glue.h) */
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
void aga_rgbout_set_block(uint32_t addr)
{
    if (aga_sandbox_active || rgb_run) return;
    if (addr && ((addr & 3) || addr < 0x1000 || addr + AGA_RGB_BLOCK_BYTES > 0x200000)) {
        kprintf("[AGA] RGB port: rejecting block at %08x\n", addr);
        aga_diag_set(AGA_D_RGB_STATE, 3);
        addr = 0;
    }
    rgb_block = addr;
}

/* $DFF1F8 (long), outside the sandbox: graphics.library's copinit, the list
   the OS's own display starts from (os_display). agaboot sends it before
   every entry; a kernel never told it shows the OS's display as before. */
void aga_rgbout_set_os_list(uint32_t addr)
{
    if (!aga_sandbox_active) rgb_os_list = addr;
}

/* $5950 / $5951, outside the sandbox: HDMI as usual, or black while the next game is on the RGB port */
void aga_rgbout_set_hdmi_off(int off)
{
    if (!aga_sandbox_active) rgb_hdmi_req = off;
}

/* $599S: the picture's size (sizes[]), and $59A1 / $59A0: a steady 25 frames a
   second or as many as get through. Both are taken when a game or the
   Position frame starts. */
void aga_rgbout_set_size(int s) { rgb_size = (s >= 0 && s <= 3) ? s : 0; }
void aga_rgbout_set_fps25(int on) { rgb_fps25 = on ? 1 : 0; }
void aga_rgbout_set_auto(int on) { rgb_auto = on ? 1 : 0; }
void aga_rgbout_set_c32(int on) { rgb_c32 = on ? 1 : 0; }

/* $54nn / $55nn, any time: the picture right / down by a signed byte (lores pixels, lines) */
void aga_rgbout_set_offset(int axis, int v)
{
    if (axis) rgb_off_y = v; else rgb_off_x = v;
    __atomic_add_fetch(&rgb_off_seq, 1, __ATOMIC_RELEASE);
}

/* core 3, per frame: black HDMI for this game? Only while the RGB port is
   really being fed, and not while it holds the OS's own display back. Core 1
   learns that the game's display is back only from a frame core 3 hands it,
   after this one is on HDMI: so the list running now is checked here too, or
   the game's first frame would reach an HDMI meant to stay black. The flag
   carries the session that set it: a core 1 slow to stop cannot leave it
   set for the next one. */
int aga_rgbout_hdmi_off(void)
{
    if (!rgb_hdmi_off || !rgb_run || rgb_calib) return 0;
    uint32_t h = __atomic_load_n(&rgb_os_held, __ATOMIC_ACQUIRE);
    return !(h && h == rgb_gen && aga_glue_copper_list() == os.list && os_sig_ok());
}

static void go(void)
{
    rgb_os_held = 0;
    rgb_gen++;
    __asm__ volatile("dmb ish" ::: "memory");
    rgb_run = 1;
    __asm__ volatile("dmb ish; sev" ::: "memory");
}

/* Sandbox entry, with the chipset loop confirmed running */
void aga_rgbout_start(uint32_t chip_real)
{
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
    if (b + AGA_RGB_BLOCK_BYTES > chip_real) {        /* lent from memory agaboot added, not real chip RAM */
        aga_diag_set(AGA_D_RGB_STATE, 3);
        aga_diag_log("[AGA] RGB port: block beyond the real chip RAM - no HAM6");
        return;
    }
    rgb_calib = 0;
    rgb_hdmi_off = rgb_hdmi_req;
    aga_diag_set(AGA_D_RGB_HDMI_OFF, (uint32_t)rgb_hdmi_off);
    go();
    aga_diag_log(rgb_hdmi_off ? "[AGA] RGB port: HAM6 on, HDMI black" : "[AGA] RGB port: HAM6 on");
}

/* $5953 / $5952, outside the sandbox: agaboot's Position frame on the RGB port,
   on the block it has just lent, or off again. agaboot puts the desktop's own
   display back afterwards (its copper list and DMA, then RethinkDisplay). */
void aga_rgbout_calibrate(int on)
{
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

/* Sandbox leave, BEFORE chip RAM is copied back: that copy goes over my
   lists, and a copper still running them would execute whatever lands there.
   Also the end of a Position frame. */
void aga_rgbout_stop(void)
{
    int was = rgb_run;
    rgb_run = 0;
    rgb_gen++;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    if (was) {
        uint64_t deadline = now() + freq() / 20;      /* 50 ms; it answers within a batch */
        while (rgb_busy && now() < deadline) __asm__ volatile("sev; yield");
        if (rgb_busy) aga_diag_log("[AGA] RGB port: core 1 slow to stop");
        ps_write_16(0xDFF096, 0x0180);                /* the real copper and planes off */
        aga_diag_set(AGA_D_RGB_STATE, 0);
        aga_diag_log(rgb_calib ? "[AGA] RGB port: Position frame off" : "[AGA] RGB port: HAM6 off");
    }
    rgb_calib = 0;
    rgb_hdmi_off = 0;
    rgb_os_held = 0;
    aga_diag_set(AGA_D_RGB_HDMI_OFF, 0);
    rgb_block = 0;                                    /* agaboot frees it */
}

/* Core 3, for every frame it presents. A woven frame's own field is every
   other row, and stays put while core 3 draws the next field between them. */
void aga_rgbout_frame(const uint32_t *frame, int pitch, int page)
{
    if (!rgb_run || rgb_calib) return;
    rgb_frame = frame;
    rgb_pitch = pitch;
    rgb_page = page;
    __atomic_store_n(&rgb_seq, rgb_seq + 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
}
