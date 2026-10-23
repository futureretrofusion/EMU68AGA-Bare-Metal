/* agaboot - Chipset menu - Sound - Voices: Paula's four voices, each placed
 * anywhere between left and right and at its own level - on HDMI and on the
 * Amiga's own outputs alike (core/src/audio.c mixes them so; arm/aga_blep.h
 * for HDMI's band-limited Paula). Paula wires 0 and 3 to the left and 1 and
 * 2 to the right, hard: fine on speakers apart, tiring on headphones, and
 * no other HDMI sound can do anything about it - it only ever gets the mix.
 * This has the voices themselves. Off - or each voice on its own side at 0
 * dB - the sound is as before, to the bit.
 *
 *   ENVARC:AGA_VOICES  ON|OFF PRESET=n PAN=p,p,p,p LEV=l,l,l,l
 *                      (off when not set; sent at every boot, MENU)
 *
 * The Sound monitor's voice scopes mute a voice with a click and solo it
 * with the right button; those are not kept.
 *
 * Words at $DFF1F2: $5200 + (v & 1 | (v & 30) << 1) a value, then $5260 +
 * (k & 1 | (k & 14) << 1) gives it to setting k: 0-3 voice k's pan + 8, 4-7
 * voice k-4's level, 8 the mixer on (arm/aga_hdmi.c). A stock Emu68 ignores
 * them.
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
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "vm.h"
#include "front.h"

#define AGA_CTRL (*(volatile UWORD *)0xDFF1F2)

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c */

#define VM_VAR "AGA_VOICES"
static const BYTE amiga_pan[4] = { -8, 8, 8, -8 };

/* ---- presets ----------------------------------------------------------------- */
static const BYTE preset_pan[][4] = {
    {  0,  0,  0,  0 },                            /* Custom: never applied */
    { -8,  8,  8, -8 },                            /* Amiga: as Paula wires them */
    { -4,  4,  4, -4 },                            /* Soft stereo */
    { -3,  3,  3, -3 },                            /* Headphones */
    { -6,  6,  2, -2 },                            /* Spread: each voice its own place */
    {  0,  0,  0,  0 },                            /* Centre: mono */
    {  8, -8, -8,  8 },                            /* Swapped */
};
static const char *const preset_names[] = { "Custom", "Amiga", "Soft stereo", "Headphones", "Spread", "Centre", "Swapped", NULL };
#define N_PRESETS ((int)(sizeof preset_pan / sizeof preset_pan[0]))

const char *vm_preset_name(int i) { return i >= 0 && i < N_PRESETS ? preset_names[i] : NULL; }

int vm_parse_preset(const char *w)
{
    for (int i = 1; i < N_PRESETS; i++) {
        const char *a = preset_names[i], *b = w;
        for (;;) {
            while (*a == ' ') a++;
            while (*b == ' ') b++;
            if (!*a || !*b || toupper((UBYTE)*a) != toupper((UBYTE)*b)) break;
            a++, b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

void vm_preset(struct vm_set *v, int i)
{
    if (i <= 0 || i >= N_PRESETS) return;
    for (int n = 0; n < 4; n++) { v->pan[n] = preset_pan[i][n]; v->lev[n] = 16; }
    v->on = 1;
    v->preset = (BYTE)i;
}

/* ---- settings ---------------------------------------------------------------- */
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static int key(const char *p, const char *k)
{
    for (; *k; p++, k++) if (toupper((UBYTE)*p) != toupper((UBYTE)*k)) return 0;
    return 1;
}

static void list(BYTE *o, const char *q, int lo, int hi)
{
    for (int n = 0; n < 4 && *q && *q != ' '; n++) {
        o[n] = (BYTE)clampi(atoi(q), lo, hi);
        while (*q && *q != ',' && *q != ' ') q++;
        if (*q == ',') q++;
    }
}

void vm_load(struct vm_set *v)
{
    char s[96];
    memset(v, 0, sizeof *v);
    for (int n = 0; n < 4; n++) { v->pan[n] = amiga_pan[n]; v->lev[n] = 16; }
    v->preset = 1;
    v->solo = -1;
    if (GetVar((STRPTR)VM_VAR, (STRPTR)s, sizeof s, 0) <= 0) return;
    for (const char *p = s; *p; ) {
        while (*p == ' ') p++;
        if (key(p, "ON") && (p[2] == ' ' || !p[2])) v->on = 1;
        else if (key(p, "OFF") && (p[3] == ' ' || !p[3])) v->on = 0;
        else if (key(p, "PRESET=")) v->preset = (BYTE)clampi(atoi(p + 7), 0, N_PRESETS - 1);
        else if (key(p, "PAN=")) list(v->pan, p + 4, -8, 8);
        else if (key(p, "LEV=")) list(v->lev, p + 4, 0, 16);
        while (*p && *p != ' ') p++;
    }
}

void vm_store(const struct vm_set *v, int save)
{
    char s[96];
    sprintf(s, "%s PRESET=%d PAN=%d,%d,%d,%d LEV=%d,%d,%d,%d", v->on ? "ON" : "OFF", v->preset,
            v->pan[0], v->pan[1], v->pan[2], v->pan[3], v->lev[0], v->lev[1], v->lev[2], v->lev[3]);
    int plain = !v->on && v->preset == 1;
    for (int n = 0; n < 4; n++) if (v->pan[n] != amiga_pan[n] || v->lev[n] != 16) plain = 0;
    if (plain) {
        DeleteVar((STRPTR)VM_VAR, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)VM_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        SetVar((STRPTR)VM_VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
        if (save) SetVar((STRPTR)VM_VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

static void put(int k, int x)
{
    Disable();                                         /* the two words together */
    AGA_CTRL = (UWORD)(0x5200 | (x & 1) | ((x & 30) << 1));
    AGA_CTRL = (UWORD)(0x5260 | (k & 1) | ((k & 14) << 1));
    Enable();
}

/* off with no mute or solo: Paula's wiring. A mute or solo with the mixer
   off still needs it on - at Paula's own pans and full level */
void vm_send(const struct vm_set *v)
{
    int any = v->solo >= 0;
    for (int n = 0; n < 4; n++) if (v->mute[n]) any = 1;
    for (int n = 0; n < 4; n++) {
        int pan = v->on ? v->pan[n] : amiga_pan[n], lev = v->on ? v->lev[n] : 16;
        if (v->solo >= 0 ? n != v->solo : v->mute[n]) lev = 0;
        put(n, pan + 8);
        put(4 + n, lev);
    }
    put(8, v->on || any);
}

const char *vm_pan_text(int p, char *o)
{
    if (p <= -8) strcpy(o, "Left");
    else if (p >= 8) strcpy(o, "Right");
    else if (!p) strcpy(o, "Centre");
    else sprintf(o, "%c %d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
    return o;
}

const char *vm_lev_text(int l, char *o)
{
    if (l <= 0) strcpy(o, "off");
    else if (l >= 16) strcpy(o, "0 dB");
    else sprintf(o, "-%d dB", (16 - l) * 2);
    return o;
}

void vm_describe(const struct vm_set *v, char *o, int len)
{
    if (!v->on) { snprintf(o, len, "off (Paula's own left and right)"); return; }
    char a[12], b[12];
    snprintf(o, len, "on, %s:", v->preset ? preset_names[(int)v->preset] : "Custom");
    for (int n = 0; n < 4; n++)
        snprintf(o + strlen(o), len - strlen(o), " %d %s%s%s", n + 1, vm_pan_text(v->pan[n], a),
                 v->lev[n] < 16 ? " " : "", v->lev[n] < 16 ? vm_lev_text(v->lev[n], b) : "");
}

/* ---- the window -------------------------------------------------------------------- */
enum { G_ON = 1, G_PRESET, G_LEV0, G_LEVT0 = G_LEV0 + 4, G_PAN0 = G_LEVT0 + 4, G_PANT0 = G_PAN0 + 4,
       G_SAVE = G_PANT0 + 4, G_USE, G_CANCEL };

struct vmw {
    struct front *fr;                              /* one voice mixer: a second open brings this one forward */
    struct vm_set *v, orig;
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist, *g_on, *g_preset, *g_lev[4], *g_levt[4], *g_pan[4], *g_pant[4];
    struct Window *win;
    char levt[4][12], pant[4][12];
};

static int tlen(struct RastPort *rp, const char *s) { return TextLength(rp, (STRPTR)s, strlen(s)); }

void vmw_sync(struct vmw *w)
{
    GT_SetGadgetAttrs(w->g_on, w->win, NULL, GTCB_Checked, w->v->on, TAG_DONE);
    GT_SetGadgetAttrs(w->g_preset, w->win, NULL, GTCY_Active, w->v->preset, TAG_DONE);
    for (int n = 0; n < 4; n++) {
        GT_SetGadgetAttrs(w->g_lev[n], w->win, NULL, GTSL_Level, w->v->lev[n], TAG_DONE);
        GT_SetGadgetAttrs(w->g_pan[n], w->win, NULL, GTSL_Level, w->v->pan[n] + 8, TAG_DONE);
        vm_lev_text(w->v->lev[n], w->levt[n]);
        vm_pan_text(w->v->pan[n], w->pant[n]);
        GT_SetGadgetAttrs(w->g_levt[n], w->win, NULL, GTTX_Text, (ULONG)w->levt[n], TAG_DONE);
        GT_SetGadgetAttrs(w->g_pant[n], w->win, NULL, GTTX_Text, (ULONG)w->pant[n], TAG_DONE);
    }
}

struct vmw *vmw_open(struct Screen *scr, struct vm_set *v)
{
    static const char *const vname[4] = { "Voice 1", "Voice 2", "Voice 3", "Voice 4" };
    static const char *const buttons[] = { "Save", "Use", "Cancel", NULL };
    int other;
    struct front *fr = front_claim("AGA-PISTORM Voices", &other);
    if (other) return NULL;                        /* open already: brought to the front */
    struct vmw *w = AllocVec(sizeof *w, MEMF_ANY | MEMF_CLEAR);
    if (!w) { front_release(fr); return NULL; }
    w->fr = fr;
    w->v = v;
    w->orig = *v;
    w->scr = scr;
    if (!(w->vi = GetVisualInfo(scr, TAG_DONE))) goto fail;
    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize, gh = fh + 6;
    int lname = tlen(srp, "Voice 4  (L)") + 8, ll = tlen(srp, "Level") + 8, sw = 110, tw1 = tlen(srp, "-30 dB") + 8;
    int lp = tlen(srp, "Pan") + 8, tw2 = tlen(srp, "Centre") + 8;
    int iw = lname + ll + sw + 4 + tw1 + 16 + lp + sw + 4 + tw2;
    int bw = 0;
    for (int i = 0; buttons[i]; i++) if (tlen(srp, buttons[i]) + 24 > bw) bw = tlen(srp, buttons[i]) + 24;
    int c_pre = 0;
    for (int i = 0; preset_names[i]; i++) if (tlen(srp, preset_names[i]) + 36 > c_pre) c_pre = tlen(srp, preset_names[i]) + 36;
    int title = scr->WBorTop + fh + 1, left = scr->WBorLeft + 10, y = title + 6;
    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = w->vi;
    ng.ng_Height = gh;
    struct Gadget *g = CreateContext(&w->glist);
    ng.ng_LeftEdge = left; ng.ng_TopEdge = y; ng.ng_Width = 26;
    ng.ng_GadgetText = (UBYTE *)"Voice mixer"; ng.ng_GadgetID = G_ON; ng.ng_Flags = PLACETEXT_RIGHT;
    g = w->g_on = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, v->on, GTCB_Scaled, TRUE, TAG_DONE);
    int px = left + 26 + tlen(srp, "Voice mixer") + 24 + tlen(srp, "Preset") + 8;
    ng.ng_LeftEdge = px; ng.ng_Width = c_pre;
    ng.ng_GadgetText = (UBYTE *)"Preset"; ng.ng_GadgetID = G_PRESET; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_preset = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)preset_names, GTCY_Active, v->preset, TAG_DONE);
    if (px + c_pre - left > iw) iw = px + c_pre - left;
    for (int n = 0; n < 4; n++) {
        int yy = y + (n + 1) * (gh + 6) + 6, x = left;
        static char lab[4][16];
        sprintf(lab[n], "%s  (%c)", vname[n], amiga_pan[n] < 0 ? 'L' : 'R');
        ng.ng_TopEdge = yy;
        ng.ng_LeftEdge = x + lname + ll; ng.ng_Width = sw;
        ng.ng_GadgetText = (UBYTE *)"Level"; ng.ng_GadgetID = G_LEV0 + n; ng.ng_Flags = PLACETEXT_LEFT;
        g = w->g_lev[n] = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 0, GTSL_Max, 16, GTSL_Level, v->lev[n],
                                       GA_RelVerify, TRUE, GA_Immediate, TRUE, TAG_DONE);
        x += lname + ll + sw + 4;
        vm_lev_text(v->lev[n], w->levt[n]);
        ng.ng_LeftEdge = x; ng.ng_Width = tw1; ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_LEVT0 + n;
        g = w->g_levt[n] = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)w->levt[n], TAG_DONE);
        x += tw1 + 16;
        ng.ng_LeftEdge = x + lp; ng.ng_Width = sw;
        ng.ng_GadgetText = (UBYTE *)"Pan"; ng.ng_GadgetID = G_PAN0 + n; ng.ng_Flags = PLACETEXT_LEFT;
        g = w->g_pan[n] = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 0, GTSL_Max, 16, GTSL_Level, v->pan[n] + 8,
                                       GA_RelVerify, TRUE, GA_Immediate, TRUE, TAG_DONE);
        x += lp + sw + 4;
        vm_pan_text(v->pan[n], w->pant[n]);
        ng.ng_LeftEdge = x; ng.ng_Width = tw2; ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_PANT0 + n;
        g = w->g_pant[n] = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)w->pant[n], TAG_DONE);
        /* the voice's name, left of its level: a text gadget, no box */
        ng.ng_LeftEdge = left; ng.ng_Width = lname - 8; ng.ng_GadgetID = 0;
        g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)lab[n], TAG_DONE);
    }
    int yb = y + 5 * (gh + 6) + 14;
    int bx[3] = { left, left + (iw - bw) / 2, left + iw - bw };
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = bx[i]; ng.ng_TopEdge = yb; ng.ng_Width = bw;
        ng.ng_GadgetText = (UBYTE *)buttons[i]; ng.ng_GadgetID = G_SAVE + i; ng.ng_Flags = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    if (!g) goto fail;
    int ww = iw + 20, ih = yb + gh + 8 - title;
    w->win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - Paula's voices",
        WA_Gadgets, (ULONG)w->glist,
        WA_InnerWidth, ww, WA_InnerHeight, ih,
        WA_Left, scr->Width > ww ? (scr->Width - ww) / 2 : 0,
        WA_Top, scr->Height > ih + title + 20 ? scr->Height - ih - title - 4 : 0,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE,
        WA_IDCMP, CYCLEIDCMP | CHECKBOXIDCMP | SLIDERIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                  IDCMP_VANILLAKEY,
        TAG_DONE);
    if (!w->win) goto fail;
    GT_RefreshWindow(w->win, NULL);
    front_set(w->fr, w->win);
    return w;
fail:
    vmw_close(w);
    return NULL;
}

ULONG vmw_sigs(struct vmw *w) { return 1UL << w->win->UserPort->mp_SigBit; }
void vmw_front(struct vmw *w) { WindowToFront(w->win); ActivateWindow(w->win); }

/* a fader moved: the mixer on, Custom */
static void moved(struct vmw *w)
{
    int sync = !w->v->on || w->v->preset;
    w->v->on = 1;
    w->v->preset = 0;
    if (sync) {
        GT_SetGadgetAttrs(w->g_on, w->win, NULL, GTCB_Checked, 1, TAG_DONE);
        GT_SetGadgetAttrs(w->g_preset, w->win, NULL, GTCY_Active, 0, TAG_DONE);
    }
    vm_send(w->v);
}

int vmw_handle(struct vmw *w)
{
    struct IntuiMessage *im;
    int rc = 0;
    while (!rc && (im = GT_GetIMsg(w->win->UserPort))) {
        ULONG cls = im->Class;
        UWORD code = im->Code;
        struct Gadget *gad = (struct Gadget *)im->IAddress;
        GT_ReplyIMsg(im);
        if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w->win); GT_EndRefresh(w->win, TRUE); continue; }
        if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_VANILLAKEY && code == 27)) { rc = 1; continue; }
        if (cls != IDCMP_GADGETUP && cls != IDCMP_GADGETDOWN && cls != IDCMP_MOUSEMOVE) continue;
        int id = gad->GadgetID;
        if (id == G_ON) { w->v->on = (gad->Flags & GFLG_SELECTED) != 0; vm_send(w->v); }
        else if (id == G_PRESET) {
            if (code) { vm_preset(w->v, code); vm_send(w->v); vmw_sync(w); }
            else w->v->preset = 0;
        }
        else if (id >= G_LEV0 && id < G_LEV0 + 4) {
            int n = id - G_LEV0;
            if (code != (UWORD)w->v->lev[n]) {
                w->v->lev[n] = (BYTE)code;
                vm_lev_text(code, w->levt[n]);
                GT_SetGadgetAttrs(w->g_levt[n], w->win, NULL, GTTX_Text, (ULONG)w->levt[n], TAG_DONE);
                moved(w);
            }
        }
        else if (id >= G_PAN0 && id < G_PAN0 + 4) {
            int n = id - G_PAN0;
            if ((int)code - 8 != w->v->pan[n]) {
                w->v->pan[n] = (BYTE)((int)code - 8);
                vm_pan_text(w->v->pan[n], w->pant[n]);
                GT_SetGadgetAttrs(w->g_pant[n], w->win, NULL, GTTX_Text, (ULONG)w->pant[n], TAG_DONE);
                moved(w);
            }
        }
        else if (id == G_SAVE) rc = 3;
        else if (id == G_USE) rc = 2;
        else if (id == G_CANCEL) rc = 1;
    }
    if (rc == 1) {                                      /* Cancel: what was there - the clicks stay */
        BYTE mute[4], solo = w->v->solo;
        memcpy(mute, w->v->mute, 4);
        *w->v = w->orig;
        memcpy(w->v->mute, mute, 4);
        w->v->solo = solo;
        vm_send(w->v);
    } else if (rc) vm_store(w->v, rc == 3);
    return rc;
}

void vmw_close(struct vmw *w)
{
    if (!w) return;
    front_release(w->fr);
    if (w->win) CloseWindow(w->win);
    if (w->glist) FreeGadgets(w->glist);
    if (w->vi) FreeVisualInfo(w->vi);
    FreeVec(w);
}

int vm_window(struct vm_set *v)
{
    int opened_intui = 0, opened_gfx = 0, opened_gt = 0, rc = 1;
    struct Screen *scr = NULL;
    struct vmw *w = NULL;
    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39))) opened_intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39))) opened_gfx = 1;
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39))) opened_gt = 1;
    if (IntuitionBase && GfxBase && GadToolsBase && (scr = LockPubScreen(NULL)) && (w = vmw_open(scr, v))) {
        rc = 0;
        while (!rc) {
            ULONG got = Wait(vmw_sigs(w) | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C) { *v = w->orig; vm_send(v); rc = 1; }
            else rc = vmw_handle(w);
        }
    }
    if (w) vmw_close(w);
    if (scr) UnlockPubScreen(NULL, scr);
    if (opened_gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (opened_gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (opened_intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    return rc - 1;
}
