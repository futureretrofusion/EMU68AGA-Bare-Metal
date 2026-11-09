/* agaboot - Chipset menu - Sound: the emulated chipset's sound on HDMI, its
 * filter and volume, and the stereo of both outputs (the Amiga's own and
 * HDMI). The kernel's side is arm/aga_mix.h (width, swap, volume),
 * arm/aga_hdmi_filter.h (the filters) and arm/aga_hdmi.c.
 *
 *   ENVARC:AGA_HDMIVOL   100 150 200 300 400 (200 when not set: 1.113's loudness)
 *   ENVARC:AGA_STEREO    100 75 50 25 0      (100 when not set: the Amiga's own)
 *   ENVARC:AGA_SWAP      1                   (left and right as they are when not set)
 *   ENVARC:AGA_AHI16     ON | LOWLATENCY     (HiFi sound: AHI programs on HDMI, ahi16.c)
 *   ENVARC:AGA_HDMIPAULA BANDLIMITED         (Paula on HDMI band-limited; classic when not set)
 *   ENVARC:AGA_AUDIORATE ANY 96 192          (HDMI sound rate; the screen's EDID when not set)
 *   ENVARC:AGA_HIFIVOL   -12, -12 MUTE        (HiFi volume, dB; 0 dB when not set - the Sound monitor's)
 *   ENVARC:AGA_HDMIFILTER, ENVARC:AGA_HDMISOUND: agaboot.c's
 *
 * Words to the kernel at $DFF1F2, n = bit 0 | bits 2-3 >> 1 so that bit 1
 * stays clear (an older kernel keeps an unknown word in the core's aga_ctrl,
 * where bit 1 means "windowed"): $5840 + n HDMI volume, $5850 + n stereo,
 * $5860 / $5861 swap, $5899 / $5898 Paula band-limited / classic (arm/aga_blep.h),
 * $5868 + (m & 1 | (m & 2) << 1) HDMI sound rate: follow (EDID), follow (any), 96, 192 kHz,
 * $5180 + (d & 1 | (d & 62) << 1) HiFi volume, d dB down, 63 silence (arm/aga_hdmi.c).
 * A stock Emu68 ignores them, which is about the only thing it does right.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sound.h"
#include "ahi16.h"
#include "monitor.h"
#include "eq.h"
#include "vm.h"
#include "front.h"

#define AGA_CTRL (*(volatile UWORD *)0xDFF1F2)

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c */

#define VOL_VAR    "AGA_HDMIVOL"
#define STEREO_VAR "AGA_STEREO"
#define SWAP_VAR   "AGA_SWAP"
#define HDMI_VAR   "AGA_HDMISOUND"
#define FILT_VAR   "AGA_HDMIFILTER"
#define PAULA_VAR  "AGA_HDMIPAULA"
#define RATE_VAR   "AGA_AUDIORATE"
#define HIFI_VAR   "AGA_HIFIVOL"

static const int vol_pc[5] = { 100, 150, 200, 300, 400 };
static const int width_pc[5] = { 100, 75, 50, 25, 0 };
static const char *const vol_names[] = { "100 %", "150 %", "200 %", "300 %", "400 %", NULL };
static const char *const width_names[] = { "Amiga", "75 %", "50 %", "25 %", "Mono", NULL };

const char *sound_vol_name(int i)   { return (i >= 0 && i < 5) ? vol_names[i] : vol_names[2]; }
const char *sound_width_name(int i) { return (i >= 0 && i < 5) ? width_names[i] : width_names[0]; }

static int enc(int i) { return (i & 1) | ((i & 6) << 1); }

int sound_parse_vol(const char *w)
{
    int v = atoi(w);
    for (int i = 0; i < 5; i++) if (v == vol_pc[i]) return i;
    return -1;
}

/* SCREEN, ANY, 96, 192 -> 0..3, -1 */
int sound_parse_rate(const char *w)
{
    if (!stricmp(w, "SCREEN")) return 0;
    if (!stricmp(w, "ANY")) return 1;
    if (!stricmp(w, "96")) return 2;
    if (!stricmp(w, "192")) return 3;
    return -1;
}

static const char *const rate_names[] = { "Follow screen", "Follow, any", "96 kHz", "192 kHz", NULL };
const char *sound_rate_name(int i) { return (i >= 0 && i < 4) ? rate_names[i] : rate_names[0]; }

int sound_parse_width(const char *w)
{
    if (!stricmp(w, "AMIGA")) return 0;
    if (!stricmp(w, "MONO")) return 4;
    if (!*w || ((*w < '0' || *w > '9'))) return -1;
    int v = atoi(w);
    for (int i = 0; i < 5; i++) if (v == width_pc[i]) return i;
    return -1;
}

static int var_int(const char *name, int dflt)
{
    char v[16];
    if (GetVar((STRPTR)name, (STRPTR)v, sizeof v, 0) <= 0) return dflt;
    return atoi(v);
}

void sound_load(struct sound_prefs *s)
{
    char v[16];
    s->hdmi = GetVar((STRPTR)HDMI_VAR, (STRPTR)v, sizeof v, 0) > 0 && v[0] == '1';
    s->filter = hdmifilter_pref();
    s->vol = 2;
    int pc = var_int(VOL_VAR, 200);
    for (int i = 0; i < 5; i++) if (pc == vol_pc[i]) s->vol = i;
    s->width = 0;
    pc = var_int(STEREO_VAR, 100);
    for (int i = 0; i < 5; i++) if (pc == width_pc[i]) s->width = i;
    s->swap = var_int(SWAP_VAR, 0) == 1;
    s->ahi16 = ahi16_pref();
    s->paula = GetVar((STRPTR)PAULA_VAR, (STRPTR)v, sizeof v, 0) > 0 && !stricmp(v, "BANDLIMITED");
    s->rate = 0;
    if (GetVar((STRPTR)RATE_VAR, (STRPTR)v, sizeof v, 0) > 0) s->rate = sound_parse_rate(v) > 0 ? sound_parse_rate(v) : 0;
}

/* name=value in ENV: (and ENVARC: when save), or gone when value is NULL */
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

void sound_store_mix(const struct sound_prefs *s, int save)
{
    char v[8];
    put_var(FILT_VAR, s->filter == 1 ? "A500" : s->filter == 2 ? "A1200" : NULL, save);
    sprintf(v, "%d", vol_pc[s->vol]);
    put_var(VOL_VAR, s->vol == 2 ? NULL : v, save);           /* not set: 200 % */
    sprintf(v, "%d", width_pc[s->width]);
    put_var(STEREO_VAR, s->width == 0 ? NULL : v, save);      /* not set: the Amiga's */
    put_var(SWAP_VAR, s->swap ? "1" : NULL, save);
    put_var(PAULA_VAR, s->paula ? "BANDLIMITED" : NULL, save);   /* not set: classic */
    put_var(RATE_VAR, s->rate == 1 ? "ANY" : s->rate == 2 ? "96" : s->rate == 3 ? "192" : NULL, save);   /* not set: the EDID's */
}

/* ---- HiFi volume ---------------------------------------------------------------- */
int hifivol_pref(int *mute)
{
    char v[24];
    *mute = 0;
    if (GetVar((STRPTR)HIFI_VAR, (STRPTR)v, sizeof v, 0) <= 0) return 0;
    int db = 0;
    hifivol_parse(v, &db, mute);
    return db;
}

/* "-12", "12" (down either way), "0", "MUTE", "UNMUTE", or "-12 MUTE" */
int hifivol_parse(const char *w, int *db, int *mute)
{
    int ok = 0;
    while (*w == ' ') w++;
    if (*w == '-' || *w == '+' || (*w >= '0' && *w <= '9')) {
        int v = atoi(w);
        if (v < 0) v = -v;
        if (v > 62) v = 62;
        *db = v;
        *mute = 0;
        ok = 1;
        while (*w && *w != ' ') w++;
        while (*w == ' ') w++;
    }
    if (!stricmp(w, "MUTE")) { *mute = 1; ok = 1; }
    else if (!stricmp(w, "UNMUTE")) { *mute = 0; ok = 1; }
    else if (*w) ok = 0;
    return ok;
}

void hifivol_store(int db, int mute, int save)
{
    char v[24];
    if (db) sprintf(v, "-%d%s", db, mute ? " MUTE" : "");
    else strcpy(v, "0 MUTE");
    put_var(HIFI_VAR, db || mute ? v : NULL, save);  /* not set: 0 dB */
}

void hifivol_send(int db, int mute)
{
    int d = mute ? 63 : db < 0 ? 0 : db > 62 ? 62 : db;
    AGA_CTRL = (UWORD)(0x5180 | (d & 1) | ((d & 62) << 1));
}

void sound_send_mix(const struct sound_prefs *s)
{
    AGA_CTRL = (UWORD)(0x5840 | enc(s->vol));
    AGA_CTRL = (UWORD)(0x5850 | enc(s->width));
    AGA_CTRL = (UWORD)(0x5860 | (s->swap ? 1 : 0));
    AGA_CTRL = (UWORD)(0x5898 | (s->paula ? 1 : 0));
    AGA_CTRL = (UWORD)(0x5868 | (s->rate & 1) | ((s->rate & 2) << 1));
}

/* ---- the window ---------------------------------------------------------------
   As the RGB port's: cycle gadgets, then Save (ENVARC:, for good), Use (ENV:,
   until the next reboot) or Cancel. Paula's path, the rate, filter, volume, stereo and
   swap are sent as they change, so they can be heard while the window is
   open; Cancel and the close gadget send the old ones back. HDMI on/off is agaboot.c's, after
   the window: it changes config.txt and waits for the kernel. HiFi sound
   too: it writes DEVS: and AHI's settings (ahi16.c), and is saved either way.
   Monitor... opens the Sound monitor (monitor.c) over it, with the HiFi volume;
   Equalizer... the effects (eq.c), which keep themselves. Fiddly as hell,
   but it works. */
enum { G_HDMI = 1, G_AHI16, G_PAULA, G_RATE, G_FILTER, G_VOL, G_WIDTH, G_SWAP, G_SAVE, G_USE, G_CANCEL, G_MONITOR, G_EQ, G_VOICES };
#define N_CYCLES 8

static int widest(struct RastPort *rp, const char *const *s)
{
    int w = 0;
    for (; *s; s++) {
        int t = TextLength(rp, (STRPTR)*s, strlen(*s));
        if (t > w) w = t;
    }
    return w;
}

int sound_window(struct sound_prefs *s)
{
    static const char *const hdmi_names[] = { "Off", "On", NULL };
    static const char *const ahi_names[] = { "Off", "On", "On, low latency", NULL };
    static const char *const filter_names[] = { "Off", "A500 (fixed + LED)", "A1200 (LED only)", NULL };
    static const char *const swap_names[] = { "As they are", "Swapped", NULL };
    static const char *const paula_names[] = { "Classic", "Band-limited", NULL };
    static const char *const labels[] = { "Sound on HDMI", "HiFi sound (AHI)", "Paula on HDMI", "Sound rate (HDMI)", "Filter (HDMI)", "Volume (HDMI)", "Stereo",
                                          "Left and right", NULL };
    static const char *const buttons[] = { "Save", "Use", "Cancel", NULL };
    struct sound_prefs cur = *s, orig = *s;
    struct Screen *scr = NULL;
    APTR vi = NULL;
    struct Gadget *glist = NULL, *g;
    struct Window *win = NULL;
    int opened_intui = 0, opened_gfx = 0, opened_gt = 0, rc = 0;

    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37))) opened_intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37))) opened_gfx = 1;
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37))) opened_gt = 1;
    if (!IntuitionBase || !GfxBase || !GadToolsBase) goto out;
    if (!(scr = LockPubScreen(NULL))) goto out;
    if (!(vi = GetVisualInfo(scr, TAG_DONE))) goto out;

    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize;
    int lw = widest(srp, labels) + 12;
    int cw = widest(srp, filter_names), t;
    if ((t = widest(srp, width_names)) > cw) cw = t;
    if ((t = widest(srp, swap_names)) > cw) cw = t;
    if ((t = widest(srp, ahi_names)) > cw) cw = t;
    cw += 40;                                        /* the cycle glyph */
    int bw = widest(srp, buttons) + 24;
    int gh = fh + 6, gap = 4;
    int left = scr->WBorLeft + 12, top = scr->WBorTop + fh + 1 + 10;
    int iw = lw + cw + 24;
    if (3 * bw + 2 * 12 + 24 > iw) iw = 3 * bw + 2 * 12 + 24;
    int tools_w = TextLength(srp, (STRPTR)"Monitor...", 10) + TextLength(srp, (STRPTR)"Equalizer...", 12)
                + TextLength(srp, (STRPTR)"Voices...", 9) + 3 * 24 + 2 * 8;   /* the row of the three windows */
    if (tools_w + 24 > iw) iw = tools_w + 24;

    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_Height = gh;
    g = CreateContext(&glist);
    const char *const *names[N_CYCLES] = { hdmi_names, ahi_names, paula_names, rate_names, filter_names, vol_names, width_names, swap_names };
    int active[N_CYCLES] = { cur.hdmi, cur.ahi16, cur.paula, cur.rate, cur.filter, cur.vol, cur.width, cur.swap };
    for (int i = 0; i < N_CYCLES; i++) {
        ng.ng_LeftEdge = left + lw;
        ng.ng_TopEdge = top + i * (gh + gap);
        ng.ng_Width = cw;
        ng.ng_GadgetText = (UBYTE *)labels[i];
        ng.ng_GadgetID = G_HDMI + i;
        ng.ng_Flags = PLACETEXT_LEFT;
        g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)names[i], GTCY_Active, active[i], TAG_DONE);
    }
    int my = top + N_CYCLES * (gh + gap) + 6;
    ng.ng_LeftEdge = left;
    ng.ng_TopEdge = my;
    ng.ng_Width = TextLength(srp, (STRPTR)"Monitor...", 10) + 24;
    ng.ng_GadgetText = (UBYTE *)"Monitor...";
    ng.ng_GadgetID = G_MONITOR;
    ng.ng_Flags = PLACETEXT_IN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    ng.ng_LeftEdge += ng.ng_Width + 8;
    ng.ng_Width = TextLength(srp, (STRPTR)"Equalizer...", 12) + 24;
    ng.ng_GadgetText = (UBYTE *)"Equalizer...";
    ng.ng_GadgetID = G_EQ;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    ng.ng_LeftEdge += ng.ng_Width + 8;
    ng.ng_Width = TextLength(srp, (STRPTR)"Voices...", 9) + 24;
    ng.ng_GadgetText = (UBYTE *)"Voices...";
    ng.ng_GadgetID = G_VOICES;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    int by = my + gh + 12;
    int bx[3] = { left, left + (iw - 24 - bw) / 2, left + iw - 24 - bw };
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = bx[i];
        ng.ng_TopEdge = by;
        ng.ng_Width = bw;
        ng.ng_GadgetText = (UBYTE *)buttons[i];
        ng.ng_GadgetID = G_SAVE + i;
        ng.ng_Flags = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    if (!g) goto out;

    int ih = by + gh + 10 - (scr->WBorTop + fh + 1);
    win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - Sound",
        WA_Gadgets, (ULONG)glist,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_Left, scr->Width > iw ? (scr->Width - iw) / 2 : 0,
        WA_Top, scr->Height > ih ? (scr->Height - ih) / 2 : 0,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE,
        WA_IDCMP, CYCLEIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW,
        TAG_DONE);
    if (!win) goto out;
    GT_RefreshWindow(win, NULL);

    for (int done = 0; !done; ) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        while (!done && (m = GT_GetIMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *gad = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
            } else if (cls == IDCMP_CLOSEWINDOW) {
                done = 1;
            } else if (cls == IDCMP_GADGETUP) {
                switch (gad->GadgetID) {
                case G_HDMI:   cur.hdmi = code; break;          /* after the window */
                case G_AHI16:  cur.ahi16 = code; break;         /* after the window too */
                case G_PAULA:  cur.paula = code; sound_send_mix(&cur); break;
                case G_RATE:   cur.rate = code; sound_send_mix(&cur); break;
                case G_FILTER: cur.filter = code; hdmifilter_send(code); break;
                case G_VOL:    cur.vol = code; sound_send_mix(&cur); break;
                case G_WIDTH:  cur.width = code; sound_send_mix(&cur); break;
                case G_SWAP:   cur.swap = code; sound_send_mix(&cur); break;
                case G_SAVE:   rc = 2; done = 1; break;
                case G_USE:    rc = 1; done = 1; break;
                case G_CANCEL: done = 1; break;
                /* Voices, Equalizer and Monitor: each a process of its own, so
                   they stay open together and beside this window (front.c);
                   in this one only when that cannot be started */
                case G_VOICES:
                    if (!front_spawn("C:agaboot SOUND VOICES ASK")) {
                        struct vm_set vs;
                        vm_load(&vs);
                        SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
                        vm_window(&vs);                /* stores what it keeps itself */
                        SetWindowPointer(win, TAG_DONE);
                    }
                    break;
                case G_EQ:
                    if (!front_spawn("C:agaboot SOUND EQ ASK")) {
                        struct eq_set es;
                        eq_load(&es);
                        SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
                        eq_window(&es);                /* stores what it keeps itself */
                        SetWindowPointer(win, TAG_DONE);
                    }
                    break;
                case G_MONITOR:
                    if (!front_spawn("C:agaboot SOUND MONITOR")) {
                        static char mmsg[160];
                        SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
                        int r = monitor_window(mmsg, sizeof mmsg);
                        SetWindowPointer(win, TAG_DONE);
                        if (r && mmsg[0]) {
                            struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM - Sound",
                                                     (UBYTE *)"%s", (UBYTE *)"OK" };
                            EasyRequest(win, &es, NULL, (ULONG)mmsg);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (rc) *s = cur;
    else {                                              /* Cancel: what was there */
        hdmifilter_send(orig.filter);
        sound_send_mix(&orig);
    }
out:
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    if (opened_gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (opened_gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (opened_intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    return rc;
}
