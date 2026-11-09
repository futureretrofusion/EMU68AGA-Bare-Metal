/* agaboot - Chipset menu - Sound - Equalizer: where I get the last fucking
 * word on sound over HDMI (arm/aga_fx.h) - a ten-band equalizer with its
 * preamp and presets, headphone crossfeed, a limiter and balance. Changes
 * are heard at once, as the Sound window's are; Save keeps them in ENVARC:,
 * Use until the next reboot, Cancel and the close gadget send the old ones
 * back.
 *
 *   ENVARC:AGA_EQ        ON|OFF PRESET=n PRE=dB XF=0-2 LIM=0|1 BAL=dB EQ=g,g,...
 *                        (off and flat when not set; sent at every boot, MENU)
 *   ENVARC:AGA_EQUSER1-3 PRE=dB XF=0-2 EQ=g,g,...  (the user's own presets, Store)
 *
 * The faders are drawn here, not GadTools sliders: drag one with the left
 * mouse button, the right button puts it back to 0 dB. A fader moved after a
 * built-in preset makes it Custom; a User preset changed stays itself until
 * Store keeps it (or another preset is picked).
 *
 * Words at $DFF1F2, two a setting (bits 1 and 15 clear, like every word
 * of mine):
 * $5200 + (v & 1 | (v & 30) << 1) the value, then $5240 + (k & 1 | (k & 14)
 * << 1) gives it to setting k - bands 0-9 and preamp as dB + 12, on, limiter,
 * balance + 12, crossfeed (aga_fx.h's order). A stock Emu68 ignores them.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
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
#include <ctype.h>
#include "eq.h"
#include "front.h"

#define AGA_CTRL (*(volatile UWORD *)0xDFF1F2)

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c */

#define EQ_VAR "AGA_EQ"
enum { K_PRE = 10, K_ON, K_LIM, K_BAL, K_XFEED };

/* ---- presets ------------------------------------------------------------------- */
struct preset { BYTE g[EQ_BANDS]; BYTE pre, xfeed; };
/*                                31  63 125 250 500  1k  2k  4k  8k 16k  pre xf */
static const struct preset builtin[] = {
    { {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },  0, 0 },   /* Custom: never applied */
    { {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },  0, 0 },   /* Flat */
    { {  6,  5,  4,  2,  0,  0,  0,  0,  0,  0 }, -6, 0 },   /* Bass boost */
    { {  0,  0,  0,  0,  0,  0,  1,  3,  5,  6 }, -6, 0 },   /* Treble boost */
    { {  6,  4,  2,  0, -1, -1,  0,  2,  4,  4 }, -6, 0 },   /* Loudness */
    { { -4, -3, -1,  1,  3,  4,  4,  3,  1,  0 }, -4, 0 },   /* Vocal */
    { {  5,  4,  2, -1, -2, -1,  1,  3,  4,  5 }, -5, 0 },   /* Rock */
    { { -1,  1,  3,  4,  3,  0, -1, -1,  0,  1 }, -4, 0 },   /* Pop */
    { {  3,  2,  1,  2, -1, -1,  0,  1,  2,  3 }, -3, 0 },   /* Jazz */
    { {  2,  1,  0,  0,  0,  0, -1, -2, -2, -3 }, -2, 0 },   /* Classical */
    { {  6,  5,  1,  0, -2,  1,  0,  1,  4,  5 }, -6, 0 },   /* Electronic */
    { { -6, -3,  2,  3,  1,  0,  1,  2,  1,  0 }, -3, 0 },   /* TV speakers: nothing they cannot play */
    { {  2,  1,  0,  0,  0,  0,  0,  0,  1,  2 }, -2, 1 },   /* Headphones, with light crossfeed */
    { {  2,  2,  1,  0,  0,  0, -1, -2, -4, -6 }, -2, 0 },   /* Warm */
    { {  0,  0,  0,  0,  0,  1,  2,  3,  4,  4 }, -4, 0 },   /* Bright */
};
#define N_BUILTIN ((int)(sizeof builtin / sizeof builtin[0]))
#define N_USER    3
#define N_PRESETS (N_BUILTIN + N_USER)
static const char *const preset_names[N_PRESETS + 1] = {
    "Custom", "Flat", "Bass boost", "Treble boost", "Loudness", "Vocal", "Rock", "Pop", "Jazz", "Classical",
    "Electronic", "TV speakers", "Headphones", "Warm", "Bright", "User 1", "User 2", "User 3", NULL };

const char *eq_preset_name(int i) { return i >= 0 && i < N_PRESETS ? preset_names[i] : NULL; }

/* names without their spaces, any case: "bassboost", "TVSPEAKERS", "user1" */
int eq_parse_preset(const char *w)
{
    for (int i = 1; i < N_PRESETS; i++) {
        const char *a = preset_names[i], *b = w;
        for (;;) {
            while (*a == ' ') a++;
            while (*b == ' ') b++;
            if (!*a || !*b) break;
            if (toupper((UBYTE)*a) != toupper((UBYTE)*b)) break;
            a++, b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

/* ---- settings ---------------------------------------------------------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* does p begin with key, any case? */
static int key(const char *p, const char *k)
{
    for (; *k; p++, k++) if (toupper((UBYTE)*p) != toupper((UBYTE)*k)) return 0;
    return 1;
}

static void parse_into(struct eq_set *e, const char *s)
{
    for (const char *p = s; *p; ) {
        while (*p == ' ') p++;
        if (key(p, "ON") && (p[2] == ' ' || !p[2])) e->on = 1;
        else if (key(p, "OFF") && (p[3] == ' ' || !p[3])) e->on = 0;
        else if (key(p, "PRESET=")) e->preset = (BYTE)clampi(atoi(p + 7), 0, N_PRESETS - 1);
        else if (key(p, "PRE=")) e->pre = (BYTE)clampi(atoi(p + 4), -12, 12);
        else if (key(p, "XF=")) e->xfeed = (BYTE)clampi(atoi(p + 3), 0, 2);
        else if (key(p, "LIM=")) e->lim = atoi(p + 4) != 0;
        else if (key(p, "BAL=")) e->bal = (BYTE)clampi(atoi(p + 4), -12, 12);
        else if (key(p, "EQ=")) {
            const char *q = p + 3;
            for (int b = 0; b < EQ_BANDS && *q && *q != ' '; b++) {
                e->gain[b] = (BYTE)clampi(atoi(q), -12, 12);
                while (*q && *q != ',' && *q != ' ') q++;
                if (*q == ',') q++;
            }
        }
        while (*p && *p != ' ') p++;
    }
}

void eq_load(struct eq_set *e)
{
    char v[96];
    memset(e, 0, sizeof *e);
    if (GetVar((STRPTR)EQ_VAR, (STRPTR)v, sizeof v, 0) > 0) parse_into(e, v);
}

static void put_var(const char *name, const char *value, int save)
{
    if (value) {
        SetVar((STRPTR)name, (STRPTR)value, -1, GVF_GLOBAL_ONLY);
        if (save) SetVar((STRPTR)name, (STRPTR)value, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

static void fmt_bands(char *o, const BYTE *g)
{
    o[0] = 0;
    for (int b = 0; b < EQ_BANDS; b++) sprintf(o + strlen(o), b ? ",%d" : "%d", g[b]);
}

int eq_is_set(const struct eq_set *e)
{
    if (e->lim || e->bal) return 1;
    if (!e->on) return 0;
    if (e->pre || e->xfeed) return 1;
    for (int b = 0; b < EQ_BANDS; b++) if (e->gain[b]) return 1;
    return 0;
}

void eq_store(const struct eq_set *e, int save)
{
    char v[128], g[48];
    fmt_bands(g, e->gain);
    sprintf(v, "%s PRESET=%d PRE=%d XF=%d LIM=%d BAL=%d EQ=%s", e->on ? "ON" : "OFF", e->preset, e->pre, e->xfeed,
            e->lim, e->bal, g);
    int any = e->on || e->lim || e->bal || e->pre || e->xfeed || e->preset;
    for (int b = 0; b < EQ_BANDS; b++) if (e->gain[b]) any = 1;
    put_var(EQ_VAR, any ? v : NULL, save);          /* not set: off and flat */
}

void eq_preset(struct eq_set *e, int i)
{
    if (i <= 0 || i >= N_PRESETS) return;
    if (i < N_BUILTIN) {
        memcpy(e->gain, builtin[i].g, EQ_BANDS);
        e->pre = builtin[i].pre;
        e->xfeed = builtin[i].xfeed;
    } else {                                           /* the user's own, flat until stored */
        char name[16], v[96];
        struct eq_set u;
        memset(&u, 0, sizeof u);
        sprintf(name, "AGA_EQUSER%d", i - N_BUILTIN + 1);
        if (GetVar((STRPTR)name, (STRPTR)v, sizeof v, 0) > 0) parse_into(&u, v);
        memcpy(e->gain, u.gain, EQ_BANDS);
        e->pre = u.pre;
        e->xfeed = u.xfeed;
    }
    e->on = 1;
    e->preset = (BYTE)i;
}

static void store_user(const struct eq_set *e)
{
    char name[16], v[96], g[48];
    if (e->preset < N_BUILTIN) return;
    fmt_bands(g, e->gain);
    sprintf(name, "AGA_EQUSER%d", e->preset - N_BUILTIN + 1);
    sprintf(v, "PRE=%d XF=%d EQ=%s", e->pre, e->xfeed, g);
    put_var(name, v, 1);
}

void eq_describe(const struct eq_set *e, char *o, int len)
{
    static const char *const xf[3] = { "", ", crossfeed light", ", crossfeed strong" };
    char a[64] = "";
    if (e->on) {
        snprintf(a, sizeof a, "on, %s", e->preset ? preset_names[(int)e->preset] : "Custom");
        if (e->pre) snprintf(a + strlen(a), sizeof a - strlen(a), ", preamp %+d dB", e->pre);
        strncat(a, xf[clampi(e->xfeed, 0, 2)], sizeof a - strlen(a) - 1);
    } else strcpy(a, "off");
    snprintf(o, len, "%s%s", a, e->lim ? ", limiter on" : "");
    if (e->bal) snprintf(o + strlen(o), len - strlen(o), ", balance %s %d dB", e->bal > 0 ? "L" : "R", -abs(e->bal));
}

/* one setting to the kernel: the value, then whose it is */
static void put(int k, int v)
{
    Disable();                                         /* the two words together */
    AGA_CTRL = (UWORD)(0x5200 | (v & 1) | ((v & 30) << 1));
    AGA_CTRL = (UWORD)(0x5240 | (k & 1) | ((k & 14) << 1));
    Enable();
}

static void send_one(const struct eq_set *e, int k)
{
    int v = k < EQ_BANDS ? e->gain[k] + 12 : k == K_PRE ? e->pre + 12 : k == K_ON ? e->on : k == K_LIM ? e->lim
          : k == K_BAL ? e->bal + 12 : e->xfeed;
    put(k, v);
}

void eq_send(const struct eq_set *e)
{
    for (int k = 0; k <= K_XFEED; k++) send_one(e, k);
}

/* ---- the window -------------------------------------------------------------------- */
enum { G_ON = 1, G_PRESET, G_STORE, G_XFEED, G_LIM, G_BAL, G_BALTXT, G_SAVE, G_USE, G_CANCEL };
enum { P_BG, P_GRID, P_TEXT, P_DIM, P_KNOB, P_PRE, P_CURVE, P_OFF, N_PENS };
static const ULONG pen_rgb[N_PENS] = { 0x12161C, 0x323C48, 0xDCE4EC, 0x8A9AAA, 0xF49C3C, 0xE8C434, 0x48D4EC, 0x5A6470 };
static const char *const col_names[EQ_BANDS + 1] = { "Pre", "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const char *const xfeed_names[] = { "Off", "Light", "Strong", NULL };

struct eqw {
    struct front *fr;                              /* one Equalizer: a second open brings this one forward */
    struct eq_set *e, orig;
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist, *g_on, *g_preset, *g_store, *g_xfeed, *g_bal, *g_baltxt;
    struct Window *win;
    struct BitMap *bm;
    struct RastPort brp, *rp;
    struct TextFont *font;
    LONG pen[N_PENS], own[N_PENS];
    int pens;
    int px, py, pw, ph;                            /* the faders' panel in the window */
    int dx, dy;                                    /* where drawing lands in rp: 0,0 off screen, px,py straight */
    int lw, cw, gap, lh, ty0, ty1;                 /* scale's width, a column, the gap after Pre, a line, the track */
    int drag;                                      /* the column being dragged + 1 */
    char baltxt[16];
};

static int tlen(struct RastPort *rp, const char *s) { return TextLength(rp, (STRPTR)s, strlen(s)); }

static int widest(struct RastPort *rp, const char *const *s)
{
    int w = 0;
    for (; *s; s++) if (tlen(rp, *s) > w) w = tlen(rp, *s);
    return w;
}

static int col_x(struct eqw *w, int c) { return w->lw + c * w->cw + (c ? w->gap : 0); }
static int col_val(struct eqw *w, int c) { return c ? w->e->gain[c - 1] : w->e->pre; }
static int val_y(struct eqw *w, int v) { return w->ty0 + (12 - v) * (w->ty1 - w->ty0) / 24; }

static void rf(struct eqw *w, int pen, int x0, int y0, int x1, int y1)
{
    if (x1 < x0 || y1 < y0) return;
    SetAPen(w->rp, w->pen[pen]);
    RectFill(w->rp, w->dx + x0, w->dy + y0, w->dx + x1, w->dy + y1);
}

static void tx(struct eqw *w, int pen, int x, int y, const char *s)
{
    SetAPen(w->rp, w->pen[pen]);
    Move(w->rp, w->dx + x, w->dy + y + w->rp->TxBaseline);
    Text(w->rp, (STRPTR)s, strlen(s));
}

static void draw_panel(struct eqw *w)
{
    static const int marks[5] = { 12, 6, 0, -6, -12 };
    static const char *const mark_s[5] = { "+12", "+6", "0", "-6", "-12" };
    char s[8];
    int on = w->e->on;
    rf(w, P_BG, 0, 0, w->pw - 1, w->ph - 1);
    for (int i = 0; i < 5; i++) {
        int y = val_y(w, marks[i]);
        rf(w, P_GRID, w->lw - 2, y, w->pw - 1, y);
        tx(w, P_DIM, w->lw - 6 - tlen(w->rp, mark_s[i]), y - w->lh / 2 + 1, mark_s[i]);
    }
    int kx[EQ_BANDS + 1], ky[EQ_BANDS + 1];
    for (int c = 0; c <= EQ_BANDS; c++) {
        int x0 = col_x(w, c), mid = x0 + w->cw / 2, v = col_val(w, c), y = val_y(w, v);
        rf(w, P_GRID, mid - 1, w->ty0, mid, w->ty1);
        sprintf(s, v > 0 ? "+%d" : "%d", v);
        tx(w, on ? P_TEXT : P_DIM, mid - tlen(w->rp, s) / 2, 0, s);
        tx(w, P_DIM, mid - tlen(w->rp, col_names[c]) / 2, w->ty1 + 6, col_names[c]);
        kx[c] = mid;
        ky[c] = y;
    }
    if (on) {                                      /* the curve through the bands' knobs */
        SetAPen(w->rp, w->pen[P_CURVE]);
        Move(w->rp, w->dx + kx[1], w->dy + ky[1]);
        for (int c = 2; c <= EQ_BANDS; c++) Draw(w->rp, w->dx + kx[c], w->dy + ky[c]);
    }
    for (int c = 0; c <= EQ_BANDS; c++) {
        int x0 = col_x(w, c);
        rf(w, !on ? P_OFF : c ? P_KNOB : P_PRE, x0 + 5, ky[c] - 3, x0 + w->cw - 6, ky[c] + 3);
    }
    if (w->bm) BltBitMapRastPort(w->bm, 0, 0, w->win->RPort, w->px, w->py, w->pw, w->ph, 0xC0);
}

static void bal_text(struct eqw *w)
{
    int b = w->e->bal;
    if (!b) strcpy(w->baltxt, "Centre");
    else sprintf(w->baltxt, "%s -%d dB", b > 0 ? "L" : "R", b > 0 ? b : -b);
    GT_SetGadgetAttrs(w->g_baltxt, w->win, NULL, GTTX_Text, (ULONG)w->baltxt, TAG_DONE);
}

/* the Store button only for the user's own presets; the checkbox and
   crossfeed as the settings now are */
static void sync_gadgets(struct eqw *w)
{
    GT_SetGadgetAttrs(w->g_on, w->win, NULL, GTCB_Checked, w->e->on, TAG_DONE);
    GT_SetGadgetAttrs(w->g_preset, w->win, NULL, GTCY_Active, w->e->preset, TAG_DONE);
    GT_SetGadgetAttrs(w->g_xfeed, w->win, NULL, GTCY_Active, w->e->xfeed, TAG_DONE);
    GT_SetGadgetAttrs(w->g_store, w->win, NULL, GA_Disabled, w->e->preset < N_BUILTIN, TAG_DONE);
}

/* a change to the equalizer's part: on with it, and a built-in preset
   becomes Custom */
static void touched(struct eqw *w)
{
    int was_on = w->e->on, was = w->e->preset;
    if (!w->e->on) { w->e->on = 1; send_one(w->e, K_ON); }
    if (w->e->preset < N_BUILTIN) w->e->preset = 0;
    if (!was_on || was != w->e->preset) sync_gadgets(w);
}

static void set_col(struct eqw *w, int c, int v)
{
    v = clampi(v, -12, 12);
    if (v == col_val(w, c) && w->e->on) return;
    if (c) w->e->gain[c - 1] = (BYTE)v;
    else w->e->pre = (BYTE)v;
    touched(w);
    send_one(w->e, c ? c - 1 : K_PRE);
    draw_panel(w);
}

/* the column under the mouse (window coordinates), -1 none */
static int hit(struct eqw *w, int mx, int my)
{
    int x = mx - w->px, y = my - w->py;
    if (y < 0 || y >= w->ph) return -1;
    for (int c = 0; c <= EQ_BANDS; c++) {
        int x0 = col_x(w, c);
        if (x >= x0 && x < x0 + w->cw) return c;
    }
    return -1;
}

static int y_val(struct eqw *w, int my)
{
    int y = my - w->py, span = w->ty1 - w->ty0;
    return 12 - ((y - w->ty0) * 24 + span / 2) / span;
}

struct eqw *eqw_open(struct Screen *scr, struct eq_set *e)
{
    int other;
    struct front *fr = front_claim("AGA-PISTORM Equalizer", &other);
    if (other) return NULL;                        /* open already: brought to the front */
    struct eqw *w = AllocVec(sizeof *w, MEMF_ANY | MEMF_CLEAR);
    if (!w) { front_release(fr); return NULL; }
    w->fr = fr;
    w->e = e;
    w->orig = *e;
    w->scr = scr;
    if (!(w->vi = GetVisualInfo(scr, TAG_DONE))) goto fail;
    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize, gh = fh + 6;
    w->lh = fh + 2;
    w->lw = tlen(srp, "+12") + 10;
    w->cw = tlen(srp, "+12") + 12;
    if (w->cw < 26) w->cw = 26;
    w->gap = 10;
    w->pw = col_x(w, EQ_BANDS + 1) - w->cw + w->cw + 4;
    /* rows: the top one, the faders, crossfeed/limiter/balance, the buttons */
    int l_pre = tlen(srp, "Preset") + 8, c_pre = widest(srp, preset_names) + 36, b_st = tlen(srp, "Store") + 20;
    int row0 = 26 + tlen(srp, "Equalizer") + 6 + 16 + l_pre + c_pre + 8 + b_st;
    int l_xf = tlen(srp, "Crossfeed") + 8, c_xf = widest(srp, xfeed_names) + 36, l_lim = tlen(srp, "Limiter") + 6;
    int l_bal = tlen(srp, "Balance") + 8, s_bal = 96, t_bal = tlen(srp, "R -12 dB") + 8;
    int row2 = l_xf + c_xf + 16 + 26 + l_lim + 16 + l_bal + s_bal + 4 + t_bal;
    static const char *const buttons[] = { "Save", "Use", "Cancel", NULL };
    int bw = widest(srp, buttons) + 24;
    int iw = w->pw;
    if (row0 > iw) iw = row0;
    if (row2 > iw) iw = row2;
    if (3 * bw + 24 > iw) iw = 3 * bw + 24;
    int title = scr->WBorTop + fh + 1;
    int avail = scr->Height - title - scr->WBorBottom - (6 + gh + 8 + 8 + gh + 8 + gh + 8) - (w->lh + 4 + 6 + w->lh) - 8;
    int th = avail > 144 ? 144 : avail < 48 ? 48 : avail;
    th -= th % 24;                                   /* whole dB steps */
    w->ty0 = w->lh + 4;
    w->ty1 = w->ty0 + th;
    w->ph = w->ty1 + 6 + w->lh;

    int left = scr->WBorLeft + 10, y0 = title + 6;
    w->px = left + (iw - w->pw) / 2;
    w->py = y0 + gh + 8;
    int y2 = w->py + w->ph + 8, y3 = y2 + gh + 8;
    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = w->vi;
    ng.ng_Height = gh;
    struct Gadget *g = CreateContext(&w->glist);
    int x = left;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y0; ng.ng_Width = 26;
    ng.ng_GadgetText = (UBYTE *)"Equalizer"; ng.ng_GadgetID = G_ON; ng.ng_Flags = PLACETEXT_RIGHT;
    g = w->g_on = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, e->on, GTCB_Scaled, TRUE, TAG_DONE);
    x += 26 + tlen(srp, "Equalizer") + 6 + 16;
    ng.ng_LeftEdge = x + l_pre; ng.ng_Width = c_pre;
    ng.ng_GadgetText = (UBYTE *)"Preset"; ng.ng_GadgetID = G_PRESET; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_preset = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)preset_names, GTCY_Active, e->preset, TAG_DONE);
    x += l_pre + c_pre + 8;
    ng.ng_LeftEdge = x; ng.ng_Width = b_st;
    ng.ng_GadgetText = (UBYTE *)"Store"; ng.ng_GadgetID = G_STORE; ng.ng_Flags = PLACETEXT_IN;
    g = w->g_store = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, e->preset < N_BUILTIN, TAG_DONE);

    x = left;
    ng.ng_LeftEdge = x + l_xf; ng.ng_TopEdge = y2; ng.ng_Width = c_xf;
    ng.ng_GadgetText = (UBYTE *)"Crossfeed"; ng.ng_GadgetID = G_XFEED; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_xfeed = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)xfeed_names, GTCY_Active, e->xfeed, TAG_DONE);
    x += l_xf + c_xf + 16;
    ng.ng_LeftEdge = x; ng.ng_Width = 26;
    ng.ng_GadgetText = (UBYTE *)"Limiter"; ng.ng_GadgetID = G_LIM; ng.ng_Flags = PLACETEXT_RIGHT;
    g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, e->lim, GTCB_Scaled, TRUE, TAG_DONE);
    x += 26 + l_lim + 16;
    ng.ng_LeftEdge = x + l_bal; ng.ng_Width = s_bal;
    ng.ng_GadgetText = (UBYTE *)"Balance"; ng.ng_GadgetID = G_BAL; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_bal = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 0, GTSL_Max, 24, GTSL_Level, e->bal + 12,
                                GA_RelVerify, TRUE, GA_Immediate, TRUE, TAG_DONE);
    x += l_bal + s_bal + 4;
    strcpy(w->baltxt, "Centre");
    if (e->bal) sprintf(w->baltxt, "%s -%d dB", e->bal > 0 ? "L" : "R", e->bal > 0 ? e->bal : -e->bal);
    ng.ng_LeftEdge = x; ng.ng_Width = t_bal;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_BALTXT;
    g = w->g_baltxt = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)w->baltxt, TAG_DONE);

    int bx[3] = { left, left + (iw - bw) / 2, left + iw - bw };
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = bx[i]; ng.ng_TopEdge = y3; ng.ng_Width = bw;
        ng.ng_GadgetText = (UBYTE *)buttons[i]; ng.ng_GadgetID = G_SAVE + i; ng.ng_Flags = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    if (!g) goto fail;

    int ih = y3 + gh + 8 - title, ww = iw + 20;
    w->win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - Equalizer",
        WA_Gadgets, (ULONG)w->glist,
        WA_InnerWidth, ww, WA_InnerHeight, ih,
        WA_Left, scr->Width > ww ? (scr->Width - ww) / 2 : 0,
        WA_Top, scr->BarHeight + 2,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE, WA_RMBTrap, TRUE,
        WA_IDCMP, CYCLEIDCMP | CHECKBOXIDCMP | SLIDERIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                  IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY,
        TAG_DONE);
    if (!w->win) goto fail;
    GT_RefreshWindow(w->win, NULL);
    front_set(w->fr, w->win);

    struct ColorMap *cm = scr->ViewPort.ColorMap;
    for (int i = 0; i < N_PENS; i++) {
        ULONG c = pen_rgb[i];
        w->own[i] = ObtainBestPen(cm, ((c >> 16) & 255) * 0x01010101UL, ((c >> 8) & 255) * 0x01010101UL,
                                  (c & 255) * 0x01010101UL, OBP_Precision, PRECISION_GUI, TAG_DONE);
        w->pen[i] = w->own[i] >= 0 ? w->own[i] : i == P_BG ? 1 : 2;
    }
    w->pens = 1;
    w->font = OpenFont(scr->Font);
    w->bm = AllocBitMap(w->pw, w->ph, GetBitMapAttr(w->win->RPort->BitMap, BMA_DEPTH), 0, w->win->RPort->BitMap);
    if (w->bm) {
        InitRastPort(&w->brp);
        w->brp.BitMap = w->bm;
        w->rp = &w->brp;
    } else {
        w->rp = w->win->RPort;
        w->dx = w->px;
        w->dy = w->py;
    }
    if (w->font) SetFont(w->rp, w->font);
    SetDrMd(w->rp, JAM1);
    draw_panel(w);
    return w;
fail:
    eqw_close(w);
    return NULL;
}

ULONG eqw_sigs(struct eqw *w) { return 1UL << w->win->UserPort->mp_SigBit; }

void eqw_front(struct eqw *w) { WindowToFront(w->win); ActivateWindow(w->win); }

int eqw_handle(struct eqw *w)
{
    struct IntuiMessage *im;
    int rc = 0;
    while (!rc && (im = GT_GetIMsg(w->win->UserPort))) {
        ULONG cls = im->Class;
        UWORD code = im->Code;
        WORD mx = im->MouseX, my = im->MouseY;
        struct Gadget *gad = (struct Gadget *)im->IAddress;
        GT_ReplyIMsg(im);
        switch (cls) {
        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(w->win);
            GT_EndRefresh(w->win, TRUE);
            draw_panel(w);
            break;
        case IDCMP_CLOSEWINDOW:
            rc = 1;
            break;
        case IDCMP_VANILLAKEY:
            if (code == 27) rc = 1;
            break;
        case IDCMP_MOUSEBUTTONS: {
            int c = hit(w, mx, my);
            if (code == SELECTDOWN && c >= 0) {
                w->drag = c + 1;
                ReportMouse(TRUE, w->win);
                set_col(w, c, y_val(w, my));
            } else if (code == SELECTUP && w->drag) {
                w->drag = 0;
                ReportMouse(FALSE, w->win);
            } else if (code == MENUDOWN && c >= 0) set_col(w, c, 0);
            break;
        }
        case IDCMP_MOUSEMOVE:
            if (w->drag) set_col(w, w->drag - 1, y_val(w, my));
            else if (gad == w->g_bal && (int)code - 12 != w->e->bal) {
                w->e->bal = (BYTE)((int)code - 12);
                send_one(w->e, K_BAL);
                bal_text(w);
            }
            break;
        case IDCMP_GADGETDOWN: case IDCMP_GADGETUP:
            switch (gad->GadgetID) {
            case G_ON:
                w->e->on = (gad->Flags & GFLG_SELECTED) != 0;
                send_one(w->e, K_ON);
                draw_panel(w);
                break;
            case G_PRESET:
                if (code) {
                    eq_preset(w->e, code);
                    for (int k = 0; k < EQ_BANDS; k++) send_one(w->e, k);
                    send_one(w->e, K_PRE);
                    send_one(w->e, K_XFEED);
                    send_one(w->e, K_ON);
                } else w->e->preset = 0;
                sync_gadgets(w);
                draw_panel(w);
                break;
            case G_STORE:
                store_user(w->e);
                DisplayBeep(w->scr);                   /* stored: a blink to say so */
                break;
            case G_XFEED:
                w->e->xfeed = (BYTE)code;
                touched(w);
                send_one(w->e, K_XFEED);
                break;
            case G_LIM:
                w->e->lim = (gad->Flags & GFLG_SELECTED) != 0;
                send_one(w->e, K_LIM);
                break;
            case G_BAL:
                if ((int)code - 12 != w->e->bal) {
                    w->e->bal = (BYTE)((int)code - 12);
                    send_one(w->e, K_BAL);
                    bal_text(w);
                }
                break;
            case G_SAVE:   rc = 3; break;
            case G_USE:    rc = 2; break;
            case G_CANCEL: rc = 1; break;
            }
            break;
        }
    }
    if (rc == 1) { *w->e = w->orig; eq_send(w->e); }   /* Cancel: what was there */
    else if (rc) eq_store(w->e, rc == 3);
    return rc;
}

void eqw_close(struct eqw *w)
{
    if (!w) return;
    front_release(w->fr);                          /* first: no one brings a closing window forward */
    if (w->drag && w->win) ReportMouse(FALSE, w->win);
    if (w->bm) { WaitBlit(); FreeBitMap(w->bm); }
    if (w->font) CloseFont(w->font);
    if (w->pens) for (int i = 0; i < N_PENS; i++) if (w->own[i] >= 0) ReleasePen(w->scr->ViewPort.ColorMap, w->own[i]);
    if (w->win) CloseWindow(w->win);
    if (w->glist) FreeGadgets(w->glist);
    if (w->vi) FreeVisualInfo(w->vi);
    FreeVec(w);
}

int eq_window(struct eq_set *e)
{
    int opened_intui = 0, opened_gfx = 0, opened_gt = 0, rc = 1;
    struct Screen *scr = NULL;
    struct eqw *w = NULL;
    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39))) opened_intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39))) opened_gfx = 1;
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39))) opened_gt = 1;
    if (IntuitionBase && GfxBase && GadToolsBase && GfxBase->LibNode.lib_Version >= 39 &&
        (scr = LockPubScreen(NULL)) && (w = eqw_open(scr, e))) {
        rc = 0;
        while (!rc) {
            ULONG got = Wait(eqw_sigs(w) | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C) {               /* as Cancel */
                *e = w->orig;
                eq_send(e);
                rc = 1;
            } else rc = eqw_handle(w);
        }
    }
    if (w) eqw_close(w);
    if (scr) UnlockPubScreen(NULL, scr);
    if (opened_gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (opened_gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (opened_intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    return rc - 1;                                      /* 0 Cancel, 1 Use, 2 Save */
}
