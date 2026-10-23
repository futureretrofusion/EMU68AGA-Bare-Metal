/* agaboot - Chipset menu - Sound - Monitor: what HDMI actually fucking plays,
 * caught red-handed - level meters, a scope and a spectrum of the very
 * samples the screen's speakers get, and the state of HDMI and HiFi sound -
 * with the HiFi volume (AHI programs' sound, which the Sound window's HDMI
 * volume leaves the hell alone) and test sounds to check the speakers by.
 *
 * The samples come from the kernel (arm/aga_hdmi.c, "the Sound monitor"): the
 * window lends it a ring in fast RAM - a header, then frames of two 16-bit
 * samples - and core 3 writes into it what it writes into HDMI's own ring,
 * after the volumes, the filters and the AHI mix. The window reads it at its
 * own pace, so it costs the sound not a fucking thing. Words at $DFF1F2,
 * bits 1 and 15 clear like every word of mine:
 *   $5140 + E4(n)  four bits of the ring's address, highest first
 *   $5161 / $5160  the ring from that address / none (returns once the kernel
 *                  no longer writes it, so it may be freed at once)
 *   $5180 + E6(d)  HiFi volume, d dB down, 63 silence (sound.c sends it)
 * with E4(n) = n & 1 | (n & 14) << 1. The window counts the header's beat up
 * at every tick, paused or not; a kernel that sees it stand for 3 s lets the
 * ring go (a window that died, memory a soft reboot handed on) and says so
 * with state 0 - and a window that was only held up lends the bugger again.
 *
 * Test sounds go out through ahi.device's unit 0 at 48 kHz: wherever AHI
 * plays that unit - on HDMI with HiFi sound on.
 *
 * The header (256 bytes) also carries the limiter's work and the effects as
 * the kernel has them (arm/aga_fx.h), the screen's ELD - its name, the
 * formats it decodes, its speakers - which Linux shows the same way, and
 * Paula's voices: a second ring after the frames with what each of the four
 * plays, and their registers. No HDMI sound anywhere else has those - it
 * only ever gets the mix, and it can bloody well lump it.
 *
 * What it shows, all at once on a screen with room (View: All) or one at a
 * time: two VU meters as the old ones look (300 ms; 0 VU at -8 dBFS by
 * default - Amiga music is loud - and a click on a meter moves it), a
 * vectorscope with the phase between left and right, the scope, Paula's
 * four voices each with its own scope, note, volume and sample length - a
 * click on one mutes it, the right button hears it alone - and always the
 * level meters and the spectrum. Record writes what HDMI plays to a WAV
 * file, and with a game in the sandbox each voice to a file of its own
 * (rec.c). Equalizer... (eq.c) and Voices... (vm.c, where each voice sits
 * and how loud) open beside this window, all served here, so the spectrum
 * and the scopes show what each fader does.
 *
 * Written for AGA-PISTORM from the AmigaOS and AHI documentation, and don't
 * you forget it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <devices/ahi.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sound.h"
#include "monitor.h"
#include "eq.h"
#include "vm.h"
#include "rec.h"

#define AGA_CTRL   (*(volatile UWORD *)0xDFF1F2)
#define DIAG_SEL_W (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT_W (*(volatile UWORD *)0xDFF1EA)

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c */
ULONG diag_read(UWORD sel);                        /* agaboot.c */
void  kernel_boards(int *built, int *runs);        /* agaboot.c */

/* the ring's header - arm/aga_hdmi.c's struct mon_hdr, word for word */
#define MON_MAGIC   0x4D4F4E31UL                   /* "MON1" */
#define MON_TAKEN   0x80000000UL                   /* the kernel took the ring */
#define MON_HDMI    1UL                            /* HDMI sound plays */
#define MON_AHI     2UL                            /* an AHI ring is on */
#define MON_AHIPLAY 4UL                            /* ... and the feed plays it now */
#define MON_AHI32   8UL                            /* ... 32-bit frames (HiFi) */
#define MON_LOWLAT  16UL                           /* ... low latency */
#define MON_OUT24   32UL                           /* the screen gets 24 bits */
#define MON_PAULABL 64UL                           /* Paula on HDMI band-limited */
#define MON_PAULA   128UL                          /* the emulated chipset's sound in it */
/* 256 and 512 are RGB2RTG's (MON_TONE, MON_LEDF - its monitor shares this
   header, EMM #2188): keep them free here */
struct mon_eld { char name[14]; UBYTE nsad, flags, sad[30], spk[3]; };   /* arm/aga_edid.h's struct edid_eld */
#define ELD_READ  1
#define ELD_BASIC 2
struct mon_voice { ULONG lc; UWORD len, per; UBYTE vol, flags, pan, level; };   /* flags: 1 DMA, 2 plays, 4 heard, 8 mixed */
struct mon_hdr {
    ULONG magic, frames, wr, rate, peak_l, peak_r, state, lead, resyncs, ahi_fill, ahi_under, offer, hifi_db, beat;
    ULONG lim;                                     /* the limiter's least gain since I last fucking looked, 65536 none */
    ULONG fx;                                      /* the effects: on, limiter, crossfeed 2, preamp+12 5, balance+12 5, bands 4 */
    struct mon_eld eld;
    UBYTE pad[64 - sizeof(struct mon_eld)];
    ULONG vframes;                                 /* mine: the voice ring's frames (0 none) */
    ULONG vwr, vrate, vstate;                      /* the kernel's: its count, rate, 1 voices come, 2 the mixer on */
    struct mon_voice v[4];
    UBYTE pad2[64];
};
/* as arm/aga_hdmi.c lays it out: the same bytes on both sides */
typedef char mon_hdr_is_256[sizeof(struct mon_hdr) == 256 && sizeof(struct mon_voice) == 12 &&
                            __builtin_offsetof(struct mon_hdr, vframes) == 128 ? 1 : -1];

#define RING_FRAMES  65536                         /* 0.34 s at 192 kHz, 1.4 s at 48: the 16384 looked at, and the recorder's room */
#define VRING_FRAMES 32768                         /* 1.2 s of Paula's voices */
#define VHIST        2048                          /* the voice words looked at: 73 ms */
#define FFT_LOG      12
#define FFT_N        (1 << FFT_LOG)                /* 4096: 11.7 Hz a bin at 48 kHz */
#define HIST_MAX     (FFT_N * 4)                   /* 192 kHz looks at every 4th frame's worth */
#define N_BANDS      48
#define FLOOR        (-600)                        /* -60.0 dB, the meters' and spectrum's bottom */
#define D_HDMI_STATE 490                           /* aga_diag.c pins it */
#define D_KVER       0x1803                        /* SEL_KVER: major * 1000 + minor */
#define MON_PORT     "AGA-PISTORM Sound monitor"

/* the spectrum's bands: 25 Hz to 20 kHz, 48 of them, each 1/16.5 decade */
static const UWORD band_hz[N_BANDS + 1] = {
    25, 29, 33, 38, 44, 50, 58, 66, 76, 88, 101, 116, 133, 153, 176, 202, 232, 267, 307, 352, 405, 466, 535, 615, 707,
    813, 934, 1074, 1234, 1419, 1631, 1874, 2154, 2476, 2846, 3272, 3761, 4323, 4968, 5711, 6564, 7545, 8672, 9968,
    11458, 13170, 15138, 17400, 20000 };

enum { P_BG, P_GRID, P_TEXT, P_DIM, P_GREEN, P_YELLOW, P_RED, P_LEFT, P_RIGHT, P_SPEC, P_PEAK, P_FACE, P_INK, N_PENS };
static const ULONG pen_rgb[N_PENS] = { 0x12161C, 0x323C48, 0xDCE4EC, 0x8A9AAA, 0x34C058, 0xE8C434, 0xE84C34,
                                       0x48D4EC, 0xF49C3C, 0x4480F0, 0xF4F4F4, 0xE8DDB5, 0x1E1E1E };

/* ---- the test sounds ---------------------------------------------------------- */
enum { T_LEFT, T_RIGHT, T_BOTH, T_SEQ, T_SWEEP, T_PINK };
static const char *const test_names[] = { "Left", "Right", "Both", "L, R, both", "Sweep", "Pink noise", NULL };
static const char *const level_names[] = { "-30 dBFS", "-20 dBFS", "-12 dBFS", "-6 dBFS", NULL };
static const WORD level_gain[4] = { 1036, 3277, 8231, 16422 };   /* 32767 * 10^(dB / 20) */
#define T_RATE   48000
#define T_FRAMES 2400                              /* 50 ms a buffer, two of them */
#define INC_1K   89478485UL                        /* 1 kHz at 48 kHz, 2^32 a turn */
#define INC_20   1789570UL                         /* 20 Hz */
#define INC_20K  1789569707UL                      /* 20 kHz */
#define SWEEP_R  1075226278UL                      /* 1000^(1 / 5000) in 2.30: a millisecond's step, three decades in 5 s */

struct test {
    struct MsgPort *port;
    struct AHIRequest *io[2];
    WORD *buf[2];
    int open, busy[2], err;                        /* err: 1 no unit 0, 2 no memory, 3 AHI said no */
    int kind, level;
    ULONG ph, inc, sub, seg_n, seg, n;             /* phase, step, a ms's frames, left-right-both */
    LONG gl, gr;                                   /* the channels' gains now, ramping to their aim (1.15) */
    ULONG rng, cnt;                                /* pink noise (Voss): its rows */
    LONG rows[16], rsum;
};

/* ---- the window's own ------------------------------------------------------- */
enum { V_ALL, V_SCOPE, V_VOICES, V_VU, V_VECTOR };
static const char *const view_names[] = { "All", "Scope", "Voices", "VU meters", "Vectorscope", NULL };

struct lay {
    int dw, dh, ox, oy, lh, lw;                    /* the drawn area, where it sits, a line, the labels' width */
    int nst;                                       /* status lines: 6, or 3 on a low screen */
    int me_y, me_h, me_x0, me_x1;                  /* meters: top, one bar, its left and right */
    int bg_y, bg_h;                                /* the view: scope, voices, VU meters, vectorscope - or all */
    int sp_y, sp_h, sp_x0;                         /* spectrum */
};

struct mon {
    struct mon_hdr *h;
    int taken, playing, built, view;
    ULONG last_wr, rate, state, stale_ms, relend_ms, kver;
    const char *why;                               /* the ring not taken: why */
    int hdmi_state;
    /* what was seen */
    int pk_hold[2], pk_t[2], rms[2], clip_ms[2];
    int lim_db, lim_ms;                            /* the limiter: most it took (tenths of dB), how long to show it */
    ULONG fx, fx_seen;                             /* the kernel's effects word, and the one eq_name is for */
    char eq_name[24];
    int band[N_BANDS], bpk[N_BANDS], bpk_t[N_BANDS];
    UWORD band_lo[N_BANDS], band_hi[N_BANDS];
    ULONG band_rate;
    int sc_t, sc_n, sc_ok;                         /* the scope's trigger frame and span */
    ULONG need;                                    /* frames in hist */
    LONG vu_ms[2];                                 /* VU: the mean square over ~300 ms, its own ballistics */
    int vu_led[2];                                 /* ms the VU's peak lamp stays lit */
    int vu_ref;                                    /* 0 VU at vu_ref_db[vu_ref] dBFS (a click on a meter steps it) */
    int vu_x[2], vu_y[2], vu_w[2], vu_h[2];        /* where the meters are drawn (for clicks); vu_w 0 none */
    int corr, corr_ok;                             /* left against right, -1000..1000 */
    int deep;                                      /* an RTG screen: more dots for the vectorscope */
    /* Paula's voices */
    ULONG vlast;                                   /* the voice ring's count last seen */
    int vn, vspan, vtrig[4];                       /* words in vhist, a voice scope's span, each one's start */
    int vp_x, vp_y, vp_w, vp_h;                    /* where the voice scopes are (for clicks); vp_w 0 none */
    struct vm_set vm;                              /* the voice mixer, with the clicks' mutes and solo */
    /* the recorder */
    struct rec rec;
    char rec_msg[256];
    ULONG hist[HIST_MAX];                          /* the newest frames, oldest first: L high word */
    ULONG vhist[VHIST];                            /* the newest voice words, oldest first */
    WORD re[FFT_N], im[FFT_N];
    WORD win[FFT_N], cs[FFT_N / 2], sn[FFT_N / 2];
    WORD qsin[1025];                               /* a quarter turn of sine, 1.15 */
    struct test t;
};

/* ---- numbers ------------------------------------------------------------------ */
/* sine of i/4096 of a turn, 1.15 */
static LONG qs(const WORD *q, int i)
{
    i &= 4095;
    if (i < 1024) return q[i];
    if (i < 2048) return q[2048 - i];
    if (i < 3072) return -q[i - 2048];
    return -q[4096 - i];
}

/* a quarter turn of sine in 256 steps, 32767 sin(pi/2 i/256); the 1024-step
   table is drawn between them (0.15 of a step off at most) - no floating
   point, which the build has no library for */
static const WORD q256[257] = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210, 2410, 2611, 2811, 3012,
    3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609, 4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195,
    6393, 6590, 6786, 6983, 7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
    9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353,
    12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828, 14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
    15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631,
    20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856, 22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027,
    23170, 23311, 23452, 23592, 23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
    27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001, 28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803,
    28898, 28992, 29085, 29177, 29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297,
    31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736, 31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098,
    32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752, 32757, 32761, 32765, 32766,
    32767
};

static void tables(struct mon *m)
{
    for (int i = 0; i <= 1024; i++) {
        int k = i >> 2, f = i & 3;
        m->qsin[i] = (WORD)(f ? q256[k] + ((q256[k + 1] - q256[k]) * f + 2) / 4 : q256[k]);
    }
    for (int i = 0; i < FFT_N / 2; i++) {
        m->cs[i] = (WORD)qs(m->qsin, i + 1024);
        m->sn[i] = (WORD)qs(m->qsin, i);
    }
    for (int i = 0; i < FFT_N; i++) m->win[i] = (WORD)((32767 - qs(m->qsin, i + 1024)) >> 1);   /* Hann */
}

/* log2 x in 1/256ths */
static LONG lg2q8(ULONG x)
{
    if (!x) return -64 * 256;
    LONG e = 31;
    while (!(x & 0x80000000UL)) { x <<= 1; e--; }
    ULONG f = x >> 16;                             /* 1.15: 1 <= f < 2 */
    for (int i = 0; i < 8; i++) {
        f = (f * f) >> 15;
        e <<= 1;
        if (f >= 65536) { f >>= 1; e |= 1; }
    }
    return e;
}

/* dB in tenths against 2^ref: of a power, of an amplitude - FLOOR at least */
static int db_pow(ULONG p, int ref)
{
    LONG v = (lg2q8(p) - ref * 256) * 301 / 2560;   /* 10 log10 2 = 3.01 dB an octave, in tenths */
    return v < FLOOR ? FLOOR : (int)v;
}
static int db_amp(ULONG a, int ref)
{
    LONG v = (lg2q8(a) - ref * 256) * 602 / 2560;
    return v < FLOOR ? FLOOR : (int)v;
}

/* In place, radix 2, block floating point: a stage is halved only when it
   could overflow, so quiet sound keeps its bits. Every value stays within a
   WORD (the magnitudes never pass 32767), so the products are the 68000's
   own 16 x 16 multiply. Returns the halvings. */
static int fft(WORD *re, WORD *im, const WORD *cs, const WORD *sn)
{
    int shifts = 0;
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            WORD t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2, step = FFT_N / 2; len <= FFT_N; len <<= 1, step >>= 1) {
        int big = 0;
        for (int i = 0; i < FFT_N; i++) {
            WORD a = re[i], b = im[i];
            if (a > big) big = a; else if (-a > big) big = -a;
            if (b > big) big = b; else if (-b > big) big = -b;
        }
        int sh = big >= 5632;                      /* 2 x sqrt 2 x 5632 < 32768 */
        shifts += sh;
        int half = len >> 1;
        for (int i = 0; i < FFT_N; i += len) {
            for (int k = 0; k < half; k++) {
                WORD c = cs[k * step], s = sn[k * step];
                WORD br = re[i + k + half], bi = im[i + k + half];
                LONG tr = ((LONG)br * c + (LONG)bi * s) >> 15;     /* b e^-i2pi k/len */
                LONG ti = ((LONG)bi * c - (LONG)br * s) >> 15;
                LONG ar = re[i + k], ai = im[i + k];
                if (sh) {
                    re[i + k] = (WORD)((ar + tr) >> 1); im[i + k] = (WORD)((ai + ti) >> 1);
                    re[i + k + half] = (WORD)((ar - tr) >> 1); im[i + k + half] = (WORD)((ai - ti) >> 1);
                } else {
                    re[i + k] = (WORD)(ar + tr); im[i + k] = (WORD)(ai + ti);
                    re[i + k + half] = (WORD)(ar - tr); im[i + k + half] = (WORD)(ai - ti);
                }
            }
        }
    }
    return shifts;
}

/* ---- the test sounds ------------------------------------------------------------ */
static LONG sine(const WORD *q, ULONG ph)
{
    int i = (int)(ph >> 20);                       /* 4096 steps a turn, the rest between them */
    LONG f = (LONG)((ph >> 4) & 0xFFFF);
    LONG a = qs(q, i), b = qs(q, i + 1);
    return a + (((b - a) * f) >> 16);
}

static LONG rnd16(struct test *t)
{
    ULONG x = t->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    t->rng = x;
    return (LONG)(x >> 16) - 32768;
}

/* Voss's pink noise: 16 rows, row k new every 2^(k+1) frames, plus white -
   at the rms of the level's sine */
static LONG pink(struct test *t, LONG g)
{
    ULONG c = ++t->cnt;
    int k = 0;
    while (k < 15 && !(c & 1)) { c >>= 1; k++; }
    t->rsum -= t->rows[k];
    t->rows[k] = rnd16(t);
    t->rsum += t->rows[k];
    LONG s = t->rsum + rnd16(t);                   /* 17 of +-32768: rms 78000 */
    LONG gq = g + ((g * 6161) >> 15);              /* g x 1.188 */
    s = ((s >> 4) * gq) >> 13;
    return s > 32767 ? 32767 : s < -32767 ? -32767 : s;
}

static void test_reset(struct test *t)
{
    t->ph = 0;
    t->inc = t->kind == T_SWEEP ? INC_20 : INC_1K;
    t->sub = t->seg_n = t->seg = 0;
    if (!t->rng) t->rng = 0x2545F491UL;
}

static void test_fill(struct test *t, const WORD *q, WORD *out, int frames)
{
    LONG g = level_gain[t->level];
    for (int i = 0; i < frames; i++) {
        LONG al = 32767, ar = 32767;               /* where the channels' gains go */
        if (t->kind == T_LEFT) ar = 0;
        else if (t->kind == T_RIGHT) al = 0;
        else if (t->kind == T_SEQ) {
            if (++t->seg_n >= T_RATE) { t->seg_n = 0; t->seg = t->seg == 2 ? 0 : t->seg + 1; }
            if (t->seg == 0) ar = 0; else if (t->seg == 1) al = 0;
        }
        /* 5 ms ramps: no click as a channel comes or goes */
        if (t->gl < al) { t->gl += 137; if (t->gl > al) t->gl = al; }
        else if (t->gl > al) { t->gl -= 137; if (t->gl < al) t->gl = al; }
        if (t->gr < ar) { t->gr += 137; if (t->gr > ar) t->gr = ar; }
        else if (t->gr > ar) { t->gr -= 137; if (t->gr < ar) t->gr = ar; }
        LONG s;
        if (t->kind == T_PINK) s = pink(t, g);
        else {
            s = (sine(q, t->ph) * g) >> 15;
            t->ph += t->inc;
            if (t->kind == T_SWEEP && ++t->sub >= T_RATE / 1000) {
                t->sub = 0;
                t->inc = (ULONG)(((unsigned long long)t->inc * SWEEP_R) >> 30);
                if (t->inc >= INC_20K) t->inc = INC_20;
            }
        }
        out[2 * i] = (WORD)((s * t->gl) >> 15);
        out[2 * i + 1] = (WORD)((s * t->gr) >> 15);
    }
}

static void test_send(struct test *t, int i, const WORD *q)
{
    struct AHIRequest *io = t->io[i];
    test_fill(t, q, t->buf[i], T_FRAMES);
    io->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
    io->ahir_Std.io_Command = CMD_WRITE;
    io->ahir_Std.io_Data = t->buf[i];
    io->ahir_Std.io_Length = T_FRAMES * 4;
    io->ahir_Std.io_Offset = 0;
    io->ahir_Frequency = T_RATE;
    io->ahir_Type = AHIST_S16S;
    io->ahir_Volume = 0x10000;
    io->ahir_Position = 0x8000;
    io->ahir_Link = t->busy[i ^ 1] ? t->io[i ^ 1] : NULL;   /* straight after the other: no gap */
    SendIO((struct IORequest *)io);
    t->busy[i] = 1;
}

static void test_stop(struct test *t)
{
    for (int i = 1; i >= 0; i--)
        if (t->busy[i]) {
            AbortIO((struct IORequest *)t->io[i]);
            WaitIO((struct IORequest *)t->io[i]);
            t->busy[i] = 0;
        }
    if (t->open) CloseDevice((struct IORequest *)t->io[0]);
    t->open = 0;
    if (t->io[1]) FreeVec(t->io[1]);
    if (t->io[0]) DeleteIORequest((struct IORequest *)t->io[0]);
    t->io[0] = t->io[1] = NULL;
    for (int i = 0; i < 2; i++) { if (t->buf[i]) FreeVec(t->buf[i]); t->buf[i] = NULL; }
    if (t->port) DeleteMsgPort(t->port);
    t->port = NULL;
}

static int test_start(struct test *t, const WORD *q)
{
    if (t->open) return 1;
    t->err = 2;
    if (!(t->port = CreateMsgPort())) goto fail;
    if (!(t->io[0] = (struct AHIRequest *)CreateIORequest(t->port, sizeof(struct AHIRequest)))) goto fail;
    t->io[0]->ahir_Version = 4;
    if (OpenDevice((STRPTR)AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)t->io[0], 0)) { t->err = 1; goto fail; }
    t->open = 1;
    if (!(t->io[1] = AllocVec(sizeof(struct AHIRequest), MEMF_PUBLIC))) goto fail;
    CopyMem(t->io[0], t->io[1], sizeof(struct AHIRequest));
    for (int i = 0; i < 2; i++)
        if (!(t->buf[i] = AllocVec(T_FRAMES * 4, MEMF_PUBLIC))) goto fail;
    t->err = 0;
    t->gl = t->gr = 0;
    test_reset(t);
    test_send(t, 0, q);
    test_send(t, 1, q);
    return 1;
fail:
    test_stop(t);
    return 0;
}

/* a buffer played: the next one after the other */
static void test_service(struct test *t, const WORD *q)
{
    struct Message *msg;
    int bad = 0;
    while (t->port && (msg = GetMsg(t->port))) {
        int i = msg == (struct Message *)t->io[0] ? 0 : 1;
        t->busy[i] = 0;
        if (t->io[i]->ahir_Std.io_Error) bad = 1;
        else if (!bad) test_send(t, i, q);
    }
    if (bad) { test_stop(t); t->err = 3; }
}

/* ---- the ring ------------------------------------------------------------------ */
static void ring_lend(struct mon_hdr *h)
{
    ULONG a = (ULONG)h;
    for (int i = 7; i >= 0; i--) {
        int n = (int)((a >> (4 * i)) & 15);
        AGA_CTRL = (UWORD)(0x5140 | (n & 1) | ((n & 14) << 1));
    }
    AGA_CTRL = 0x5161;
}

static ULONG kernel_version(void)
{
    Disable();
    DIAG_SEL_W = D_KVER;
    ULONG hi = DIAG_DAT_W;
    ULONG lo = DIAG_DAT_W;
    Enable();
    return (hi << 16) | lo;
}

static void set_why(struct mon *m)
{
    int built, runs;
    kernel_boards(&built, &runs);
    m->built = built;
    if (!built) m->why = "the AGA-PISTORM kernel is not running";
    else if (kernel_version() < 1118) m->why = "the kernel is older than the monitor - install the current one, power cycle";
    else m->why = "the kernel refused the ring - C:AGA-diag.log says why";
}

/* the newest frames into hist: enough for the spectrum at this rate */
static void ring_read(struct mon *m, ULONG wr, ULONG need)
{
    const ULONG *ring = (const ULONG *)(m->h + 1);
    ULONG have = wr < need ? wr : need;
    if (have < need) memset(m->hist, 0, (need - have) * 4);
    ULONG s = (wr - have) & (RING_FRAMES - 1), first = RING_FRAMES - s;
    if (first > have) first = have;
    CopyMem((APTR)(ring + s), m->hist + (need - have), first * 4);
    if (have > first) CopyMem((APTR)ring, m->hist + (need - have) + first, (have - first) * 4);
    m->need = need;
}

/* ---- what is seen, at every tick ----------------------------------------------- */
static void fall(int *v, int to, int per_s, int dt)
{
    if (to >= *v) *v = to;
    else {
        *v -= per_s * dt / 1000;
        if (*v < to) *v = to;
    }
}

static unsigned long long isqrt64(unsigned long long x)
{
    unsigned long long r = 0, b = 1ULL << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* the newest of Paula's voices, and where each voice scope starts: a rising
   zero crossing, as recent as there is */
static void voices_read(struct mon *m)
{
    struct mon_hdr *h = m->h;
    ULONG vw = h->vwr;
    if (!(h->vstate & 1) || !h->vrate || !vw) { m->vn = 0; m->vlast = vw; return; }
    m->vlast = vw;
    ULONG have = vw < VHIST ? vw : VHIST;
    const ULONG *vr = (const ULONG *)((const UBYTE *)(h + 1) + RING_FRAMES * 4);
    ULONG s = (vw - have) & (VRING_FRAMES - 1), first = VRING_FRAMES - s;
    if (first > have) first = have;
    CopyMem((APTR)(vr + s), m->vhist, first * 4);
    if (have > first) CopyMem((APTR)vr, m->vhist + first, (have - first) * 4);
    m->vn = (int)have;
    m->vspan = (int)(h->vrate / 25);               /* 40 ms */
    if (m->vspan > m->vn / 2) m->vspan = m->vn / 2;
    if (m->vspan < 8) { m->vn = 0; return; }
    for (int n = 0; n < 4; n++) {
        int sh = 24 - 8 * n, hi = m->vn - m->vspan, lo = m->vn - 2 * m->vspan, t = hi;
        if (lo < 1) lo = 1;
        for (int i = hi; i > lo; i--)
            if ((BYTE)(m->vhist[i - 1] >> sh) < 0 && (BYTE)(m->vhist[i] >> sh) >= 0) { t = i; break; }
        m->vtrig[n] = t;
    }
}

static void analyse(struct mon *m, int dt)
{
    struct mon_hdr *h = m->h;
    ULONG pk[2] = { h->peak_l, h->peak_r }, lim = h->lim;
    h->peak_l = h->peak_r = 0;                     /* the kernel's max since I last looked */
    h->lim = 65536;
    m->fx = h->fx;
    if (lim && lim < 65000) {                      /* the limiter took something off */
        int d = -db_amp(lim, 16);
        if (d > m->lim_db || m->lim_ms <= 0) m->lim_db = d;
        m->lim_ms = 1500;
    } else if (m->lim_ms > 0) m->lim_ms -= dt;
    ULONG wr = h->wr, rate = h->rate;
    m->state = h->state;
    if (wr == m->last_wr) m->stale_ms += dt;
    else m->stale_ms = 0;
    if (wr < m->last_wr) m->stale_ms = 0;          /* taken anew: counts from 0 */
    m->last_wr = wr;
    m->playing = m->taken && m->stale_ms < 300 && rate >= 32000 && wr;
    m->rate = rate;
    int k300 = dt > 300 ? 300 : dt;
    if (!m->playing) {
        m->hdmi_state = m->taken ? (int)diag_read(D_HDMI_STATE) : -1;
        for (int c = 0; c < 2; c++) {
            fall(&m->rms[c], FLOOR, 240, dt);
            fall(&m->pk_hold[c], FLOOR, 200, dt);
            if (m->clip_ms[c] > 0) m->clip_ms[c] -= dt;
            m->vu_ms[c] -= (LONG)(((long long)m->vu_ms[c] * k300) / 300);
            if (m->vu_led[c] > 0) m->vu_led[c] -= dt;
        }
        for (int b = 0; b < N_BANDS; b++) { fall(&m->band[b], FLOOR, 400, dt); fall(&m->bpk[b], FLOOR, 120, dt); }
        m->sc_ok = 0;
        m->corr_ok = 0;
        m->vn = 0;
        return;
    }
    int d = rate > 96000 ? 4 : rate > 48000 ? 2 : 1, dsh = d == 4 ? 3 : d == 2 ? 2 : 1;
    ULONG need = (ULONG)FFT_N * d;
    ring_read(m, wr, need);
    voices_read(m);

    /* the meters: peaks from the kernel (every frame), rms over 50 ms; the
       VU meters from the same, over 300 ms; the phase from left times right */
    ULONG nr = rate / 20;
    if (nr > need) nr = need;
    unsigned long long sq[2] = { 0, 0 };
    long long slr = 0;
    for (ULONG i = need - nr; i < need; i++) {
        WORD l = (WORD)(m->hist[i] >> 16), r = (WORD)m->hist[i];
        sq[0] += (ULONG)((LONG)l * l);
        sq[1] += (ULONG)((LONG)r * r);
        slr += (LONG)l * r;
    }
    for (int c = 0; c < 2; c++) {
        LONG ms = (LONG)(sq[c] / nr);
        fall(&m->rms[c], db_pow((ULONG)ms, 30), 240, dt);
        m->vu_ms[c] += (LONG)(((long long)(ms - m->vu_ms[c]) * k300) / 300);
        int p = db_amp(pk[c], 23);                 /* 24 bits: 2^23 full scale */
        if (p >= m->pk_hold[c]) { m->pk_hold[c] = p; m->pk_t[c] = 0; }
        else if ((m->pk_t[c] += dt) > 1500) fall(&m->pk_hold[c], p, 200, dt);
        if (pk[c] >= 0x7FFF00UL) m->clip_ms[c] = 2000;   /* full scale: 16-bit 32767 or the 24-bit clamp */
        else if (m->clip_ms[c] > 0) m->clip_ms[c] -= dt;
        if (pk[c] >= 5938679UL) m->vu_led[c] = 600;      /* -3 dBFS: the VU's peak lamp */
        else if (m->vu_led[c] > 0) m->vu_led[c] -= dt;
    }
    unsigned long long ra = isqrt64(sq[0]), rb = isqrt64(sq[1]);
    m->corr_ok = ra > 64 && rb > 64;
    if (m->corr_ok) {
        long long c = slr * 1000 / (long long)(ra * rb);
        m->corr = c > 1000 ? 1000 : c < -1000 ? -1000 : (int)c;
    }

    /* the spectrum: left and right together, d frames to one, Hann */
    for (int j = 0; j < FFT_N; j++) {
        const ULONG *p = m->hist + j * d;
        LONG a = 0;
        for (int k = 0; k < d; k++) a += (WORD)(p[k] >> 16) + (WORD)p[k];
        a >>= dsh;
        m->re[j] = (WORD)((a * m->win[j]) >> 15);
        m->im[j] = 0;
    }
    int s = fft(m->re, m->im, m->cs, m->sn);
    ULONG fs = rate / d;
    if (fs != m->band_rate) {
        m->band_rate = fs;
        for (int b = 0; b < N_BANDS; b++) {
            /* the bins nearest the edges: a narrow low band still gets its own */
            ULONG lo = ((ULONG)band_hz[b] * FFT_N + fs / 2) / fs, hi = ((ULONG)band_hz[b + 1] * FFT_N + fs / 2) / fs;
            if (lo < 1) lo = 1;
            if (hi <= lo) hi = lo + 1;
            if (hi > FFT_N / 2) hi = FFT_N / 2;
            if (lo >= hi) lo = hi - 1;
            m->band_lo[b] = (UWORD)lo;
            m->band_hi[b] = (UWORD)hi;
        }
    }
    /* a full-scale sine, windowed: A N / 4 = 2^25 before the halvings */
    int ref = 2 * (25 - s);
    for (int b = 0; b < N_BANDS; b++) {
        ULONG big = 0;
        for (int k = m->band_lo[b]; k < m->band_hi[b]; k++) {
            ULONG p = (ULONG)((LONG)m->re[k] * m->re[k]) + (ULONG)((LONG)m->im[k] * m->im[k]);
            if (p > big) big = p;
        }
        int v = db_pow(big, ref);
        fall(&m->band[b], v, 400, dt);
        if (v >= m->bpk[b]) { m->bpk[b] = v; m->bpk_t[b] = 0; }
        else if ((m->bpk_t[b] += dt) > 800) fall(&m->bpk[b], v, 120, dt);
    }

    /* the scope: 20 ms from a rising zero crossing, as recent as there is */
    int span = (int)(rate / 50), hi = (int)need - span, lo = (int)need - 2 * span, t = hi;
    if (lo < 1) lo = 1;
    for (int i = hi; i > lo; i--) {
        LONG a = (WORD)(m->hist[i - 1] >> 16) + (WORD)m->hist[i - 1];
        LONG b = (WORD)(m->hist[i] >> 16) + (WORD)m->hist[i];
        if (a < 0 && b >= 0) { t = i; break; }
    }
    m->sc_t = t;
    m->sc_n = span;
    m->sc_ok = 1;
}

/* the recorder: what came into the rings since the last tick, to its files */
static void rec_poll(struct mon *m)
{
    struct rec *r = &m->rec;
    struct mon_hdr *h = m->h;
    if (!r->on) return;
    if (h->rate != r->rate) {
        rec_stop(r);
        snprintf(m->rec_msg, sizeof m->rec_msg, "stopped: the sound's rate changed - %s kept", r->path);
        return;
    }
    ULONG wr = h->wr, room = RING_FRAMES - 8192;    /* the newest 8192 may be written still */
    if (wr < r->rd) r->rd = wr;                    /* the ring taken anew */
    ULONG avail = wr - r->rd;
    if (avail > room) { r->dropped += avail - room; r->rd = wr - room; avail = room; }
    const ULONG *ring = (const ULONG *)(h + 1);
    ULONG s = r->rd & (RING_FRAMES - 1), first = RING_FRAMES - s;
    if (first > avail) first = avail;
    rec_mix(r, ring + s, first);
    if (avail > first) rec_mix(r, ring, avail - first);
    r->rd += avail;
    if (r->voices && r->on) {
        const ULONG *vr = (const ULONG *)((const UBYTE *)(h + 1) + RING_FRAMES * 4);
        ULONG vw = h->vwr, vroom = VRING_FRAMES - 2048;
        if (vw < r->vrd) r->vrd = vw;
        ULONG va = vw - r->vrd;
        if (va > vroom) { r->vrd = vw - vroom; va = vroom; }
        ULONG vs = r->vrd & (VRING_FRAMES - 1), vf = VRING_FRAMES - vs;
        if (vf > va) vf = va;
        rec_voices(r, vr + vs, vf);
        if (va > vf) rec_voices(r, vr, va - vf);
        r->vrd += va;
    }
    if (r->err) snprintf(m->rec_msg, sizeof m->rec_msg, "stopped: the disk would not take more - %s", r->path);
}

/* ---- drawing ------------------------------------------------------------------- */
struct dr { struct RastPort *rp; int x, y; LONG pen[N_PENS]; };

static void rf(struct dr *d, int pen, int x0, int y0, int x1, int y1)
{
    if (x1 < x0 || y1 < y0) return;
    SetAPen(d->rp, d->pen[pen]);
    RectFill(d->rp, d->x + x0, d->y + y0, d->x + x1, d->y + y1);
}

static int tw(struct dr *d, const char *s) { return TextLength(d->rp, (STRPTR)s, strlen(s)); }

static void tx(struct dr *d, int pen, int x, int y, const char *s, int maxw)
{
    struct TextExtent te;
    ULONG n = strlen(s);
    if (maxw > 0) n = TextFit(d->rp, (STRPTR)s, n, &te, NULL, 1, maxw, 32767);
    SetAPen(d->rp, d->pen[pen]);
    Move(d->rp, d->x + x, d->y + y + d->rp->TxBaseline);
    Text(d->rp, (STRPTR)s, n);
}

static void khz(char *o, ULONG hz)
{
    if (hz % 1000) sprintf(o, "%lu.%lu", (unsigned long)(hz / 1000), (unsigned long)(hz % 1000 / 100));
    else sprintf(o, "%lu", (unsigned long)(hz / 1000));
}

static void fmt_db(char *o, int v)
{
    if (v <= FLOOR) { strcpy(o, "-inf"); return; }
    int a = v < 0 ? -v : v;
    sprintf(o, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
}

static const char *hdmi_reason(int st)
{
    switch (st) {
    case 0:  return "Sound on HDMI is Off (Chipset menu - Sound)";
    case 2:  return "no HDMI link that carries sound";
    case 3:  return "the HDMI clock is off";
    case 4:  return "the Pi's firmware holds its packet slot";
    case 5:  return "no DMA channel is free";
    case 6:  return "an old test kernel without it";
    case 7:  return "cmdline.txt says aga.nohdmiaudio";
    case 8:  return "another program plays over HDMI";
    default: return "nothing plays";
    }
}

/* the screen as its EDID tells it (Linux's ELD): name, LPCM's channels and
   sizes, the rates I may damn well use, the other formats it decodes, its
   speakers */
static void screen_line(char *s, const struct mon_eld *e, ULONG offer, const ULONG *rates)
{
    static const char *const fmt[16] = { "?", "LPCM", "AC-3", "MPEG-1", "MP3", "MPEG-2", "AAC", "DTS", "ATRAC",
                                         "DSD", "DD+", "DTS-HD", "TrueHD", "DST", "WMA Pro", "ext" };
    char a[16];
    int ch = e->flags & ELD_BASIC ? 2 : 0, sz = e->flags & ELD_BASIC ? 1 : 0;
    for (int i = 0; i < e->nsad && i < 10; i++)
        if (((e->sad[3 * i] >> 3) & 15) == 1) {
            if ((e->sad[3 * i] & 7) + 1 > ch) ch = (e->sad[3 * i] & 7) + 1;
            sz |= e->sad[3 * i + 2] & 7;
        }
    sprintf(s, "%s:", e->name[0] ? e->name : (e->flags & ELD_READ) ? "the screen" : "no EDID");
    if (ch) {
        sprintf(s + strlen(s), " LPCM %dch %s%s%s bit,", ch, sz & 1 ? "16" : "", sz & 2 ? (sz & 1 ? "/20" : "20") : "",
                sz & 4 ? (sz & 3 ? "/24" : "24") : "");
    }
    strcat(s, " takes");
    for (int i = 0; i < 6; i++)
        if (offer & (1UL << i)) { khz(a, rates[i]); strcat(s, " "); strcat(s, a); }
    strcat(s, " kHz");
    int other = 0;
    for (int i = 0; i < e->nsad && i < 10; i++) {
        int f = (e->sad[3 * i] >> 3) & 15;
        if (f == 1) continue;
        strcat(s, other++ ? ", " : "; also ");
        strcat(s, fmt[f]);
    }
    if (e->spk[0]) {                               /* FL/FR, LFE, FC, RL/RR, RC, FLC/FRC, RLC/RRC */
        int n = 2 * !!(e->spk[0] & 1) + !!(e->spk[0] & 4) + 2 * !!(e->spk[0] & 8) + !!(e->spk[0] & 16)
              + 2 * !!(e->spk[0] & 32) + 2 * !!(e->spk[0] & 64);
        sprintf(s + strlen(s), "; speakers %d.%d", n, !!(e->spk[0] & 2));
    }
}

/* the effects as the kernel has them, with the preset's name from ENV: */
static void effects_line(struct mon *m, char *s)
{
    static const char *const xf[4] = { "", ", crossfeed light", ", crossfeed strong", "" };
    ULONG w = m->fx;
    if (w != m->fx_seen) {                         /* a change: the preset's name again */
        struct eq_set e;
        eq_load(&e);
        strcpy(m->eq_name, eq_preset_name(e.preset) ? eq_preset_name(e.preset) : "Custom");
        m->fx_seen = w;
    }
    int pre = (int)((w >> 4) & 31) - 12, bal = (int)((w >> 9) & 31) - 12;
    if (w & 1) {
        sprintf(s, "equalizer %s", m->eq_name);
        if (pre) sprintf(s + strlen(s), " (preamp %+d dB)", pre);
        strcat(s, xf[(w >> 2) & 3]);
    } else strcpy(s, "equalizer off");
    if (w & 2) {
        if (m->lim_ms > 0) sprintf(s + strlen(s), ", limiter -%d.%d dB", m->lim_db / 10, m->lim_db % 10);
        else strcat(s, ", limiter on");
    }
    if (bal) sprintf(s + strlen(s), ", balance %s -%d dB", bal > 0 ? "L" : "R", bal > 0 ? bal : -bal);
}

static void draw_status(struct mon *m, struct dr *d, const struct lay *L)
{
    static const char *const label[5] = { "HDMI", "HiFi", "Paula", "Screen", "Effects" };
    static const ULONG rates[6] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    static const int all[5] = { 0, 1, 2, 3, 4 }, low[3] = { 0, 1, 4 };
    char s[5][256], a[16];
    ULONG st = m->state;
    int vx = L->lw, vw = L->dw - L->lw;

    if (!m->taken) strcpy(s[0], m->why);
    else if (!m->playing) sprintf(s[0], "silent - %s", hdmi_reason(m->hdmi_state));
    else {
        ULONG lt = m->rate ? m->h->lead * 10000UL / m->rate : 0, rs = m->h->resyncs;
        khz(a, m->rate);
        sprintf(s[0], "%s kHz, %s bit, %lu.%lu ms ahead, %lu resync%s", a, (st & MON_OUT24) ? "24" : "16",
                (unsigned long)(lt / 10), (unsigned long)(lt % 10), (unsigned long)rs, rs == 1 ? "" : "s");
    }
    if (!m->playing) strcpy(s[1], "-");
    else if (!(st & MON_AHI)) strcpy(s[1], "off - no AHI program plays to HDMI");
    else if (!(st & MON_AHIPLAY)) strcpy(s[1], "AHI's ring is on, nothing in it");
    else {
        ULONG ms = m->rate ? m->h->ahi_fill * 1000UL / m->rate : 0, un = m->h->ahi_under;
        sprintf(s[1], "playing, %s bit%s, %lu ms queued, %lu dropout%s", (st & MON_AHI32) ? "32" : "16",
                (st & MON_LOWLAT) ? ", low latency" : "", (unsigned long)ms, (unsigned long)un, un == 1 ? "" : "s");
    }
    if (!m->playing) strcpy(s[2], "-");
    else if (st & MON_PAULA) sprintf(s[2], "in the mix (System wide), %s%s", (st & MON_PAULABL) ? "band-limited" : "classic",
                                     (m->h->vstate & 2) ? ", voice mixer on" : "");
    else strcpy(s[2], "on the Amiga's own outputs (the real chipset plays)");
    if (!m->taken) strcpy(s[3], "-");
    else screen_line(s[3], &m->h->eld, m->h->offer, rates);
    if (!m->taken) strcpy(s[4], "-");
    else effects_line(m, s[4]);

    const int *which = L->nst == 5 ? all : low;
    for (int i = 0; i < L->nst; i++) {
        int k = which[i];
        tx(d, P_DIM, 0, 2 + i * L->lh, label[k], 0);
        tx(d, P_TEXT, vx, 2 + i * L->lh, s[k], vw);
    }
}

/* the test sound and the recorder, under everything - and only while there
   is something to say: one playing, a recording, how the last one ended */
static void draw_bottom(struct mon *m, struct dr *d, const struct lay *L)
{
    struct test *t = &m->t;
    struct rec *r = &m->rec;
    const char *lab = "Test";
    char s[256], a[16];
    s[0] = 0;
    if (r->on) {
        ULONG secs = r->rate ? r->bytes[0] / (r->rate * 4) : 0;
        lab = "Record";
        snprintf(s, sizeof s, "%02lu:%02lu %s%s, %lu KB", (unsigned long)(secs / 60), (unsigned long)(secs % 60), r->path,
                 r->voices ? " + its 4 voices" : "", (unsigned long)(r->bytes[0] >> 10));
        if (r->dropped) sprintf(s + strlen(s), ", %lu ms lost", (unsigned long)(r->dropped * 1000UL / r->rate));
    }
    else if (t->err == 1) strcpy(s, "ahi.device's unit 0 would not open - AHI not installed?");
    else if (t->err == 2) strcpy(s, "no memory for the test sound");
    else if (t->err == 3) strcpy(s, "AHI would not play it - unit 0's mode set in AHI's prefs?");
    else if (!t->open && m->rec_msg[0]) { lab = "Record"; strcpy(s, m->rec_msg); }
    else if (t->open) {
        static const char *const side[3] = { "left", "right", "both" };
        const char *lv = level_names[t->level];
        switch (t->kind) {
        case T_LEFT: case T_RIGHT: case T_BOTH:
            sprintf(s, "1 kHz, %s, %s", side[t->kind], lv); break;
        case T_SEQ:
            sprintf(s, "1 kHz, %s now (left, right, both), %s", side[t->seg], lv); break;
        case T_SWEEP: {
            ULONG hz = (ULONG)(((unsigned long long)t->inc * T_RATE) >> 32);
            if (hz < 1000) sprintf(a, "%lu Hz", (unsigned long)hz);
            else { khz(a, hz); strcat(a, " kHz"); }
            sprintf(s, "sweep 20 Hz - 20 kHz in 5 s, %s now, %s", a, lv);
            break;
        }
        default:
            sprintf(s, "pink noise, %s rms", lv); break;
        }
    }
    if (!s[0]) return;
    int y = L->sp_y + L->sp_h + L->lh + 1;
    tx(d, P_DIM, 0, y, lab, 0);
    tx(d, P_TEXT, L->lw, y, s, L->dw - L->lw);
}

static int db_x(int v, int x0, int x1)
{
    if (v < FLOOR) v = FLOOR;
    if (v > 0) v = 0;
    return x0 + (v - FLOOR) * (x1 - x0) / -FLOOR;
}

static void draw_meters(struct mon *m, struct dr *d, const struct lay *L)
{
    static const char *const ch[2] = { "L", "R" };
    static const int marks[8] = { -600, -480, -360, -240, -180, -120, -60, 0 };
    static const char *const mark_s[8] = { "-60", "-48", "-36", "-24", "-18", "-12", "-6", "0" };
    char s[24];
    int x0 = L->me_x0, x1 = L->me_x1, yy = db_x(-180, x0, x1), xr = db_x(-60, x0, x1);
    for (int c = 0; c < 2; c++) {
        int y0 = L->me_y + c * (L->me_h + 3), y1 = y0 + L->me_h - 1;
        tx(d, P_DIM, 0, y0 + (L->me_h - L->lh + 2) / 2, ch[c], 0);
        int x = db_x(m->rms[c], x0, x1);
        rf(d, P_GRID, x0, y0, x1, y1);
        rf(d, P_GREEN, x0, y0, (x < yy ? x : yy) - 1, y1);
        if (x > yy) rf(d, P_YELLOW, yy, y0, (x < xr ? x : xr) - 1, y1);
        if (x > xr) rf(d, P_RED, xr, y0, x - 1, y1);
        if (m->pk_hold[c] > FLOOR) {
            int px = db_x(m->pk_hold[c], x0, x1);
            if (px > x1 - 1) px = x1 - 1;
            rf(d, m->pk_hold[c] > -60 ? P_RED : P_PEAK, px, y0, px + 1, y1);
        }
        fmt_db(s, m->pk_hold[c]);
        tx(d, P_TEXT, x1 + 8, y0 + (L->me_h - L->lh + 2) / 2, s, 0);
        if (m->clip_ms[c] > 0) tx(d, P_RED, x1 + 8 + tw(d, "-00.0 "), y0 + (L->me_h - L->lh + 2) / 2, "CLIP", 0);
        else if (m->lim_ms > 0) tx(d, P_YELLOW, x1 + 8 + tw(d, "-00.0 "), y0 + (L->me_h - L->lh + 2) / 2, "LIM", 0);
    }
    int ys = L->me_y + 2 * L->me_h + 3;
    for (int i = 0; i < 8; i++) {
        int x = db_x(marks[i], x0, x1), w = tw(d, mark_s[i]);
        rf(d, P_DIM, x, ys, x, ys + 1);
        int lx = x - w / 2;
        if (lx < x0) lx = x0;
        if (lx + w > x1) lx = x1 - w;
        tx(d, P_DIM, lx, ys + 2, mark_s[i], 0);
    }
}

static void frame(struct dr *d, int pen, int x, int y, int w, int h)
{
    rf(d, pen, x, y, x + w - 1, y);
    rf(d, pen, x, y + h - 1, x + w - 1, y + h - 1);
    rf(d, pen, x, y, x, y + h - 1);
    rf(d, pen, x + w - 1, y, x + w - 1, y + h - 1);
}

/* left and right, 20 ms from where the sound rises through zero */
static void draw_scope(struct mon *m, struct dr *d, int x, int y, int w, int h)
{
    int lane = h / 2;
    for (int c = 0; c < 2; c++) {
        int top = y + c * lane, mid = top + lane / 2, half = lane / 2 - 1;
        rf(d, P_GRID, x, mid, x + w - 1, mid);
        if (c) rf(d, P_GRID, x, top, x + w - 1, top);
        if (!m->sc_ok) continue;
        int pen = c ? P_RIGHT : P_LEFT, sh = c ? 0 : 16;
        LONG prev = (WORD)(m->hist[m->sc_t] >> sh);
        for (int i = 0; i < w; i++) {
            int f0 = m->sc_t + i * m->sc_n / w, f1 = m->sc_t + (i + 1) * m->sc_n / w;
            if (f1 <= f0) f1 = f0 + 1;
            if (f1 > (int)m->need) f1 = (int)m->need;
            LONG lo = prev, hi = prev;
            for (int f = f0; f < f1; f++) {
                LONG v = (WORD)(m->hist[f] >> sh);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                prev = v;
            }
            rf(d, pen, x + i, mid - hi * half / 32768, x + i, mid - lo * half / 32768);
        }
    }
    tx(d, P_DIM, x + 2, y, "L", 0);
    tx(d, P_DIM, x + 2, y + lane, "R", 0);
    tx(d, P_DIM, x + w - tw(d, "20 ms") - 2, y, "20 ms", 0);
}

/* a VU meter, the way the old ones look: cream, an arc from -20 to +3 VU,
   red from 0, a needle on 300 ms. 0 VU is -18 dBFS of a sine (EBU), and as
   on the real thing the needle moves with the level, not with the dB */
static const UWORD vu_pos[11] = { 71, 224, 316, 398, 501, 562, 631, 708, 794, 891, 1000 };   /* 10^(VU/20)/1.4125 */
/* where 0 VU sits: broadcast's -18 dBFS pins the needle on the loud mixes
   Amiga music has (a ProTracker module through HDMI's 200 %: about -7 dBFS,
   +11 VU), so -8 by default and a click on a meter steps through the rest.
   vu_full: the rms of a sine at +3 VU, in 16 bits */
#define N_VUREF 7
static const BYTE vu_ref_db[N_VUREF] = { -18, -14, -12, -10, -8, -6, -4 };
static const UWORD vu_full[N_VUREF] = { 4120, 6530, 8221, 10350, 13029, 16403, 20650 };
#define VU_VAR "AGA_VUREF"
static const char *const vu_lab[11] = { "20", "10", "7", "5", "3", "", "", "0", "", "", "3" };

static void vu_point(struct mon *m, int px, int py, int r, int pos, int *x, int *y)
{
    int a = -569 + pos * 1138 / 1000;              /* -50 .. +50 degrees, 4096 a turn */
    *x = px + (int)(qs(m->qsin, a) * r / 32768);
    *y = py - (int)(qs(m->qsin, a + 1024) * r / 32768);
}

static void draw_vu(struct mon *m, struct dr *d, int x, int y, int w, int h, int c, int lh)
{
    m->vu_x[c] = x; m->vu_y[c] = y; m->vu_w[c] = w; m->vu_h[c] = h;
    rf(d, P_FACE, x, y, x + w - 1, y + h - 1);
    frame(d, P_INK, x, y, w, h);
    int px = x + w / 2, py = y + h + h / 6;        /* the pivot, below the window, as on the real one */
    int r = (w / 2 - 6) * 1000 / 766, rmax = py - y - 6;
    if (r > rmax) r = rmax;
    if (r < 12) return;
    int x0, y0, x1, y1;
    /* the arc, red from 0 VU */
    for (int i = 0; i < 40; i++) {
        int p0 = i * 25, p1 = (i + 1) * 25, red = p0 >= 708;
        for (int k = 0; k < (red ? 3 : 1); k++) {
            vu_point(m, px, py, r - k, p0, &x0, &y0);
            vu_point(m, px, py, r - k, p1, &x1, &y1);
            SetAPen(d->rp, d->pen[red ? P_RED : P_INK]);
            Move(d->rp, d->x + x0, d->y + y0);
            Draw(d->rp, d->x + x1, d->y + y1);
        }
    }
    for (int i = 0; i < 11; i++) {                 /* the marks and the numbers */
        vu_point(m, px, py, r + 1, vu_pos[i], &x0, &y0);
        vu_point(m, px, py, r + 5, vu_pos[i], &x1, &y1);
        SetAPen(d->rp, d->pen[i >= 7 ? P_RED : P_INK]);
        Move(d->rp, d->x + x0, d->y + y0);
        Draw(d->rp, d->x + x1, d->y + y1);
        if (vu_lab[i][0] && r > 3 * lh) {
            vu_point(m, px, py, r - lh, vu_pos[i], &x0, &y0);
            tx(d, i >= 7 ? P_RED : P_INK, x0 - tw(d, vu_lab[i]) / 2, y0 - lh / 2, vu_lab[i], 0);
        }
    }
    tx(d, P_INK, px - tw(d, "VU") / 2, py - r / 2 - lh / 2, "VU", 0);
    tx(d, P_INK, x + 4, y + h - lh, c ? "R" : "L", 0);
    {   /* where 0 VU is, under the needle's foot */
        char rs[24];
        sprintf(rs, "0 VU = %d dBFS", vu_ref_db[m->vu_ref]);
        if (tw(d, rs) < w - 2 * tw(d, "RR") - 8 && r > 2 * lh) tx(d, P_INK, px - tw(d, rs) / 2, y + h - lh, rs, 0);
    }
    /* the peak lamp: -3 dBFS */
    rf(d, m->vu_led[c] > 0 ? P_RED : P_GRID, x + w - 10, y + 4, x + w - 5, y + 8);
    /* the needle */
    ULONG rms = (ULONG)isqrt64((unsigned long long)(m->vu_ms[c] > 0 ? m->vu_ms[c] : 0));
    int pos = (int)(rms * 1000 / vu_full[m->vu_ref]);   /* 1000: +3 VU */
    if (pos > 1030) pos = 1030;
    vu_point(m, px, py, r + 3, pos, &x1, &y1);
    y0 = y + h - 2;                                /* from where it comes into the window */
    x0 = py > y1 ? px + (x1 - px) * (py - y0) / (py - y1) : px;
    SetAPen(d->rp, d->pen[P_INK]);
    Move(d->rp, d->x + x0, d->y + y0);
    Draw(d->rp, d->x + x1, d->y + y1);
    Move(d->rp, d->x + x0 + 1, d->y + y0);
    Draw(d->rp, d->x + x1 + 1, d->y + y1);
}

/* left against right: the goniometer - the middle goes up, the sides out
   to the diagonals - and under it how alike they are, -1 to +1 */
static void draw_vector(struct mon *m, struct dr *d, int x, int y, int w, int h, int lh)
{
    int side = h - lh - 4;
    if (side > w) side = w;
    if (side < 16) return;
    int sx = x + (w - side) / 2, cx = sx + side / 2, cy = y + side / 2, half = side / 2 - 1;
    frame(d, P_GRID, sx, y, side, side);
    SetAPen(d->rp, d->pen[P_GRID]);
    Move(d->rp, d->x + sx, d->y + y); Draw(d->rp, d->x + sx + side - 1, d->y + y + side - 1);
    Move(d->rp, d->x + sx + side - 1, d->y + y); Draw(d->rp, d->x + sx, d->y + y + side - 1);
    rf(d, P_GRID, cx, y, cx, y + side - 1);
    tx(d, P_DIM, sx + 3, y + 1, "L", 0);
    tx(d, P_DIM, sx + side - tw(d, "R") - 3, y + 1, "R", 0);
    tx(d, P_DIM, cx + 3, y + 1, "M", 0);
    if (m->sc_ok) {
        int n = (int)(m->rate / 20), pts = m->deep ? 1200 : 400;
        if (n > (int)m->need) n = (int)m->need;
        int k = n / pts > 0 ? n / pts : 1;
        SetAPen(d->rp, d->pen[P_LEFT]);
        for (int i = (int)m->need - n; i < (int)m->need; i += k) {
            LONG l = (WORD)(m->hist[i] >> 16), r = (WORD)m->hist[i];
            WritePixel(d->rp, d->x + cx + (int)((r - l) * half / 65536), d->y + cy - (int)((l + r) * half / 65536));
        }
    }
    int by = y + side + 3, bh = lh - 4;
    if (bh < 3) bh = 3;
    rf(d, P_GRID, sx, by, sx + side - 1, by + bh - 1);
    rf(d, P_DIM, cx, by - 1, cx, by + bh);
    if (m->corr_ok) {
        int mx = sx + (m->corr + 1000) * (side - 4) / 2000;
        rf(d, m->corr < 0 ? P_RED : m->corr < 300 ? P_YELLOW : P_GREEN, mx, by, mx + 3, by + bh - 1);
    }
}

/* a period as a note, ProTracker's way: 428 is C-2; any period, not just
   ProTracker's three octaves */
static void note_name(char *o, UWORD per)
{
    static const char *const nn[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    if (per < 28) { strcpy(o, "---"); return; }
    LONG semis = ((lg2q8(856) - lg2q8(per)) * 12 + 128 + 256 * 48) / 256 - 48;   /* above C-1 */
    LONG idx = semis + 12;
    if (idx < 0 || idx > 107) { strcpy(o, "---"); return; }
    sprintf(o, "%s%ld", nn[idx % 12], (long)(idx / 12));
}

/* Paula's four voices: each its scope and what it plays - a click mutes
   one, the right button hears it alone */
static void draw_voices(struct mon *m, struct dr *d, int x, int y, int w, int h, int lh)
{
    m->vp_x = x; m->vp_y = y; m->vp_w = w; m->vp_h = h;
    int cw = w / 4;
    if (!m->vn) {
        for (int n = 0; n < 4; n++) frame(d, P_GRID, x + n * cw + 1, y, cw - 3, h);
        const char *s1 = "Paula voices for your software show here!", *s2 = "(System wide on: Chipset menu - Expert)";
        tx(d, P_DIM, x + (w - tw(d, s1)) / 2, y + h / 2 - lh, s1, w);
        tx(d, P_DIM, x + (w - tw(d, s2)) / 2, y + h / 2, s2, w);
        return;
    }
    int lines = h >= 5 * lh ? 2 : 1;
    for (int n = 0; n < 4; n++) {
        const struct mon_voice *v = &m->h->v[n];
        int x0 = x + n * cw + 1, ww = cw - 3, sy = y + lines * lh + 1, sh = y + h - sy;
        int muted = m->vm.solo >= 0 ? m->vm.solo != n : m->vm.mute[n];
        char s[48], nt[8];
        if (v->flags & 2) note_name(nt, v->per); else strcpy(nt, "---");
        sprintf(s, "%d %s %2u", n + 1, nt, (unsigned)v->vol);
        tx(d, muted ? P_DIM : P_TEXT, x0 + 2, y, s, ww - 2);
        if (m->vm.solo == n) tx(d, P_YELLOW, x0 + ww - tw(d, "SOLO") - 2, y, "SOLO", 0);
        else if (muted) tx(d, P_RED, x0 + ww - tw(d, "MUTE") - 2, y, "MUTE", 0);
        if (lines == 2) {
            int pn = v->pan - 8;                   /* short: L, R, C, L3 - a column is narrow */
            char ps[8];
            if (pn <= -8) strcpy(ps, "L"); else if (pn >= 8) strcpy(ps, "R"); else if (!pn) strcpy(ps, "C");
            else sprintf(ps, "%c%d", pn < 0 ? 'L' : 'R', pn < 0 ? -pn : pn);
            sprintf(s, "%lu B %s", (unsigned long)v->len * 2, ps);
            tx(d, P_DIM, x0 + 2, y + lh, s, ww - 2);
        }
        if (sh < 6) continue;
        frame(d, P_GRID, x0, sy, ww, sh);
        int mid = sy + sh / 2, half = sh / 2 - 2, bits = 24 - 8 * n, pen = muted ? P_DIM : (n == 0 || n == 3) ? P_LEFT : P_RIGHT;
        int t0 = m->vtrig[n], span = m->vspan, iw = ww - 2;
        LONG prev = (BYTE)(m->vhist[t0] >> bits);
        for (int i = 0; i < iw; i++) {
            int f0 = t0 + i * span / iw, f1 = t0 + (i + 1) * span / iw;
            if (f1 <= f0) f1 = f0 + 1;
            if (f1 > m->vn) f1 = m->vn;
            LONG lo = prev, hi = prev;
            for (int f = f0; f < f1; f++) {
                LONG s8 = (BYTE)(m->vhist[f] >> bits);
                if (s8 < lo) lo = s8;
                if (s8 > hi) hi = s8;
                prev = s8;
            }
            rf(d, pen, x0 + 1 + i, mid - hi * half / 128, x0 + 1 + i, mid - lo * half / 128);
        }
    }
}

static void draw_spectrum(struct mon *m, struct dr *d, const struct lay *L)
{
    static const UWORD fl[7] = { 50, 100, 500, 1000, 5000, 10000, 20000 };
    static const char *const fl_s[7] = { "50", "100", "500", "1k", "5k", "10k", "20k" };
    static const char *const dbl[4] = { "0", "-20", "-40", "-60" };
    int x0 = L->sp_x0, x1 = L->dw - 1, y0 = L->sp_y, y1 = L->sp_y + L->sp_h - 1, w = x1 - x0 + 1;
    LONG a = lg2q8(band_hz[0]), z = lg2q8(band_hz[N_BANDS]) - a;
    for (int i = 0; i < 4; i++) {                  /* 0, -20, -40, -60 dB */
        int y = y0 + i * (L->sp_h - 1) / 3;
        rf(d, P_GRID, x0, y, x1, y);
        int ty = i == 0 ? y : i == 3 ? y - L->lh + 2 : y - L->lh / 2;
        tx(d, P_DIM, x0 - 4 - tw(d, dbl[i]), ty, dbl[i], 0);
    }
    for (int i = 0; i < 7; i++) {
        int x = x0 + (int)((lg2q8(fl[i]) - a) * w / z), lw = tw(d, fl_s[i]);
        if (i == 1 || i == 3 || i == 5) rf(d, P_GRID, x, y0, x, y1);
        int lx = i == 6 ? x1 - lw : x - lw / 2;
        tx(d, P_DIM, lx, y1 + 2, fl_s[i], 0);
    }
    for (int b = 0; b < N_BANDS; b++) {
        int bx0 = x0 + b * w / N_BANDS, bx1 = x0 + (b + 1) * w / N_BANDS - 2;
        if (bx1 < bx0) bx1 = bx0;
        int h = (m->band[b] - FLOOR) * L->sp_h / -FLOOR;
        if (h > 0) rf(d, P_SPEC, bx0, y1 - h + 1, bx1, y1);
        if (m->bpk[b] > FLOOR) {
            int py = y1 - (m->bpk[b] - FLOOR) * (L->sp_h - 1) / -FLOOR;
            rf(d, P_PEAK, bx0, py, bx1, py);
        }
    }
}

static void draw_all(struct mon *m, struct dr *d, const struct lay *L)
{
    rf(d, P_BG, 0, 0, L->dw - 1, L->dh - 1);
    draw_status(m, d, L);
    draw_meters(m, d, L);
    int y = L->bg_y, h = L->bg_h, w = L->dw, lh = L->lh;
    m->vp_w = 0;
    m->vu_w[0] = m->vu_w[1] = 0;
    switch (m->view) {
    case V_SCOPE:  draw_scope(m, d, 0, y, w, h); break;
    case V_VOICES: draw_voices(m, d, 0, y, w, h, lh); break;
    case V_VU:
        draw_vu(m, d, 0, y, w / 2 - 4, h, 0, lh);
        draw_vu(m, d, w / 2 + 4, y, w - w / 2 - 4, h, 1, lh);
        break;
    case V_VECTOR: draw_vector(m, d, 0, y, w, h, lh); break;
    default: {                                     /* all at once: VU meters and vectorscope, scope and voices */
        int h1 = (h - 6) / 2, sq = h1 * 9 / 10 + lh, vw = (w - sq - 16) / 2;
        draw_vu(m, d, 0, y, vw, h1, 0, lh);
        draw_vu(m, d, vw + 6, y, vw, h1, 1, lh);
        draw_vector(m, d, w - sq, y, sq, h1, lh);
        draw_scope(m, d, 0, y + h1 + 6, w / 2 - 4, h - h1 - 6);
        draw_voices(m, d, w / 2 + 4, y + h1 + 6, w - w / 2 - 4, h - h1 - 6, lh);
        break;
    }
    }
    draw_spectrum(m, d, L);
    draw_bottom(m, d, L);
}

/* ---- the window ---------------------------------------------------------------- */
enum { G_REFRESH = 1, G_VOL, G_VOLTXT, G_MUTE, G_TEST, G_LEVEL, G_PLAY, G_STOP, G_EQ, G_VIEW, G_REC, G_RECSTOP, G_FOLDER, G_VOICES };
static const char *const refresh_names[] = { "25/s", "12/s", "6/s", "Paused", NULL };
static const int refresh_ms[4] = { 40, 83, 167, 250 };   /* paused: the beat, the state and the recorder only */

static int widest(struct RastPort *rp, const char *const *s)
{
    int w = 0;
    for (; *s; s++) {
        int t = TextLength(rp, (STRPTR)*s, strlen(*s));
        if (t > w) w = t;
    }
    return w;
}
static int tl(struct RastPort *rp, const char *s) { return TextLength(rp, (STRPTR)s, strlen(s)); }

static void vol_text(char *o, int db) { sprintf(o, db ? "-%d dB" : "0 dB", db); }

/* a button at x, row y: its width */
static struct Gadget *button(struct Gadget *g, struct NewGadget *ng, int x, int y, int w, const char *t, int id, int off)
{
    ng->ng_LeftEdge = x; ng->ng_TopEdge = y; ng->ng_Width = w;
    ng->ng_GadgetText = (UBYTE *)t; ng->ng_GadgetID = id; ng->ng_Flags = PLACETEXT_IN;
    return CreateGadget(BUTTON_KIND, g, ng, GA_Disabled, off, TAG_DONE);
}

int monitor_window(char *msg, int len)
{
    struct mon *m = NULL;
    struct mon_hdr *ring = NULL;
    struct MsgPort *pub = NULL, *tport = NULL;
    struct timerequest *treq = NULL;
    struct Screen *scr = NULL;
    APTR vi = NULL;
    struct Gadget *glist = NULL, *g, *g_vol = NULL, *g_voltxt = NULL, *g_play = NULL, *g_stop = NULL;
    struct Gadget *g_rec = NULL, *g_recstop = NULL;
    struct Window *win = NULL;
    struct BitMap *bm = NULL;
    struct TextFont *font = NULL;
    struct RastPort brp;
    struct dr d;
    struct lay L;
    int opened_intui = 0, opened_gfx = 0, opened_gt = 0, timer_open = 0, timer_busy = 0, lent = 0, rc = 5, pens = 0;
    LONG own[N_PENS];                              /* the pens ObtainBestPen gave, -1 none */
    int db0, mute0, db, mute, refresh;
    struct eqw *eqw = NULL;                        /* the Equalizer sitting beside me, or none */
    struct vmw *vmw = NULL;                        /* the voice mixer sitting beside me, or none */
    struct eq_set eqs;
    static char voltxt[16];
    msg[0] = 0;
    memset(&d, 0, sizeof d);

    /* one window: a second call brings the first one to the front */
    Forbid();
    struct MsgPort *other = FindPort((STRPTR)MON_PORT);
    if (other && other->mp_SigTask) Signal((struct Task *)other->mp_SigTask, SIGBREAKF_CTRL_F);
    Permit();
    if (other) return 0;

    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39))) opened_intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39))) opened_gfx = 1;
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39))) opened_gt = 1;
    if (!IntuitionBase || !GfxBase || !GadToolsBase || GfxBase->LibNode.lib_Version < 39) {
        snprintf(msg, len, "The Sound monitor needs AmigaOS 3.0 or newer.");
        goto out;
    }
    if (!(m = AllocVec(sizeof *m, MEMF_ANY | MEMF_CLEAR)) ||
        !(ring = AllocVec(sizeof *ring + (RING_FRAMES + VRING_FRAMES) * 4, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR))) {
        snprintf(msg, len, "No memory for the Sound monitor (it needs about 550 KB).");
        goto out;
    }
    if ((pub = CreateMsgPort())) {
        pub->mp_Node.ln_Name = (char *)MON_PORT;
        pub->mp_Node.ln_Pri = 0;
        AddPort(pub);
    }
    if (!(tport = CreateMsgPort()) || !(treq = (struct timerequest *)CreateIORequest(tport, sizeof *treq)) ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0)) {
        snprintf(msg, len, "The Sound monitor could not open timer.device.");
        goto out;
    }
    timer_open = 1;
    tables(m);
    for (int c = 0; c < 2; c++) m->rms[c] = m->pk_hold[c] = FLOOR;
    for (int b = 0; b < N_BANDS; b++) m->band[b] = m->bpk[b] = FLOOR;
    db0 = db = hifivol_pref(&mute0);
    mute = mute0;
    vm_load(&m->vm);
    m->vu_ref = 4;                                 /* -8 dBFS */
    {
        char rv[8];
        if (GetVar((STRPTR)VU_VAR, (STRPTR)rv, sizeof rv, 0) > 0)
            for (int i = 0; i < N_VUREF; i++) if (atoi(rv) == vu_ref_db[i]) m->vu_ref = i;
    }

    /* the ring: lent, and taken at once by a kernel that knows it */
    ring->magic = MON_MAGIC;
    ring->frames = RING_FRAMES;
    ring->vframes = VRING_FRAMES;
    m->h = ring;
    ring_lend(ring);
    lent = 1;
    m->taken = (ring->state & MON_TAKEN) != 0;
    if (!m->taken) set_why(m);

    if (!(scr = LockPubScreen(NULL)) || !(vi = GetVisualInfo(scr, TAG_DONE))) {
        snprintf(msg, len, "The Sound monitor found no screen to open on.");
        goto out;
    }
    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize, gh = fh + 6;
    m->deep = GetBitMapAttr(srp->BitMap, BMA_DEPTH) > 8;
    L.lh = fh + 2;
    {
        static const char *const lab[] = { "HDMI", "HiFi", "Paula", "Screen", "Effects", "Record", NULL };   /* Record: the bottom line's */
        L.lw = widest(srp, lab) + 10;
    }
    int l1 = tl(srp, "Refresh") + 8, c1 = widest(srp, refresh_names) + 36;
    int l2 = tl(srp, "HiFi volume") + 8, sw = 90, rw = tl(srp, "-62 dB") + 8, cb = 26, l3 = tl(srp, "Mute") + 6;
    int l4 = tl(srp, "Test") + 8, c4 = widest(srp, test_names) + 36;
    int l5 = tl(srp, "Level") + 8, c5 = widest(srp, level_names) + 36;
    int l6 = tl(srp, "View") + 8, c6 = widest(srp, view_names) + 36;
    static const char *const pb[] = { "Play", "Stop", NULL };
    int bw = widest(srp, pb) + 20, eqw_w = tl(srp, "Equalizer...") + 20, vow = tl(srp, "Voices...") + 20;
    int rbw = tl(srp, "Record") + 20, fbw = tl(srp, "Folder...") + 20;
    int row1 = l1 + c1 + 16 + l2 + sw + 4 + rw + 8 + cb + l3 + 12 + eqw_w;
    int row2 = l4 + c4 + 16 + l5 + c5 + 16 + bw + 8 + bw + 12 + vow;
    int row3 = l6 + c6 + 16 + rbw + 8 + bw + 8 + fbw;
    int maxw = scr->Width - scr->WBorLeft - scr->WBorRight - 24;
    L.dw = 512;
    if (row1 > L.dw) L.dw = row1;
    if (row2 > L.dw) L.dw = row2;
    if (row3 > L.dw) L.dw = row3;
    if (scr->Width >= 900 && L.dw < 760) L.dw = 760;   /* room for everything at once */
    if (L.dw > maxw) L.dw = maxw;

    /* heights: the view and the spectrum get what the screen has left; a low
       screen gets three status lines */
    int title = scr->WBorTop + fh + 1;
    L.nst = scr->Height < 360 ? 3 : 5;
    int st_h = L.nst * L.lh + 4;
    L.me_h = fh < 8 ? 8 : fh;
    int me_all = 2 * L.me_h + 3 + 2 + L.lh;
    int fixed = 6 + st_h + 6 + me_all + 8 + 8 + 2 * L.lh + 8 + 3 * gh + 2 * 6 + 8;   /* + the test/record line */
    int avail = scr->Height - title - scr->WBorBottom - fixed - 4;
    L.bg_h = avail * 55 / 100;
    if (L.bg_h < 48) L.bg_h = 48;
    if (L.bg_h > 320) L.bg_h = 320;
    L.sp_h = avail - L.bg_h;
    if (L.sp_h < 40) L.sp_h = 40;
    if (L.sp_h > 180) L.sp_h = 180;
    L.me_y = st_h + 6;
    L.me_x0 = tl(srp, "R") + 8;
    L.me_x1 = L.dw - tl(srp, "-00.0 CLIP") - 12;
    L.bg_y = L.me_y + me_all + 8;
    L.sp_y = L.bg_y + L.bg_h + 8;
    L.sp_x0 = tl(srp, "-60") + 6;
    L.dh = L.sp_y + L.sp_h + 2 * L.lh + 2;        /* the spectrum's labels, then the test/record line */
    L.ox = scr->WBorLeft + 10;
    L.oy = title + 6;
    m->view = L.bg_h >= 140 && L.dw >= 560 ? V_ALL : V_SCOPE;

    /* the gadgets, three rows under the picture */
    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_Height = gh;
    g = CreateContext(&glist);
    int y1 = L.oy + L.dh + 8, y2 = y1 + gh + 6, y3 = y2 + gh + 6, x = L.ox;
    refresh = m->deep ? 0 : 1;                     /* RTG: 25 a second, planar 12 */

    ng.ng_LeftEdge = x + l1; ng.ng_TopEdge = y1; ng.ng_Width = c1;
    ng.ng_GadgetText = (UBYTE *)"Refresh"; ng.ng_GadgetID = G_REFRESH; ng.ng_Flags = PLACETEXT_LEFT;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)refresh_names, GTCY_Active, refresh, TAG_DONE);
    x += l1 + c1 + 16;
    ng.ng_LeftEdge = x + l2; ng.ng_Width = sw;
    ng.ng_GadgetText = (UBYTE *)"HiFi volume"; ng.ng_GadgetID = G_VOL;
    g = g_vol = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 0, GTSL_Max, 62, GTSL_Level, 62 - db,
                             GA_RelVerify, TRUE, GA_Immediate, TRUE, GA_Disabled, !m->taken, TAG_DONE);
    x += l2 + sw + 4;
    vol_text(voltxt, db);
    ng.ng_LeftEdge = x; ng.ng_Width = rw;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_VOLTXT;
    g = g_voltxt = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)voltxt, TAG_DONE);
    x += rw + 8;
    ng.ng_LeftEdge = x; ng.ng_Width = cb;
    ng.ng_GadgetText = (UBYTE *)"Mute"; ng.ng_GadgetID = G_MUTE; ng.ng_Flags = PLACETEXT_RIGHT;
    g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, mute, GTCB_Scaled, TRUE, GA_Disabled, !m->taken, TAG_DONE);
    g = button(g, &ng, L.ox + L.dw - eqw_w, y1, eqw_w, "Equalizer...", G_EQ, !m->taken);

    x = L.ox;
    ng.ng_LeftEdge = x + l4; ng.ng_TopEdge = y2; ng.ng_Width = c4;
    ng.ng_GadgetText = (UBYTE *)"Test"; ng.ng_GadgetID = G_TEST; ng.ng_Flags = PLACETEXT_LEFT;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)test_names, GTCY_Active, 0, TAG_DONE);
    x += l4 + c4 + 16;
    ng.ng_LeftEdge = x + l5; ng.ng_Width = c5;
    ng.ng_GadgetText = (UBYTE *)"Level"; ng.ng_GadgetID = G_LEVEL;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)level_names, GTCY_Active, 1, TAG_DONE);
    m->t.level = 1;
    x += l5 + c5 + 16;
    g = g_play = button(g, &ng, x, y2, bw, "Play", G_PLAY, 0);
    g = g_stop = button(g, &ng, x + bw + 8, y2, bw, "Stop", G_STOP, 1);
    g = button(g, &ng, L.ox + L.dw - vow, y2, vow, "Voices...", G_VOICES, !m->taken);

    x = L.ox;
    ng.ng_LeftEdge = x + l6; ng.ng_TopEdge = y3; ng.ng_Width = c6;
    ng.ng_GadgetText = (UBYTE *)"View"; ng.ng_GadgetID = G_VIEW; ng.ng_Flags = PLACETEXT_LEFT;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)view_names, GTCY_Active, m->view, TAG_DONE);
    x += l6 + c6 + 16;
    g = g_rec = button(g, &ng, x, y3, rbw, "Record", G_REC, !m->taken);
    g = g_recstop = button(g, &ng, x + rbw + 8, y3, bw, "Stop", G_RECSTOP, 1);
    g = button(g, &ng, x + rbw + 8 + bw + 8, y3, fbw, "Folder...", G_FOLDER, 0);
    if (!g) goto out;

    int iw = L.dw + 20, ih = 6 + L.dh + 8 + 3 * gh + 2 * 6 + 8;
    win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - Sound monitor",
        WA_Gadgets, (ULONG)glist,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_Left, scr->Width > iw ? (scr->Width - iw) / 2 : 0,
        WA_Top, scr->Height > ih + title ? (scr->Height - ih - title) / 2 : 0,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE, WA_RMBTrap, TRUE,
        WA_IDCMP, CYCLEIDCMP | SLIDERIDCMP | CHECKBOXIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW |
                  IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS,
        TAG_DONE);
    if (!win) { snprintf(msg, len, "The Sound monitor's window would not open."); goto out; }
    GT_RefreshWindow(win, NULL);

    /* the pens, as near to mine as the screen can fucking get */
    {
        struct ColorMap *cm = scr->ViewPort.ColorMap;
        for (int i = 0; i < N_PENS; i++) {
            ULONG c = pen_rgb[i];
            own[i] = ObtainBestPen(cm, ((c >> 16) & 255) * 0x01010101UL, ((c >> 8) & 255) * 0x01010101UL,
                                   (c & 255) * 0x01010101UL, OBP_Precision, PRECISION_GUI, TAG_DONE);
            d.pen[i] = own[i] >= 0 ? own[i] : (i == P_BG || i == P_INK) ? 1 : 2;
        }
        pens = 1;
    }
    /* drawn off screen and put up in one go - no flicker; straight into the
       window when there is no memory for it */
    font = OpenFont(scr->Font);
    bm = AllocBitMap(L.dw, L.dh, GetBitMapAttr(win->RPort->BitMap, BMA_DEPTH), 0, win->RPort->BitMap);
    if (bm) {
        InitRastPort(&brp);
        brp.BitMap = bm;
        d.rp = &brp;
        d.x = d.y = 0;
    } else {
        d.rp = win->RPort;
        d.x = L.ox;
        d.y = L.oy;
    }
    if (font) SetFont(d.rp, font);
    SetDrMd(d.rp, JAM1);

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = 0;
    treq->tr_time.tv_micro = 1000;
    SendIO((struct IORequest *)treq);
    timer_busy = 1;
    rc = 0;

    ULONG wsig = 1UL << win->UserPort->mp_SigBit, tsig = 1UL << tport->mp_SigBit;
    for (int done = 0, was_open = 0, was_rec = 0; !done; ) {
        ULONG asig = m->t.port ? 1UL << m->t.port->mp_SigBit : 0, esig = eqw ? eqw_sigs(eqw) : 0, vsig = vmw ? vmw_sigs(vmw) : 0;
        ULONG got = Wait(wsig | tsig | asig | esig | vsig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);
        if (esig && (got & esig) && eqw_handle(eqw)) { eqw_close(eqw); eqw = NULL; }
        if (vsig && (got & vsig) && vmw_handle(vmw)) { vmw_close(vmw); vmw = NULL; }
        if (got & SIGBREAKF_CTRL_C) break;
        if (got & SIGBREAKF_CTRL_F) { ScreenToFront(win->WScreen); WindowToFront(win); ActivateWindow(win); }
        if (asig && (got & asig)) test_service(&m->t, m->qsin);
        if ((got & tsig) && CheckIO((struct IORequest *)treq)) {
            WaitIO((struct IORequest *)treq);
            int dt = refresh_ms[refresh];
            if (m->rec.on && dt > 83) dt = 83;     /* recording: often enough for the ring */
            ring->beat++;                          /* still here (the kernel lets a quiet ring go) */
            if (lent && m->taken && !(ring->state & MON_TAKEN) && (m->relend_ms += dt) >= 1000) {
                m->relend_ms = 0;                  /* let go while I was held up: lend the bugger again */
                ring_lend(ring);
            }
            if (lent && !m->taken && (ring->state & MON_TAKEN)) m->taken = 1;
            rec_poll(m);
            if (refresh < 3) {
                analyse(m, dt);
                draw_all(m, &d, &L);
                if (bm) BltBitMapRastPort(bm, 0, 0, win->RPort, L.ox, L.oy, L.dw, L.dh, 0xC0);
            } else ring->peak_l = ring->peak_r = 0;   /* Paused: the picture stands */
            if (m->t.open != was_open) {           /* Play / Stop as the test runs or ended */
                was_open = m->t.open;
                GT_SetGadgetAttrs(g_play, win, NULL, GA_Disabled, was_open, TAG_DONE);
                GT_SetGadgetAttrs(g_stop, win, NULL, GA_Disabled, !was_open, TAG_DONE);
            }
            if (m->rec.on != was_rec) {            /* Record / Stop as the recorder runs or stopped */
                was_rec = m->rec.on;
                GT_SetGadgetAttrs(g_rec, win, NULL, GA_Disabled, was_rec, TAG_DONE);
                GT_SetGadgetAttrs(g_recstop, win, NULL, GA_Disabled, !was_rec, TAG_DONE);
            }
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0;
            treq->tr_time.tv_micro = dt * 1000;
            SendIO((struct IORequest *)treq);
        }
        struct IntuiMessage *im;
        while (!done && (im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            WORD mx = im->MouseX, my = im->MouseY;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            switch (cls) {
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                if (bm) BltBitMapRastPort(bm, 0, 0, win->RPort, L.ox, L.oy, L.dw, L.dh, 0xC0);
                break;
            case IDCMP_CLOSEWINDOW:
                done = 1;
                break;
            case IDCMP_VANILLAKEY:
                if (code == 27) done = 1;
                else if (code == ' ') { if (m->t.open) test_stop(&m->t); else test_start(&m->t, m->qsin); }
                break;
            case IDCMP_MOUSEBUTTONS: {                 /* on a voice scope: mute it, or hear it alone */
                for (int c = 0; c < 2; c++) {          /* on a VU meter: 0 VU one step up (left) or down (right button) */
                    int ux = mx - L.ox - m->vu_x[c], uy = my - L.oy - m->vu_y[c];
                    if (!m->vu_w[c] || ux < 0 || uy < 0 || ux >= m->vu_w[c] || uy >= m->vu_h[c]) continue;
                    if (code == SELECTDOWN) m->vu_ref = (m->vu_ref + 1) % N_VUREF;
                    else if (code == MENUDOWN) m->vu_ref = (m->vu_ref + N_VUREF - 1) % N_VUREF;
                    else break;
                    char rv[8];
                    sprintf(rv, "%d", vu_ref_db[m->vu_ref]);
                    SetVar((STRPTR)VU_VAR, (STRPTR)rv, -1, GVF_GLOBAL_ONLY);
                    SetVar((STRPTR)VU_VAR, (STRPTR)rv, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
                    break;
                }
                int vx = mx - L.ox - m->vp_x, vy = my - L.oy - m->vp_y;
                if (!m->vp_w || !m->vn || vx < 0 || vy < 0 || vx >= m->vp_w || vy >= m->vp_h) break;
                int n = vx * 4 / m->vp_w;
                if (code == SELECTDOWN) { m->vm.mute[n] = !m->vm.mute[n]; m->vm.solo = -1; vm_send(&m->vm); }
                else if (code == MENUDOWN) { m->vm.solo = m->vm.solo == n ? -1 : n; vm_send(&m->vm); }
                break;
            }
            case IDCMP_GADGETDOWN: case IDCMP_GADGETUP: case IDCMP_MOUSEMOVE:
                switch (gad->GadgetID) {
                case G_REFRESH: refresh = code; break;
                case G_VOL:
                    if (62 - code != db) {
                        db = 62 - code;
                        vol_text(voltxt, db);
                        GT_SetGadgetAttrs(g_voltxt, win, NULL, GTTX_Text, (ULONG)voltxt, TAG_DONE);
                        hifivol_send(db, mute);
                    }
                    break;
                case G_MUTE:
                    mute = (gad->Flags & GFLG_SELECTED) != 0;
                    hifivol_send(db, mute);
                    break;
                case G_TEST:
                    m->t.kind = code;
                    if (m->t.open) test_reset(&m->t);
                    break;
                case G_LEVEL: m->t.level = code; break;
                case G_PLAY: m->rec_msg[0] = 0; test_start(&m->t, m->qsin); break;
                case G_STOP: test_stop(&m->t); break;
                case G_EQ:
                    if (eqw) eqw_front(eqw);
                    else {
                        eq_load(&eqs);
                        eqw = eqw_open(win->WScreen, &eqs);
                    }
                    break;
                case G_VOICES:
                    if (vmw) vmw_front(vmw);
                    else vmw = vmw_open(win->WScreen, &m->vm);
                    break;
                case G_VIEW: m->view = code; m->vp_w = 0; break;
                case G_REC:
                    if (!m->rec.on) {
                        char e[96];
                        if (!m->playing) { strcpy(m->rec_msg, "nothing plays on HDMI to record"); break; }
                        if (rec_start(&m->rec, NULL, ring->rate, (ring->vstate & 1) != 0, ring->vrate, e, sizeof e)) {
                            m->rec.rd = ring->wr;
                            m->rec.vrd = ring->vwr;
                            m->rec_msg[0] = 0;
                        } else snprintf(m->rec_msg, sizeof m->rec_msg, "not recording: %s", e);
                    }
                    break;
                case G_RECSTOP:
                    if (m->rec.on) {
                        ULONG secs = m->rec.rate ? m->rec.bytes[0] / (m->rec.rate * 4) : 0;
                        rec_poll(m);
                        rec_stop(&m->rec);
                        snprintf(m->rec_msg, sizeof m->rec_msg, "kept %s%s, %02lu:%02lu", m->rec.path,
                                 m->rec.voices ? " and its 4 voices" : "", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
                    }
                    break;
                case G_FOLDER: {
                    char dir[128];
                    if (rec_ask_dir(win, dir, sizeof dir)) snprintf(m->rec_msg, sizeof m->rec_msg, "recordings go to %s", dir);
                    break;
                }
                }
                break;
            }
        }
    }
    if (db != db0 || mute != mute0) hifivol_store(db, mute, 1);   /* kept, as a mixer keeps it */
out:
    if (eqw) {                                     /* closes with me: kept until the next reboot, as Use */
        eq_store(&eqs, 0);
        eqw_close(eqw);
    }
    if (vmw) {
        vm_store(&m->vm, 0);
        vmw_close(vmw);
    }
    if (m) {
        int clicked = m->vm.solo >= 0;             /* the clicks' mutes and solo go with the window */
        for (int n = 0; n < 4; n++) if (m->vm.mute[n]) clicked = 1;
        if (clicked) {
            memset(m->vm.mute, 0, 4);
            m->vm.solo = -1;
            vm_send(&m->vm);
        }
        if (m->rec.on) { rec_poll(m); rec_stop(&m->rec); }
        test_stop(&m->t);
    }
    if (lent) AGA_CTRL = 0x5160;                   /* returns once no feed writes the ring */
    if (timer_busy) {
        if (!CheckIO((struct IORequest *)treq)) AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
    }
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (bm) { WaitBlit(); FreeBitMap(bm); }
    if (font) CloseFont(font);
    if (pens) for (int i = 0; i < N_PENS; i++) if (own[i] >= 0) ReleasePen(scr->ViewPort.ColorMap, own[i]);
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    if (pub) { RemPort(pub); DeleteMsgPort(pub); }
    if (ring) FreeVec(ring);
    if (m) FreeVec(m);
    if (opened_gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (opened_gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (opened_intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    return rc;
}
