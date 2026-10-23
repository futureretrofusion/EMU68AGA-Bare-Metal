/* agaboot - the game's picture on the Amiga's own RGB port: Chipset menu - RGB port.
 *
 * Kept in ENV(ARC):AGA_RGBPORT as words - for example
 * "HAM6 384x286 VARIABLE X=0 Y=-10":
 *   OFF    the RGB port stays dark while a game runs (the default)
 *   HAM6   the picture is there too, as HAM6; HDMI as usual
 *   ONLY   the picture is there only, as HAM6; HDMI stays black
 *   384x286 368x280 352x272 320x256
 *          how much of the frame it shows, around the standard screen: all
 *          of the overscan (the default) down to the standard screen alone
 *   VARIABLE / FIXED25 / AUTO
 *          a new picture as often as one gets through, or a steady 25 a
 *          second (each one up for two vertical blanks), or as often as
 *          one gets through but a steady 25 while 50 does not (1.115)
 *   X= Y=  the picture right / down on the RGB monitor, in lores pixels and
 *          lines (Position...)
 * The kernel's core 1 draws it (arm/aga_rgbout.c): the whole overscan frame,
 * where a real Amiga puts it, into 162 KB of chip RAM lent at every sandbox
 * entry and freed at the leave. Position... shows a calibration frame on the
 * RGB port through the same path, so what is set there is what games get.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rgbport.h"

#define AGA_CTRL     (*(volatile UWORD *)0xDFF1F2)
#define AGA_RGBBLOCK (*(volatile ULONG *)0xDFF1F4)
#define AGA_RGBOSLIST (*(volatile ULONG *)0xDFF1F8)
#define HW           (*(volatile struct Custom *)0xDFF000)
/* two 384 x 286 six-plane frames and their copper lists: AGA_RGB_BLOCK_BYTES
   in arm/aga_glue.h is the same number */
#define RGB_BYTES    (2 * 6 * 24 * 2 * 286 + 1024)
#define RGBPORT_VAR  "AGA_RGBPORT"
#define RGB_VAR      "AGA_RGBBLOCK"                /* ENV: the block while a game has it */

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c */
int  aga_active(void);                             /* agaboot.c */
void kernel_boards(int *built, int *runs);         /* agaboot.c */

static const char *const mode_words[3] = { "OFF", "HAM6", "ONLY" };
static const char *const size_words[4] = { "384x286", "368x280", "352x272", "320x256" };
static const char *const fps_words[3] = { "VARIABLE", "FIXED25", "AUTO" };
static const char *const col_words[2] = { "SMOOTH", "EXACT" };

int rgbport_parse(const char *s, struct rgb_port *p)
{
    int known = 0;
    char w[24];
    while (s && *s) {
        int n = 0;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
            if (n < (int)sizeof w - 1) w[n++] = (*s >= 'a' && *s <= 'z') ? *s - 32 : *s;
            s++;
        }
        w[n] = 0;
        if (!n) break;
        known++;
        if (!strcmp(w, "OFF")) p->mode = 0;
        else if (!strcmp(w, "HAM6") || !strcmp(w, "ON")) p->mode = 1;
        else if (!strcmp(w, "ONLY")) p->mode = 2;
        else if (!strcmp(w, "384X286") || !strcmp(w, "FULL")) p->size = 0;
        else if (!strcmp(w, "368X280")) p->size = 1;
        else if (!strcmp(w, "352X272")) p->size = 2;
        else if (!strcmp(w, "320X256") || !strcmp(w, "STANDARD")) p->size = 3;
        else if (!strcmp(w, "VARIABLE")) p->pace = 0;
        else if (!strcmp(w, "FIXED25") || !strcmp(w, "25FPS")) p->pace = 1;
        else if (!strcmp(w, "AUTO")) p->pace = 2;
        else if (!strcmp(w, "SMOOTH") || !strcmp(w, "DITHER")) p->exact = 0;
        else if (!strcmp(w, "EXACT")) p->exact = 1;
        else if (!strcmp(w, "32COLOURS") || !strcmp(w, "32COLORS")) p->c32 = 1;
        else if (!strcmp(w, "4096COLOURS") || !strcmp(w, "4096COLORS")) p->c32 = 0;
        else if (!strncmp(w, "X=", 2)) p->x = atoi(w + 2);
        else if (!strncmp(w, "Y=", 2)) p->y = atoi(w + 2);
        else known--;
    }
    if (p->x < -100) p->x = -100;
    if (p->x > 100) p->x = 100;
    if (p->y < -100) p->y = -100;
    if (p->y > 100) p->y = 100;
    return known;
}

void rgbport_format(const struct rgb_port *p, char *buf)
{
    sprintf(buf, "%s %s %s %s X=%d Y=%d%s", mode_words[p->mode], size_words[p->size], fps_words[p->pace],
            col_words[p->exact ? 1 : 0], p->x, p->y, p->c32 ? " 32COLOURS" : "");
}

void rgbport_load(struct rgb_port *p)
{
    char v[64];
    p->mode = 0;
    p->size = 0;
    p->pace = 2;        /* Auto (1.115): a steady 25 when 50 does not get through - judder otherwise */
    p->exact = 1;       /* Exact: the user preferred it on the A500's monitor to the dither's grain (2026-10-11) */
    p->c32 = 0;         /* HAM6 unless 32 colours is chosen */
    p->x = p->y = 0;
    if (GetVar((STRPTR)RGBPORT_VAR, (STRPTR)v, sizeof v, 0) > 0) rgbport_parse(v, p);
}

void rgbport_store(const struct rgb_port *p, int save)
{
    char v[64];
    rgbport_format(p, v);
    SetVar((STRPTR)RGBPORT_VAR, (STRPTR)v, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)RGBPORT_VAR, (STRPTR)v, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* Called with the machine Disable()d: the size, the pace and the offsets the
   kernel moves the picture by (a kernel before these words ignores them) */
static void offset_words(const struct rgb_port *p)
{
    AGA_CTRL = (UWORD)(0x5990 | p->size);
    AGA_CTRL = (UWORD)(0x59A0 | (p->pace == 1));
    AGA_CTRL = (UWORD)(0x59A8 | (p->pace == 2));        /* Auto (1.115) */
    AGA_CTRL = (UWORD)(0x59A4 | (p->exact ? 0 : 1));   /* Colours: smooth (dithered) or exact */
    AGA_CTRL = (UWORD)(0x59AC | (p->c32 ? 1 : 0));     /* Palette: 32 colours or HAM6 (1.115) */
    AGA_CTRL = (UWORD)(0x5400 | (UBYTE)p->x);
    AGA_CTRL = (UWORD)(0x5500 | (UBYTE)p->y);
}

/* Sandbox entry, before the enter word. Without the option the kernel is told
   0, so a block from a run that never reached the leave cannot be reused; and
   HDMI goes black only when the RGB port really gets the picture. The OS's
   copinit goes along: in a WHDLoad game the RGB port stays black while the
   OS's own display - the one copinit starts - is up, before WHDLoad takes the
   machine and after it gives it back. The kernel takes it only as a long
   write outside the sandbox: inside, $1F8 is the emulated chipset's window
   width (agawindow). An older kernel passes the write on to the real chips,
   which have no register there. */
void rgbport_entry(void)
{
    struct rgb_port p;
    rgbport_load(&p);
    APTR blk = p.mode ? AllocMem(RGB_BYTES, MEMF_CHIP | MEMF_PUBLIC | MEMF_CLEAR) : NULL;
    if (blk) {
        char v[16];
        sprintf(v, "%lu", (unsigned long)blk);
        SetVar((STRPTR)RGB_VAR, (STRPTR)v, -1, GVF_GLOBAL_ONLY);
    } else if (p.mode) {
        PutStr("agaboot: no 162 KB of chip RAM for the RGB port, continuing without it\n");
    }
    Disable();
    AGA_RGBBLOCK = (ULONG)blk;
    AGA_RGBOSLIST = GfxBase ? (ULONG)GfxBase->copinit : 0;
    AGA_CTRL = (UWORD)(0x5950 | (blk && p.mode == 2 ? 1 : 0));
    offset_words(&p);
    Enable();
}

/* After the leave: the kernel stopped core 1 and turned the real copper off
   inside it, before chip RAM went back, so nothing touches the block now. */
void rgbport_leave(void)
{
    char v[16];
    if (GetVar((STRPTR)RGB_VAR, (STRPTR)v, sizeof v, 0) <= 0) return;
    APTR blk = (APTR)strtoul(v, NULL, 10);
    if (blk) FreeMem(blk, RGB_BYTES);
    DeleteVar((STRPTR)RGB_VAR, GVF_GLOBAL_ONLY);
}

/* the libraries a window needs; each call closes only what it opened */
struct libs { int intui, gfx, gt; };
static int libs_open(struct libs *l, int gadtools)
{
    memset(l, 0, sizeof *l);
    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37))) l->intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37))) l->gfx = 1;
    if (gadtools && !GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37))) l->gt = 1;
    return IntuitionBase && GfxBase && (!gadtools || GadToolsBase);
}
static void libs_close(struct libs *l)
{
    if (l->gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (l->gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (l->intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

static LONG say(struct Window *w, const char *text, const char *buttons)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM - RGB port",
                             (UBYTE *)text, (UBYTE *)buttons };
    return EasyRequestArgs(w, &es, NULL, NULL);
}

/* ---- Position... ------------------------------------------------------------
   The kernel shows a calibration frame on the RGB port - a standard screen's
   border and grid, the offsets in the middle - moved as a game's picture
   will be. A borderless window over the desktop, which nobody looks at
   meanwhile, takes the keys: cursor keys, W A S D or keypad 8 4 6 2 move it
   (Shift: eight at a time); Return or the right mouse button keeps it, Esc
   puts it back. Afterwards the desktop's own native display is put back. */
static const char *const pos_text[] = {
    "AGA-PISTORM - RGB port position",
    "",
    "The RGB monitor shows the calibration frame now: the border of a standard screen.",
    "Move it with the cursor keys, W A S D or keypad 8 4 6 2 (Shift: bigger steps).",
    "Return or the right mouse button keeps it, Esc puts it back.",
};

int rgbport_position(struct rgb_port *p)
{
    struct libs l;
    struct rgb_port live = *p;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    APTR blk = NULL;
    int kept = 0, built, runs, shown = 0;

    if (!libs_open(&l, 0)) { libs_close(&l); return 0; }
    kernel_boards(&built, &runs);
    if (!built) {
        say(NULL, "This machine is not running the AGA-PISTORM kernel,\nso there is no RGB port picture to position.", "OK");
        goto out;
    }
    if (aga_active()) {
        say(NULL, "The emulated chipset has the machine (System wide).\n\nPosition the RGB port picture from the normal desktop.", "OK");
        goto out;
    }
    if (say(NULL, "The RGB monitor will show a calibration frame:\nthe border of a standard screen, with the offsets\n"
                  "in the middle, where a game's picture will be.\n\n"
                  "Move it with the cursor keys, W A S D or\nkeypad 8 4 6 2 (Shift: bigger steps).\n"
                  "Return or the right mouse button keeps it,\nEsc puts it back.",
            "Start|Cancel") != 1)
        goto out;
    if (!(blk = AllocMem(RGB_BYTES, MEMF_CHIP | MEMF_PUBLIC | MEMF_CLEAR))) {
        say(NULL, "There are not 162 KB of chip RAM free\nfor the calibration frame.", "OK");
        goto out;
    }
    if (!(scr = LockPubScreen(NULL))) goto out;
    win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, scr->Width, WA_Height, scr->Height,
                         WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
    if (!win) goto out;
    {   /* the same words on the desktop, for whoever looks there */
        struct RastPort *rp = win->RPort;
        int th = rp->TxHeight + 4, n = sizeof pos_text / sizeof pos_text[0];
        int y = (scr->Height - n * th) / 2 + rp->TxBaseline;
        SetRast(rp, 0);
        SetAPen(rp, 1);
        for (int i = 0; i < n; i++, y += th) {
            int w = TextLength(rp, (STRPTR)pos_text[i], strlen(pos_text[i]));
            Move(rp, (scr->Width - w) / 2, y);
            Text(rp, (STRPTR)pos_text[i], strlen(pos_text[i]));
        }
    }
    Disable();
    AGA_RGBBLOCK = (ULONG)blk;
    offset_words(&live);
    AGA_CTRL = 0x5953;                    /* the calibration frame on */
    Enable();
    shown = 1;

    for (int done = 0; !done; ) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        int ax = live.x, ay = live.y;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code, qual = m->Qualifier;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_MOUSEBUTTONS && code == MENUDOWN) { kept = 1; done = 1; }
            else if (cls == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                int step = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? 8 : 1;
                if (code == 0x44 || code == 0x43) { kept = 1; done = 1; }                  /* Return, Enter */
                if (code == 0x45) done = 1;                                                /* Esc */
                if (code == 0x4C || code == 0x11 || code == 0x3E) live.y -= step;          /* up, W, pad 8 */
                if (code == 0x4D || code == 0x21 || code == 0x1E) live.y += step;          /* down, S, pad 2 */
                if (code == 0x4E || code == 0x22 || code == 0x2F) live.x += step;          /* right, D, pad 6 */
                if (code == 0x4F || code == 0x20 || code == 0x2D) live.x -= step;          /* left, A, pad 4 */
            }
        }
        if (live.x < -100) live.x = -100;
        if (live.x > 100) live.x = 100;
        if (live.y < -100) live.y = -100;
        if (live.y > 100) live.y = 100;
        if (!done && (live.x != ax || live.y != ay)) {
            Disable();
            offset_words(&live);
            Enable();
        }
    }
    if (kept) { p->x = live.x; p->y = live.y; }
out:
    if (shown) {
        /* the frame off: the kernel stops core 1 and turns the real copper
           and planes off; then the desktop's own native display comes back,
           its copper list and DMA first, then Intuition's view */
        Disable();
        AGA_CTRL = 0x5952;
        AGA_RGBBLOCK = 0;
        HW.cop1lc = (ULONG)GfxBase->copinit;
        HW.copjmp1 = 0;
        HW.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE;
        Enable();
        RethinkDisplay();
    }
    if (win) CloseWindow(win);
    if (scr) UnlockPubScreen(NULL, scr);
    if (blk) FreeMem(blk, RGB_BYTES);
    libs_close(&l);
    return kept;
}

/* ---- RGB port -----------------------------------------------------------------
   The picture, its palette, its size and its pace as cycle gadgets, Position..., Save
   (ENVARC:, for good), Use (ENV:, until the next reboot) or Cancel. It takes
   effect from the next game. */
enum { G_MODE = 1, G_PAL, G_SIZE, G_FPS, G_COL, G_POS, G_SAVE, G_USE, G_CANCEL };
#define N_CYCLES 5

static int widest(struct RastPort *rp, const char *const *s)
{
    int w = 0;
    for (; *s; s++) {
        int t = TextLength(rp, (STRPTR)*s, strlen(*s));
        if (t > w) w = t;
    }
    return w;
}

int rgbport_window(void)
{
    static const char *const mode_names[] = { "Off", "HAM6 and HDMI", "RGB port only (HDMI black)", NULL };
    static const char *const pal_names[] = { "HAM6 (4096 colours)", "32 colours (smoother motion)", NULL };
    static const char *const size_names[] = { "Full frame 384 x 286", "368 x 280", "352 x 272", "Standard 320 x 256", NULL };
    static const char *const fps_names[] = { "Variable, up to 50", "Fixed 25", "Auto: 25 when 50 is too much", NULL };
    static const char *const col_names[] = { "Exact (rounded)", "Smooth (dithered)", NULL };
    static const char *const labels[] = { "Picture", "Palette", "Size", "Frame rate", "Colours", NULL };
    static const char *const buttons[] = { "Save", "Use", "Cancel", NULL };
    struct libs l;
    struct rgb_port cur;
    struct Screen *scr = NULL;
    APTR vi = NULL;
    struct Gadget *glist = NULL, *g;
    struct Window *win = NULL;
    int rc = 20;

    if (!libs_open(&l, 1)) goto out;
    rgbport_load(&cur);
    if (!(scr = LockPubScreen(NULL))) goto out;
    if (!(vi = GetVisualInfo(scr, TAG_DONE))) goto out;

    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize;
    int lw = widest(srp, labels) + 12;
    int cw = widest(srp, mode_names), t;
    if ((t = widest(srp, pal_names)) > cw) cw = t;
    if ((t = widest(srp, size_names)) > cw) cw = t;
    if ((t = widest(srp, fps_names)) > cw) cw = t;
    if ((t = widest(srp, col_names)) > cw) cw = t;
    cw += 40;                                        /* the cycle glyph */
    int bw = widest(srp, buttons) + 24;
    int gh = fh + 6, gap = 4;
    int left = scr->WBorLeft + 12, top = scr->WBorTop + fh + 1 + 10;
    int iw = lw + cw + 24;
    if (3 * bw + 2 * 12 + 24 > iw) iw = 3 * bw + 2 * 12 + 24;

    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_Height = gh;
    g = CreateContext(&glist);
    const char *const *names[N_CYCLES] = { mode_names, pal_names, size_names, fps_names, col_names };
    int active[N_CYCLES] = { cur.mode, cur.c32, cur.size, cur.pace, cur.exact ? 0 : 1 };
    for (int i = 0; i < N_CYCLES; i++) {
        ng.ng_LeftEdge = left + lw;
        ng.ng_TopEdge = top + i * (gh + gap);
        ng.ng_Width = cw;
        ng.ng_GadgetText = (UBYTE *)labels[i];
        ng.ng_GadgetID = G_MODE + i;
        ng.ng_Flags = PLACETEXT_LEFT;
        g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)names[i], GTCY_Active, active[i], TAG_DONE);
    }
    ng.ng_LeftEdge = left + lw;
    ng.ng_TopEdge = top + N_CYCLES * (gh + gap);
    ng.ng_Width = cw;
    ng.ng_GadgetText = (UBYTE *)"Position...";
    ng.ng_GadgetID = G_POS;
    ng.ng_Flags = PLACETEXT_IN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    int by = top + (N_CYCLES + 1) * (gh + gap) + 10;
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
        WA_Title, (ULONG)"AGA-PISTORM - RGB port",
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
                case G_MODE: cur.mode = code; break;
                case G_PAL:  cur.c32 = code; break;
                case G_SIZE: cur.size = code; break;
                case G_FPS:  cur.pace = code; break;
                case G_COL:  cur.exact = code == 0; break;
                case G_POS:  rgbport_position(&cur); break;
                case G_SAVE: rgbport_store(&cur, 1); done = 1; break;
                case G_USE:  rgbport_store(&cur, 0); done = 1; break;
                case G_CANCEL: done = 1; break;
                }
            }
        }
    }
    rc = 0;
out:
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    libs_close(&l);
    return rc;
}
