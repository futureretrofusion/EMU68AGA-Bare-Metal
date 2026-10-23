/* agaboot - the AGA picture on HDMI: Chipset menu - RTG Image.
 *
 * The settings, kept together in ENV(ARC):AGA_ZOOM as words - for example
 * "640x256 SMOOTH 4:3 SCANLINES=0 X=4 Y=-2":
 *   size   FULL      the whole 800 x 286 overscan frame
 *          720x272   what a TV shows
 *          640x256   the standard screen
 *          CUSTOM    the frame less the edges CUT= (PAL) or NTSCCUT= (NTSC)
 *                    say: left,top,right,bottom in hires pixels and lines,
 *                    set with Crop... (1.114)
 *          AUTO      the game's own display window, followed as it runs -
 *                    with FILL the game fills the display (1.114, RGB2RTG's)
 *   look   SHARP     nearest neighbour: square pixels
 *          SMOOTH    the scaler's Mitchell-Netravali kernel
 *          TV        neighbouring pixels blended across, as composite video
 *                    mixed them, for games dithered for a TV (Virtual
 *                    Karting's grey road is red next to cyan)
 *   scale  4:3       the shape a real monitor gives (the default)
 *          INTEGER   that shape, a whole number of display lines a line
 *          FILL      the whole display
 *          5:4       square pixels: a PAL screen 5:4, an NTSC one 16:10
 *          16:9 16:10 21:9 32:9  the picture stretched to that shape
 *                    (1.116, RGB2RTG's)
 *   SCANLINES=n      dark lines between the Amiga's: 0 off, 1-4 = 25-100%
 *   X= Y=  the picture right / down, in hires pixels and lines. The crop
 *          window moves the other way and never leaves the frame, so FULL
 *          has nowhere to go - RGB2RTG's model.
 * and since 1.110, RGB2RTG's too:
 *   LACED=n          scanlines for interlaced screens shown woven, as above
 *   FLICKERFIX=ON|OFF  interlaced screens woven into one steady picture, or
 *                    a field at a time as the Amiga's own output shows them
 *   BRIGHTNESS=n     0 normal, 1-4 = 110, 120, 135, 150% - for scanlines,
 *                    which darken the picture
 *   BORDER=n         the TV border, 0 off or 1-10%: the picture that much
 *                    smaller on a TV that cuts the edges of 1080p off;
 *                    BORDERX= and BORDERY= set one direction each
 *   PAL | NTSC       the standard a game starts in - an NTSC game at its
 *                    60 Hz, framed as NTSC
 * and since 1.116, RGB2RTG's too:
 *   MASK=n           the CRT mask, an aperture grille's dark columns over the
 *                    picture: 0 off, 1-4 = 25-100% (darkening only)
 *   GAMMA=EXACT|CRT|LIGHTER  as it is, a 1084S's darker mid-tones, or the
 *                    shadows lifted for a dark game; Brightness goes on top
 *   SCANSHAPE=EVEN|CRISP|WHOLE  the scanlines' shape: every line as dark
 *                    wherever its rows fall (the default), a whole dark row
 *                    at each line's end, or the picture fitted to a whole
 *                    number of rows a line
 * An AGA_ZOOM from 1.103-1.106 ("720x272 SHARP") reads the same. "TV" was an
 * undocumented name for the 720x272 size there; it is the look now.
 * Scale and scanlines are RGB2RTG's, new in 1.107.
 *
 * A game can have a picture of its own (picture_load_game): the same words in
 * ENV(ARC):AGA-PISTORM/Games/<its drawer's name>, which it gets instead of
 * AGA_ZOOM when it starts - and since 1.114 a demo or program started with
 * Run program too, under its drawer's name. RTG Image's "Settings for" lists
 * every one that has its own, and Add... picks any game's or demo's drawer.
 *
 * RTG Image is a small window in RGB2RTG's style, applied as you pick.
 * Position... is RGB2RTG's calibration screen, drawn by the kernel here:
 * the desktop's native screens never reach HDMI on this machine. Crop... is
 * RGB2RTG's crop, on the last game's picture, which the kernel keeps.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "picture.h"

#define AGA_CTRL  (*(volatile UWORD *)0xDFF1F2)
#define ZOOM_VAR  "AGA_ZOOM"
#define GAMES_DIR "AGA-PISTORM/Games/"

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
struct Library *GadToolsBase;

int  aga_active(void);                          /* agaboot.c: Lisa answers */
void kernel_boards(int *built, int *runs);      /* agaboot.c: mine, and for which Pi */
ULONG diag_read(UWORD sel);                     /* agaboot.c: one of the kernel's counters */
int  hdmisync_pref(void);                       /* agaboot.c: HDMI sync and rate */
void hdmisync_store(int on, int save);
void hdmisync_send(int on);
int  hdmirate_pref(void);
int  hdmirate_set(int rate, char *msg, int len);

static const char *const size_words[5] = { "FULL", "720x272", "640x256", "CUSTOM", "AUTO" };
static const char *const look_words[3] = { "SHARP", "SMOOTH", "TV" };
static const char *const shape_words[3] = { "EVEN", "CRISP", "WHOLE" };
static const char *const gamma_words[3] = { "EXACT", "CRT", "LIGHTER" };
static int in_custom;          /* picture_crt_custom is parsing the Custom look: no CRTLOOK= in it */
static const char *const scale_words[8] = { "4:3", "INTEGER", "FILL", "5:4", "16:9", "16:10", "21:9", "32:9" };

/* The crops as the kernel has them (arm/aga_hvs.c zoom_rect): x, y, w, h in
   the 800 x 286 frame. Only for the position's limits. */
static const short crop[3][4] = { { 0, 0, 800, 286 }, { 74, 10, 720, 272 }, { 114, 18, 640, 256 } };
static const short crop_ntsc[3][4] = { { 0, 0, 800, 237 }, { 74, 10, 720, 227 }, { 114, 18, 640, 200 } };

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- CUSTOM's edges ---------------------------------------------------------
   As arm/aga_crop.h has them: until the user cuts their own, 720 x 272's;
   the kernel takes left and right even and leaves 320 x 100 at least, so
   cut_fix does the same and the numbers here are the ones on the screen.
   The words carry an edge in two five-bit pieces, then where it goes; bits
   1 and 15 stay clear (aga_crop.h, which the host tests check). */
#define D_CROP       504        /* AGA_D_CROP: arm/aga_diag.c pins it */
#define D_AUTO_X     505        /* ...and Size Auto's window: x << 16 | w, y << 16 | h (+$8000 NTSC) */
#define D_AUTO_Y     506
#define CROP_E3(s)   (((s) & 1) | (((s) & 6) << 1))
#define CROP_E5(d)   (((d) & 1) | (((d) & 0x1E) << 1))
static const short cut_default[2][4] = { { 74, 10, 6, 4 }, { 74, 10, 6, 0 } };

static void cut_fix(int *c, int ntsc)
{
    int rows = ntsc ? 237 : 286;
    for (int e = 0; e < 4; e++) c[e] = clamp(c[e], 0, 800);
    c[0] &= ~1;
    c[2] &= ~1;
    if (c[0] > 800 - 320) c[0] = 800 - 320;
    if (c[2] > 800 - 320 - c[0]) c[2] = 800 - 320 - c[0];
    if (c[1] > rows - 100) c[1] = rows - 100;
    if (c[3] > rows - 100 - c[1]) c[3] = rows - 100 - c[1];
}

/* a size's rectangle as edges, where its position puts it (aga_hvs.c apply_crop) */
static void cut_from_size(const struct aga_picture *p, int ntsc, int *c)
{
    const short *r = (ntsc ? crop_ntsc : crop)[p->size < 3 ? p->size : 1];
    int rows = ntsc ? 237 : 286;
    int x = clamp(r[0] - p->x, 0, 800 - r[2]) & ~1, y = clamp(r[1] - p->y, 0, rows - r[3]);
    c[0] = x;
    c[1] = y;
    c[2] = 800 - x - r[2];
    c[3] = rows - y - r[3];
    cut_fix(c, ntsc);
}

static void cut_parse(const char *s, int *c, int ntsc)
{
    for (int e = 0; e < 4 && *s; e++) {
        c[e] = clamp(atoi(s), 0, 800);
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    cut_fix(c, ntsc);
}

/* one edge to the kernel: e 0-3 left, top, right, bottom; caller Disable()s */
static void cut_send(int ntsc, int e, int v)
{
    AGA_CTRL = (UWORD)(0x58C0 | CROP_E5((v >> 5) & 31));
    AGA_CTRL = (UWORD)(0x58C0 | CROP_E5(v & 31));
    AGA_CTRL = (UWORD)(0x5870 | CROP_E3(e + 4 * ntsc));
}

int picture_has_crop(void)
{
    int built, runs;
    kernel_boards(&built, &runs);
    return built && (diag_read(D_CROP) & 1);
}

void picture_default(struct aga_picture *p, int size)
{
    p->size = (size >= 0 && size <= 4) ? size : 0;
    for (int n = 0; n < 2; n++)
        for (int e = 0; e < 4; e++) p->cut[n][e] = cut_default[n][e];
    p->look = 1;
    p->scale = 0;
    p->scan = 0;
    p->x = p->y = 0;
    p->lscan = 0;
    p->mask = 0;
    p->sshape = 0;
    p->gamma = 0;
    p->nscan = -1;
    p->flicker = 1;
    p->bright = 0;
    p->bx = p->by = 0;
    p->ntsc = 0;
}

int picture_parse(const char *s, struct aga_picture *p)
{
    int known = 0;
    char w[32];
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
        if (!strcmp(w, "FULL") || !strcmp(w, "800X286")) p->size = 0;
        else if (!strcmp(w, "720X272") || !strcmp(w, "OVERSCAN")) p->size = 1;
        else if (!strcmp(w, "640X256") || !strcmp(w, "STANDARD")) p->size = 2;
        else if (!strcmp(w, "CUSTOM")) p->size = 3;
        else if (!strcmp(w, "AUTO")) p->size = 4;
        else if (n == 1 && w[0] >= '0' && w[0] <= '4') p->size = w[0] - '0';
        else if (!strncmp(w, "CUT=", 4)) cut_parse(w + 4, p->cut[0], 0);
        else if (!strncmp(w, "NTSCCUT=", 8)) cut_parse(w + 8, p->cut[1], 1);
        else if (!strcmp(w, "SHARP")) p->look = 0;
        else if (!strcmp(w, "SMOOTH")) p->look = 1;
        else if (!strcmp(w, "TV")) p->look = 2;
        else if (!strcmp(w, "4:3") || !strcmp(w, "ASPECT")) p->scale = 0;
        else if (!strcmp(w, "INTEGER")) p->scale = 1;
        else if (!strcmp(w, "FILL")) p->scale = 2;
        else if (!strcmp(w, "5:4") || !strcmp(w, "SQUARE")) p->scale = 3;
        else if (!strcmp(w, "16:9")) p->scale = 4;
        else if (!strcmp(w, "16:10")) p->scale = 5;
        else if (!strcmp(w, "21:9")) p->scale = 6;
        else if (!strcmp(w, "32:9")) p->scale = 7;
        else if (!strcmp(w, "GAMMA=EXACT")) p->gamma = 0;
        else if (!strcmp(w, "GAMMA=CRT")) p->gamma = 1;
        else if (!strcmp(w, "GAMMA=LIGHTER")) p->gamma = 2;
        else if (!strncmp(w, "SCANLINES=", 10)) p->scan = clamp(atoi(w + 10), 0, 4);
        else if (!strncmp(w, "LACED=", 6)) p->lscan = clamp(atoi(w + 6), 0, 4);
        else if (!strcmp(w, "NTSCLINES=PAL")) p->nscan = -1;
        else if (!strncmp(w, "NTSCLINES=", 10)) p->nscan = clamp(atoi(w + 10), 0, 4);
        else if (!strncmp(w, "MASK=", 5)) p->mask = clamp(atoi(w + 5), 0, 4);
        else if (!strncmp(w, "CRT=", 4)) p->mask = clamp(atoi(w + 4), 0, 4);
        else if (!strncmp(w, "SCANSHAPE=", 10) && w[10] >= 48 && w[10] <= 50 && !w[11]) p->sshape = w[10] - 48;
        else if (!strcmp(w, "SCANSHAPE=EVEN")) p->sshape = 0;
        else if (!strcmp(w, "SCANSHAPE=CRISP")) p->sshape = 1;
        else if (!strcmp(w, "SCANSHAPE=WHOLE")) p->sshape = 2;
        else if (!strcmp(w, "CRTLOOK=1084S") || !strcmp(w, "CRTLOOK=1084")) picture_crt_1084s(p);
        else if (!strcmp(w, "CRTLOOK=NONE") || !strcmp(w, "CRTLOOK=OFF")) picture_crt_none(p);
        else if (!strcmp(w, "CRTLOOK=CUSTOM")) { if (!picture_crt_custom(p)) known--; }
        else if (!strcmp(w, "CRTLOOK=SAVE")) { if (in_custom || !picture_crt_keep(p)) known--; }
        else if (!strcmp(w, "FLICKERFIX") || !strcmp(w, "FLICKERFIX=ON")) p->flicker = 1;
        else if (!strcmp(w, "FLICKERFIX=OFF")) p->flicker = 0;
        else if (!strncmp(w, "BRIGHTNESS=", 11)) p->bright = clamp(atoi(w + 11), 0, 4);
        else if (!strncmp(w, "BORDER=", 7)) p->bx = p->by = clamp(atoi(w + 7), 0, 10);
        else if (!strncmp(w, "BORDERX=", 8)) p->bx = clamp(atoi(w + 8), 0, 10);
        else if (!strncmp(w, "BORDERY=", 8)) p->by = clamp(atoi(w + 8), 0, 10);
        else if (!strcmp(w, "PAL")) p->ntsc = 0;
        else if (!strcmp(w, "NTSC")) p->ntsc = 1;
        else if (!strncmp(w, "X=", 2)) p->x = atoi(w + 2);
        else if (!strncmp(w, "Y=", 2)) p->y = atoi(w + 2);
        else known--;
    }
    return known;
}

void picture_format(const struct aga_picture *p, char *buf)
{
    sprintf(buf, "%s %s %s SCANLINES=%d LACED=%d SCANSHAPE=%s MASK=%d FLICKERFIX=%s BRIGHTNESS=%d GAMMA=%s"
                 " BORDERX=%d BORDERY=%d %s X=%d Y=%d",
            size_words[p->size], look_words[p->look], scale_words[p->scale >= 0 && p->scale <= 7 ? p->scale : 0],
            p->scan, p->lscan, shape_words[p->sshape >= 0 && p->sshape <= 2 ? p->sshape : 0], p->mask,
            p->flicker ? "ON" : "OFF", p->bright, gamma_words[p->gamma >= 0 && p->gamma <= 2 ? p->gamma : 0],
            p->bx, p->by, p->ntsc ? "NTSC" : "PAL", p->x, p->y);
    if (p->nscan >= 0) sprintf(buf + strlen(buf), " NTSCLINES=%d", p->nscan);   /* only when not as PAL's */
    int own = p->size == 3;                      /* the edges when they are used, or not the default */
    for (int n = 0; n < 2; n++)
        for (int e = 0; e < 4; e++) if (p->cut[n][e] != cut_default[n][e]) own = 1;
    if (own)
        sprintf(buf + strlen(buf), " CUT=%d,%d,%d,%d NTSCCUT=%d,%d,%d,%d",
                p->cut[0][0], p->cut[0][1], p->cut[0][2], p->cut[0][3],
                p->cut[1][0], p->cut[1][1], p->cut[1][2], p->cut[1][3]);
}

/* The buffers here and RTG Image's state are static, off the stack: the
   Chipset menu runs agaboot on a 4096-byte stack, and 1.110's window with
   them on it overran that and froze the machine (Intuition, GadTools and
   Picasso96 need most of it to open the window). agaboot is one command at
   a time, nothing here is re-entered. */
void picture_load(struct aga_picture *p, int size_default)
{
    static char v[PICTURE_TEXT];
    picture_default(p, size_default);
    if (GetVar((STRPTR)ZOOM_VAR, (STRPTR)v, sizeof v, 0) > 0) picture_parse(v, p);
}

void picture_store(const struct aga_picture *p, int save)
{
    static char v[PICTURE_TEXT];
    picture_format(p, v);
    SetVar((STRPTR)ZOOM_VAR, (STRPTR)v, -1, GVF_GLOBAL_ONLY | (save ? GVF_SAVE_VAR : 0));
}

/* ---- one game's own picture -------------------------------------------------
   ENV:AGA-PISTORM/Games/<key>, and ENVARC: when saved. SetVar makes no
   drawers, so they are made first; one that is there already is fine. */
static void game_var(const char *key, char *name)
{
    strcpy(name, GAMES_DIR);
    strncat(name, key, 30);
}

int picture_load_game(struct aga_picture *p, const char *key)
{
    static char name[64], v[PICTURE_TEXT];
    if (!key || !key[0]) return 0;
    game_var(key, name);
    if (GetVar((STRPTR)name, (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) <= 0) return 0;
    picture_default(p, 0);
    picture_parse(v, p);
    return 1;
}

static void make_drawer(const char *path)
{
    BPTR l = CreateDir((STRPTR)path);
    if (l) UnLock(l);
}

int picture_store_game(const struct aga_picture *p, const char *key, int save)
{
    static char name[64], v[PICTURE_TEXT];
    if (!key || !key[0]) return 0;
    make_drawer("ENV:AGA-PISTORM");
    make_drawer("ENV:AGA-PISTORM/Games");
    if (save) {
        make_drawer("ENVARC:AGA-PISTORM");
        make_drawer("ENVARC:AGA-PISTORM/Games");
    }
    game_var(key, name);
    picture_format(p, v);
    return SetVar((STRPTR)name, (STRPTR)v, -1, GVF_GLOBAL_ONLY | (save ? GVF_SAVE_VAR : 0)) != 0;
}

void picture_forget_game(const char *key)
{
    static char name[64];
    if (!key || !key[0]) return;
    game_var(key, name);
    DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY);
    DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* ---- CRT look in one (1.118, RGB2RTG 0.76-dev83, EMM #2197) ------------------
   The CRT look as a Commodore 1084S shows the Amiga, from the monitor's own
   figures as Lola Retro (zoran123456, MIT) works them out of its service
   manual: lines about a quarter dark on PAL, a 6.5 MHz amplifier that
   softens hires pixels (Smooth), a CRT's darker mid-tones; Brightness 120%
   for what the lines and the mask take away (chosen on RGB2RTG's A1200 -
   110% was a little dark). No lines on interlaced screens: on the real
   monitor the two fields fill each other's gaps. The 1084S has a slot mask,
   which RGB2RTG draws; here the aperture grille stands in, at 25% too. */
void picture_crt_1084s(struct aga_picture *p)
{
    p->look = 1;
    p->scan = 1;
    p->nscan = 2;                               /* NTSC's lines stronger: 50% (RGB2RTG 0.76-dev85) */
    p->lscan = 0;
    p->sshape = 0;
    p->mask = 1;
    p->bright = 2;
    p->gamma = 1;
}

/* no CRT look: the scanlines, the mask, Brightness and Gamma as without one
   (Look and the scanlines' shape stay as they are) */
void picture_crt_none(struct aga_picture *p)
{
    p->scan = 0;
    p->nscan = -1;
    p->lscan = 0;
    p->mask = 0;
    p->bright = 0;
    p->gamma = 0;
}

/* A CRT look of the user's own: Look and CRT look's rows, kept by Save as
   Custom look (agaboot ZOOM CRTLOOK=SAVE) in AGA-PISTORM/CRTLook - ENV: and
   ENVARC: at once, a preset and not a setting that Cancel takes back - and
   put back by Custom look (CRTLOOK=CUSTOM), for all games or one. In the
   picture's own words, so picture_parse reads it. */
#define CUSTOM_LOOK "AGA-PISTORM/CRTLook"

int picture_crt_custom(struct aga_picture *p)
{
    static char v[PICTURE_TEXT];
    static struct aga_picture t;
    if (in_custom || GetVar((STRPTR)CUSTOM_LOOK, (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) <= 0) return 0;
    t = *p;
    in_custom = 1;
    picture_parse(v, &t);
    in_custom = 0;
    p->look = t.look;
    p->scan = t.scan;
    p->nscan = t.nscan;
    p->lscan = t.lscan;
    p->sshape = t.sshape;
    p->mask = t.mask;
    p->bright = t.bright;
    p->gamma = t.gamma;
    return 1;
}

int picture_crt_custom_has(void)
{
    char v[8];
    return GetVar((STRPTR)CUSTOM_LOOK, (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) > 0;
}

int picture_crt_keep(const struct aga_picture *p)
{
    static char v[160];
    sprintf(v, "%s SCANLINES=%d LACED=%d SCANSHAPE=%s MASK=%d BRIGHTNESS=%d GAMMA=%s NTSCLINES=",
            look_words[p->look >= 0 && p->look <= 2 ? p->look : 1], p->scan, p->lscan,
            shape_words[p->sshape >= 0 && p->sshape <= 2 ? p->sshape : 0], p->mask, p->bright,
            gamma_words[p->gamma >= 0 && p->gamma <= 2 ? p->gamma : 0]);
    if (p->nscan >= 0 && p->nscan <= 4) sprintf(v + strlen(v), "%d", p->nscan);
    else strcat(v, "PAL");
    make_drawer("ENV:AGA-PISTORM");
    make_drawer("ENVARC:AGA-PISTORM");
    return SetVar((STRPTR)CUSTOM_LOOK, (STRPTR)v, -1, GVF_GLOBAL_ONLY) != 0 &&
           SetVar((STRPTR)CUSTOM_LOOK, (STRPTR)v, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR) != 0;
}

/* ---- the TV border on the RTG desktop -----------------------------------------
   The kernel shrinks the game's picture itself; the desktop is VideoCore.card's,
   and 1.110's card (as RGB2RTG's) takes VCMD_SET_BORDER at its "VideoCore" port:
   the RTG screen, the pointer and the overlays that much smaller, centred. The
   card takes a message only when mn_Length is its struct VC4Msg's size (its
   src/messages.h), so this is laid out the same: 66 bytes, GetKernel the
   largest of the union. It is asked first (VCMD_GET_CAPS): a card without the
   border does not know the command. */
struct vc_msg {
    struct Message msg;
    ULONG cmd;
    union {
        struct { UBYTE kernel; ULONG b; ULONG c; WORD kernel_val[16]; } GetKernel;
        struct { ULONG caps; } GetCaps;
        struct { UBYTE x, y; } SetBorder;
    } u;
};
typedef char vc_msg_is_66_bytes[sizeof(struct vc_msg) == 66 ? 1 : -1];
#define VCMD_GET_CAPS    9
#define VCMD_SET_BORDER  12
#define VC4CAP_BORDER    (1UL << 5)

/* One command to VideoCore.card, answered in *msg: 1 answered, 0 no card
   or no answer in time. The card's task can be held up - on AmiKit it did
   not answer while RGB2RTG's APPLY ran from S:User-Startup, and an endless
   WaitPort there held the boot (RGB2RTG 864145c, EMM #2134); agaboot MENU
   sends the border from User-Startup the same way. So the command goes as a
   copy in public memory and the wait is bounded (2 s). A copy the card has
   not taken is taken back off its port; one it has taken stays with it: the
   copy and the reply port stay allocated (a few bytes) for its ReplyMsg,
   and the port no longer signals anyone. */
#define VC_WAIT_TICKS 100                        /* 1/50 s each */
static int vc_ask(struct vc_msg *msg)
{
    struct MsgPort *reply = CreateMsgPort(), *vc;
    struct vc_msg *m;
    int sent = 0;
    if (!reply) return 0;
    if (!(m = AllocMem(sizeof *m, MEMF_PUBLIC | MEMF_CLEAR))) { DeleteMsgPort(reply); return 0; }
    CopyMem(msg, m, sizeof *m);
    m->msg.mn_ReplyPort = reply;
    m->msg.mn_Length = sizeof *m;
    Forbid();
    if ((vc = FindPort((STRPTR)"VideoCore")) != NULL) { PutMsg(vc, &m->msg); sent = 1; }
    Permit();
    if (!sent) { FreeMem(m, sizeof *m); DeleteMsgPort(reply); return 0; }   /* no card running */
    for (int t = 0; t < VC_WAIT_TICKS; t++) {
        if (GetMsg(reply)) {
            CopyMem(m, msg, sizeof *m);
            FreeMem(m, sizeof *m);
            DeleteMsgPort(reply);
            return 1;
        }
        Delay(1);
    }
    int queued = 0;                              /* no answer in time */
    Forbid();
    for (struct Node *n = vc->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n == &m->msg.mn_Node) { Remove(n); queued = 1; break; }
    if (!queued) reply->mp_Flags = PA_IGNORE;    /* its reply, whenever it comes, wakes nobody */
    Permit();
    if (queued) { FreeMem(m, sizeof *m); DeleteMsgPort(reply); }
    return 0;
}

int picture_border_desktop(int bx, int by)
{
    struct vc_msg m;
    memset(&m, 0, sizeof m);
    m.cmd = VCMD_GET_CAPS;
    if (!vc_ask(&m) || !(m.u.GetCaps.caps & VC4CAP_BORDER)) return 0;   /* no card, no answer, or a card without it */
    memset(&m, 0, sizeof m);
    m.cmd = VCMD_SET_BORDER;
    m.u.SetBorder.x = (UBYTE)clamp(bx, 0, 10);
    m.u.SetBorder.y = (UBYTE)clamp(by, 0, 10);
    return vc_ask(&m);
}

/* CUSTOM's edges first, so that the size finds them there (a kernel before
   1.114 ignores them); then $590N - the size and, for a kernel before 1.107,
   sharp or not - then the look, the scale and scanlines, then the position;
   a kernel before 1.107 ignores all after $590N, one before 1.110 the last
   six and one before 1.116 the CRT mask's $510n, the scanline shape's
   $511n, the gamma's $512n and the full scale's $513n (bits 1 and 15
   clear, so they only land in the core's aga_ctrl there, harmlessly) - it
   takes the scale from $597S, 4:3 for the new shapes. $5910/1 is for the
   NEXT game: it takes effect when the sandbox is entered. */
void picture_words(const struct aga_picture *p)
{
    for (int n = 0; n < 2; n++)
        for (int e = 0; e < 4; e++) cut_send(n, e, p->cut[n][e]);
    AGA_CTRL = (UWORD)(0x5900 | p->size | (p->look == 0 ? 8 : 0));
    AGA_CTRL = (UWORD)(0x5920 | p->look);
    AGA_CTRL = (UWORD)(0x5970 | (p->scale >= 0 && p->scale <= 2 ? p->scale : 0));
    AGA_CTRL = (UWORD)(0x5130 | (p->scale & 1) | ((p->scale & 6) << 1));
    AGA_CTRL = (UWORD)(0x5110 | (p->sshape & 1) | ((p->sshape & 6) << 1));   /* before the scanlines: Whole lines fits to them */
    AGA_CTRL = (UWORD)(0x5980 | p->scan);
    AGA_CTRL = (UWORD)(0x5100 | (p->mask & 1) | ((p->mask & 6) << 1));
    AGA_CTRL = (UWORD)(0x5700 | (UBYTE)p->x);
    AGA_CTRL = (UWORD)(0x5600 | (UBYTE)p->y);
    AGA_CTRL = (UWORD)(0x59B0 | p->bx);
    AGA_CTRL = (UWORD)(0x59C0 | p->by);
    AGA_CTRL = (UWORD)(0x59D0 | (p->flicker ? 1 : 0));
    AGA_CTRL = (UWORD)(0x59E0 | p->lscan);
    {   /* $517n: NTSC screens' own scanlines - 0 as PAL's, 1-5 off .. 100% (a kernel before 1.118 ignores it) */
        int v = p->nscan >= 0 && p->nscan <= 4 ? p->nscan + 1 : 0;
        AGA_CTRL = (UWORD)(0x5170 | (v & 1) | ((v & 6) << 1));
    }
    AGA_CTRL = (UWORD)(0x59F0 | p->bright);
    AGA_CTRL = (UWORD)(0x5120 | (p->gamma & 1) | ((p->gamma & 6) << 1));
    AGA_CTRL = (UWORD)(0x5910 | (p->ntsc ? 1 : 0));
}

void picture_send(const struct aga_picture *p)
{
    Disable();
    picture_words(p);
    Enable();
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
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM - RTG Image",
                             (UBYTE *)text, (UBYTE *)buttons };
    return EasyRequestArgs(w, &es, NULL, NULL);
}

/* ---- Position... ------------------------------------------------------------
   RGB2RTG's Position screen. The kernel draws the calibration pattern where
   a standard PAL screen lands in the frame and puts it on HDMI in the
   desktop's place ($5931); a borderless window over the whole desktop, which
   nobody can see meanwhile, takes the keys and the mouse. Dragging with the
   left button moves the picture with the pointer; so do the cursor keys,
   W A S D and keypad 8 4 6 2 (two hires pixels or a line; with Shift sixteen
   or eight). The right button or Return keeps it, Esc puts it back. With the
   sandbox up (System wide) the kernel draws nothing and the desktop itself
   moves. Returns 1 when kept: p->x and p->y are then the new position. */
static const char *const pos_text[] = {
    "AGA-PISTORM - picture position",
    "",
    "HDMI shows the calibration frame now, where a standard screen lands.",
    "Drag it with the mouse - or cursor keys, W A S D, keypad 8 4 6 2 (Shift: bigger steps).",
    "Right mouse button or Return keeps it, Esc puts it back.",
};

int picture_position(struct aga_picture *p)
{
    struct libs l;
    struct aga_picture live = *p;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    int kept = 0, built, runs;

    if (!libs_open(&l, 0)) { libs_close(&l); return 0; }
    kernel_boards(&built, &runs);
    if (!built) {
        say(NULL, "This machine is not running the AGA-PISTORM kernel,\nso there is no picture to position.", "OK");
        goto out;
    }
    if (live.size == 0) {
        say(NULL, "Full frame shows the whole picture:\nthere is nothing to move.\n\n"
                  "Pick 720 x 272 or 640 x 256 first.", "OK");
        goto out;
    }
    if (live.size == 3) {
        say(NULL, "Custom's edges are its place:\nCrop... moves them.", "OK");
        goto out;
    }
    if (live.size == 4) {
        say(NULL, "Auto follows the game's own window:\nthere is nothing to move.", "OK");
        goto out;
    }
    int live_desktop = aga_active();       /* the emulated chipset has the desktop */
    if (say(NULL, live_desktop
            ? "The emulated chipset is showing the desktop,\nso the picture itself moves.\n\n"
              "Drag it with the mouse, or use the cursor keys,\nW A S D or keypad 8 4 6 2 (Shift: bigger steps).\n"
              "Right mouse button or Return keeps it,\nEsc puts it back."
            : "HDMI will show a calibration frame instead of\nthe desktop: the border of a standard screen,\n"
              "with the offsets in the middle.\n\n"
              "Drag it with the mouse, or use the cursor keys,\nW A S D or keypad 8 4 6 2 (Shift: bigger steps).\n"
              "Right mouse button or Return keeps it,\nEsc puts it back.",
            "Start|Cancel") != 1)
        goto out;

    const short *c = crop[live.size];
    int xmin = c[0] - (800 - c[2]), xmax = c[0], ymin = c[1] - (286 - c[3]), ymax = c[1];
    if (!(scr = LockPubScreen(NULL))) goto out;
    win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, scr->Width, WA_Height, scr->Height,
                         WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, TAG_DONE);
    if (!win) goto out;
    {   /* the same words on the desktop, for whoever can see it */
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
    /* A drag follows the pointer: the kernel fits the crop into the display
       at one scale for both axes, a line counting two rows (aga_hvs.c fit),
       and the desktop is taken to be the display's size. */
    ULONG sx16 = ((ULONG)c[2] << 16) / scr->Width, sy16 = ((ULONG)c[3] << 17) / scr->Height;
    ULONG s16 = sx16 > sy16 ? sx16 : sy16;              /* frame pixels per desktop pixel, 16.16 */

    if (live.x < xmin) live.x = xmin;
    if (live.x > xmax) live.x = xmax;
    if (live.y < ymin) live.y = ymin;
    if (live.y > ymax) live.y = ymax;
    live.x = live.x / 2 * 2;
    Disable();
    picture_words(&live);
    if (!live_desktop) AGA_CTRL = 0x5931;
    Enable();

    int drag = 0, mx0 = 0, my0 = 0, x0 = 0, y0 = 0, ax = live.x, ay = live.y;
    for (int done = 0; !done; ) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code, qual = m->Qualifier;
            WORD mx = m->MouseX, my = m->MouseY;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_MOUSEBUTTONS) {
                if (code == SELECTDOWN) { drag = 1; mx0 = mx; my0 = my; x0 = live.x; y0 = live.y; }
                else if (code == SELECTUP) drag = 0;
                else if (code == MENUDOWN) { kept = 1; done = 1; }
            } else if (cls == IDCMP_MOUSEMOVE) {
                if (drag) {
                    live.x = x0 + (int)(((LONG)(mx - mx0) * (LONG)s16) >> 16);
                    live.y = y0 + (int)(((LONG)(my - my0) * (LONG)s16) >> 17);
                }
            } else if (cls == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                int big = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
                if (code == 0x44 || code == 0x43) { kept = 1; done = 1; }                         /* Return, Enter */
                if (code == 0x45) done = 1;                                                       /* Esc */
                if (code == 0x4C || code == 0x11 || code == 0x3E) live.y -= big ? 8 : 1;          /* up, W, pad 8 */
                if (code == 0x4D || code == 0x21 || code == 0x1E) live.y += big ? 8 : 1;          /* down, S, pad 2 */
                if (code == 0x4E || code == 0x22 || code == 0x2F) live.x += big ? 16 : 2;         /* right, D, pad 6 */
                if (code == 0x4F || code == 0x20 || code == 0x2D) live.x -= big ? 16 : 2;         /* left, A, pad 4 */
            }
        }
        /* once per batch: a drag sends many moves */
        if (live.x < xmin) live.x = xmin;
        if (live.x > xmax) live.x = xmax;
        if (live.y < ymin) live.y = ymin;
        if (live.y > ymax) live.y = ymax;
        live.x = live.x / 2 * 2;                    /* the kernel's steps: one lores pixel */
        if (!done && (live.x != ax || live.y != ay)) {
            Disable();
            AGA_CTRL = (UWORD)(0x5700 | (UBYTE)live.x);
            AGA_CTRL = (UWORD)(0x5600 | (UBYTE)live.y);
            Enable();
            ax = live.x;
            ay = live.y;
        }
    }
    if (!live_desktop) { Disable(); AGA_CTRL = 0x5930; Enable(); }
    if (kept) { p->x = live.x; p->y = live.y; }
out:
    if (win) CloseWindow(win);
    if (scr) UnlockPubScreen(NULL, scr);
    picture_send(p);                     /* the picture as kept, or as it was */
    libs_close(&l);
    return kept;
}

/* ---- Crop... ------------------------------------------------------------------
   RGB2RTG's crop, for Size Custom: the four edges cut off the frame, PAL's
   and NTSC's. The kernel puts the last game's picture on HDMI cut to them -
   the Position pattern when no game of that standard has run since the Pi
   started - with each edge's number by it, the one that moves in orange
   ($5880 + E3, arm/aga_crop.h). A borderless window over the desktop, which
   nobody sees meanwhile, takes the keys and the mouse: Tab picks the edge
   (Shift-Tab back), the cursor keys - W A S D, keypad 8 4 6 2 - move it the
   way they point, two pixels or a line (Shift: sixteen or eight), and so
   does a drag with the left button. N switches between the PAL and the NTSC
   edges, 0 starts again from 720 x 272's. The right button or Return keeps
   them, Esc puts them back. With the sandbox up (System wide) the live
   picture is cut instead, in the standard it is in. A size other than
   Custom starts from its own rectangle. Returns 1 when kept: p->size is
   CUSTOM then, p->cut the edges. */
static const char *const crop_text[] = {
    "AGA-PISTORM - custom crop",
    "",
    "HDMI shows the picture cut to the edges, each edge's number by it.",
    "Tab picks an edge; cursor keys, W A S D, keypad 8 4 6 2 or a drag move it (Shift: bigger steps).",
    "N: the PAL or the NTSC edges.  0: start again from 720 x 272.",
    "Right mouse button or Return keeps them, Esc puts them back.",
};

int picture_crop(struct aga_picture *p)
{
    static struct aga_picture live;         /* static: see picture_load */
    static int sent[2][4];
    struct libs l;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    int kept = 0, built, runs;

    live = *p;
    if (!libs_open(&l, 0)) { libs_close(&l); return 0; }
    kernel_boards(&built, &runs);
    if (!built) {
        say(NULL, "This machine is not running the AGA-PISTORM kernel,\nso there is no picture to crop.", "OK");
        goto out;
    }
    ULONG st = diag_read(D_CROP);
    if (!(st & 1)) {
        say(NULL, "The kernel running here has no custom crop:\ninstall the current AGA-PISTORM.", "OK");
        goto out;
    }
    int live_desktop = aga_active();        /* the emulated chipset has the desktop */
    /* the standard to cut: the live picture's, or the last game's, or the one games start in */
    int std = live_desktop ? (int)((st >> 2) & 1) : (st & 8) ? (int)((st >> 4) & 1) : live.ntsc;
    if (live.size != 3) {
        cut_from_size(&live, 0, live.cut[0]);
        cut_from_size(&live, 1, live.cut[1]);
        if (live.size == 4) {                    /* Auto: from the window it followed last */
            ULONG ax = diag_read(D_AUTO_X), ay = diag_read(D_AUTO_Y);
            int an = (ay & 0x8000) ? 1 : 0, *c = live.cut[an], rows = an ? 237 : 286;
            int x = (int)(ax >> 16), w = (int)(ax & 0xFFFF), y = (int)(ay >> 16), h = (int)(ay & 0x7FFF);
            if (w >= 160 && h >= 64 && x + w <= 800 && y + h <= rows) {
                c[0] = x; c[1] = y; c[2] = 800 - x - w; c[3] = rows - y - h;
                cut_fix(c, an);
            }
        }
        live.size = 3;
    }
    cut_fix(live.cut[0], 0);
    cut_fix(live.cut[1], 1);
    if (say(NULL, live_desktop
            ? "The emulated chipset is showing the desktop,\nso the desktop itself is cut.\n\n"
              "Tab picks an edge, the cursor keys or a drag\nmove it (Shift: bigger steps).\n"
              "0 starts again from 720 x 272.\n"
              "Right mouse button or Return keeps them,\nEsc puts them back."
            : "HDMI will show the last game's picture cut to\nthe edges - with no game since the Pi started,\n"
              "the calibration frame - each edge's number\nby it, the one that moves in orange.\n\n"
              "Tab picks an edge, the cursor keys or a drag\nmove it (Shift: bigger steps).\n"
              "N: the PAL or the NTSC edges.\n0 starts again from 720 x 272.\n"
              "Right mouse button or Return keeps them,\nEsc puts them back.",
            "Start|Cancel") != 1)
        goto out;

    if (!(scr = LockPubScreen(NULL))) goto out;
    win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, scr->Width, WA_Height, scr->Height,
                         WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, TAG_DONE);
    if (!win) goto out;
    {   /* the same words on the desktop, for whoever can see it */
        struct RastPort *rp = win->RPort;
        int th = rp->TxHeight + 4, n = sizeof crop_text / sizeof crop_text[0];
        int y = (scr->Height - n * th) / 2 + rp->TxBaseline;
        SetRast(rp, 0);
        SetAPen(rp, 1);
        for (int i = 0; i < n; i++, y += th) {
            int w = TextLength(rp, (STRPTR)crop_text[i], strlen(crop_text[i]));
            Move(rp, (scr->Width - w) / 2, y);
            Text(rp, (STRPTR)crop_text[i], strlen(crop_text[i]));
        }
    }

    int edge = 0, shown_std = std, shown_edge = edge;
    Disable();
    picture_words(&live);
    if (!live_desktop) AGA_CTRL = (UWORD)(0x5880 | CROP_E3(edge + 4 * std));
    Enable();
    for (int n = 0; n < 2; n++)
        for (int e = 0; e < 4; e++) sent[n][e] = live.cut[n][e];

    /* A drag follows the pointer, as on the Position screen: the kernel fits
       what is left into the display at one scale for both axes, a line two
       rows, and the desktop is taken to be the display's size. */
    int drag = 0, mx0 = 0, my0 = 0, v0 = 0;
    ULONG s16 = 0x10000;
    for (int done = 0; !done; ) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code, qual = m->Qualifier;
            WORD mx = m->MouseX, my = m->MouseY;
            ReplyMsg((struct Message *)m);
            int *c = live.cut[std];
            if (cls == IDCMP_MOUSEBUTTONS) {
                if (code == SELECTDOWN) {
                    int rows = std ? 237 : 286;
                    ULONG sx16 = ((ULONG)(800 - c[0] - c[2]) << 16) / scr->Width;
                    ULONG sy16 = ((ULONG)(rows - c[1] - c[3]) << 17) / scr->Height;
                    s16 = sx16 > sy16 ? sx16 : sy16;     /* frame pixels per desktop pixel, 16.16 */
                    drag = 1; mx0 = mx; my0 = my; v0 = c[edge];
                }
                else if (code == SELECTUP) drag = 0;
                else if (code == MENUDOWN) { kept = 1; done = 1; }
            } else if (cls == IDCMP_MOUSEMOVE) {
                if (drag) {
                    int d = (edge & 1) ? (int)(((LONG)(my - my0) * (LONG)s16) >> 17)
                                       : (int)(((LONG)(mx - mx0) * (LONG)s16) >> 16);
                    c[edge] = edge < 2 ? v0 + d : v0 - d;   /* left and top grow with the pointer */
                }
            } else if (cls == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                int big = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
                int dx = 0, dy = 0;
                if (code == 0x44 || code == 0x43) { kept = 1; done = 1; }                         /* Return, Enter */
                if (code == 0x45) done = 1;                                                       /* Esc */
                if (code == 0x42) { edge = (edge + (big ? 3 : 1)) & 3; drag = 0; }               /* Tab */
                if (code == 0x36 && !live_desktop) { std ^= 1; drag = 0; }                       /* N */
                if (code == 0x0A || code == 0x0F)                                                 /* 0, pad 0 */
                    for (int e = 0; e < 4; e++) c[e] = cut_default[std][e];
                if (code == 0x4C || code == 0x11 || code == 0x3E) dy = -1;                        /* up, W, pad 8 */
                if (code == 0x4D || code == 0x21 || code == 0x1E) dy = 1;                         /* down, S, pad 2 */
                if (code == 0x4E || code == 0x22 || code == 0x2F) dx = 1;                         /* right, D, pad 6 */
                if (code == 0x4F || code == 0x20 || code == 0x2D) dx = -1;                        /* left, A, pad 4 */
                c = live.cut[std];
                if (edge == 0) c[0] += dx * (big ? 16 : 2);           /* the edges move the way the key points */
                if (edge == 2) c[2] -= dx * (big ? 16 : 2);
                if (edge == 1) c[1] += dy * (big ? 8 : 1);
                if (edge == 3) c[3] -= dy * (big ? 8 : 1);
            }
        }
        /* once per batch: a drag sends many moves */
        cut_fix(live.cut[std], std);
        if (!done) {
            Disable();
            for (int e = 0; e < 4; e++)
                if (live.cut[std][e] != sent[std][e]) {
                    cut_send(std, e, live.cut[std][e]);
                    sent[std][e] = live.cut[std][e];
                }
            if (!live_desktop && (std != shown_std || edge != shown_edge))
                AGA_CTRL = (UWORD)(0x5880 | CROP_E3(edge + 4 * std));
            Enable();
            shown_std = std;
            shown_edge = edge;
        }
    }
    if (!live_desktop) { Disable(); AGA_CTRL = 0x5930; Enable(); }
    if (kept) *p = live;
out:
    if (win) CloseWindow(win);
    if (scr) UnlockPubScreen(NULL, scr);
    picture_send(p);                     /* the picture as kept, or as it was */
    libs_close(&l);
    return kept;
}

/* ---- RTG Image ----------------------------------------------------------------
   Every setting as a cycle gadget, applied as you pick; Position...; Save
   (ENVARC:, for good), Use (ENV:, until the next reboot) or Cancel; Help
   says what each row does. The
   picture is the one games get; on the desktop nothing shows until a game
   starts, or the Position screen is up.

   "Settings for" picks what the window edits: all games (AGA_ZOOM), or the
   game started last - its own picture, which it then gets instead. Save and
   Use keep every one of the two that was changed; Remove drops the game's
   own picture, so it gets the all-games one again. */
enum { G_TARGET = 1, G_SIZE, G_LOOK, G_SCALE, G_SCAN, G_LSCAN, G_SSHAPE, G_MASK, G_FLICKER, G_BRIGHT, G_GAMMA,
       G_BORDER, G_VIDEO, G_RATE, G_SYNC, G_NSCAN, G_POS, G_CROP, G_ADD, G_REMOVE, G_SAVE, G_USE, G_CANCEL, G_HELP,
       G_CRT1084S, G_CRTNONE, G_CRTCUSTOM, G_CRTKEEP };
/* the last two cycles are HDMI's own, the same for every game: HDMI rate and
   HDMI sync (agaboot.c hdmirate_set, hdmisync_*) - below Position... and
   Crop..., apart from the picture's */
static int win_rate, win_sync;          /* 0 config.txt's own / 1 1080p50 / 2 1080p60 / 3 1080p 50.2 Hz;
                                           0 off / 1 on */
static int has_crop;                 /* the kernel cuts Custom edges and follows Auto: both offered */
#define N_CYCLES 16
#define N_PICTURE 13                    /* ...of which the picture's (and the target) */
#define C_SSHAPE (G_SSHAPE - G_TARGET)  /* the shape's cycle: greyed while both scanline rows are off */

static int widest(struct RastPort *rp, const char *const *s)
{
    int w = 0;
    for (; *s; s++) {
        int t = TextLength(rp, (STRPTR)*s, strlen(*s));
        if (t > w) w = t;
    }
    return w;
}

/* The Help button (1.116, RGB2RTG 0.75-dev45's): what each row does, short,
   in the window's order - README's PICTURE SIZE AND SHARPNESS has the whole
   story */
static const char img_help[] =
    "\001SETTINGS FOR:\001 what the rows below are for - All games and demos, or one game or demo of its own, "
    "which it then gets while it runs. The game started last is in the list; \001Add...\001 picks any game's or "
    "demo's drawer, \001Remove\001 gives it the all-games picture again.\n"
    "\001PICTURE:\001\n"
    "\001Look:\001 Sharp, crisp square pixels, all the same width; Smooth, the scaler's soft edges; TV, "
    "neighbouring pixels blended as composite video did - for games dithered for a TV.\n"
    "\001Size:\001 Full frame, the whole overscan area; 720 x 272, what a TV shows; 640 x 256, the standard "
    "screen; Custom, the frame less the edges Crop... cuts off; Auto, the game's own display window, "
    "followed as it plays.\n"
    "\001Scale:\001 Integer, a whole number of HDMI lines for every Amiga line - the sharpest; Fill screen, the "
    "whole display; 4:3, a real monitor's shape; 5:4 (square), square pixels; 16:9, 16:10, 21:9 and 32:9, "
    "the picture stretched to that shape - 16:9 fills a TV, 21:9 and 32:9 an ultrawide.\n"
    "\001TV border:\001 the picture that much smaller, for a TV that cuts the edges off. All games' border "
    "shrinks the RTG desktop and its pointer too.\n"
    "\001Flicker fixer:\001 On, an interlaced screen as one steady picture with all its lines (720p and up); "
    "Off, each half-picture as it comes.\n"
    "\001Video:\001 PAL or NTSC, the standard games start in, from the next game on. A game that sets its "
    "standard itself still gets its own.\n"
    "\001Crop...\001 sets Custom's edges, \001Position...\001 moves the picture - both on a calibration screen on HDMI.\n"
    "\001CRT LOOK:\001\n"
    "\001Scanlines and Scanlines (interlace):\001 dark lines between the Amiga's lines, as on a CRT - each with "
    "its own strength, for normal and for interlaced screens.\n"
    "\001Scanlines (NTSC):\001 NTSC screens' own strength - a CRT shows NTSC's lines stronger, the beam as wide "
    "and the lines further apart (a 1084S about 40% dark against PAL's 27%); As PAL, the same as Scanlines. A "
    "game that switches between PAL and NTSC gets the other lines from that frame on.\n"
    "\001Scanline shape\001 (greyed while both are Off): Even, every Amiga line as dark with 4:3 and Fill screen "
    "too; Crisp, a whole dark HDMI line at the end of each, some lines a line taller; Whole lines, every "
    "Amiga line a whole number of HDMI lines - the picture a little smaller, its shape kept.\n"
    "\001CRT mask:\001 an aperture grille's fine dark columns over the picture, 25% to 100% dark.\n"
    "\001Brightness:\001 110% to 150%, to make up for the dark lines; white stays white. The RGB port stays "
    "as it was.\n"
    "\001Gamma:\001 Exact, the Amiga's colours as they are; CRT, a 1084S's darker mid-tones; Lighter, the dark "
    "parts lifted.\n"
    "\0011084S look\001 sets the rows above as a Commodore 1084S shows the Amiga: scanlines at 25%, 50% on NTSC "
    "screens (none on interlaced ones, whose fields fill the gaps), the CRT mask at 25%, Look Smooth, Gamma CRT and "
    "Brightness 120% for what the lines and the mask darken - a start to change from. \001No CRT look\001 puts "
    "the scanlines, the mask, Brightness and Gamma back to none. \001Save as Custom look\001 keeps Look and the "
    "CRT look's rows as they are now as your own - at once, \001Cancel does not take it back\001 - and "
    "\001Custom look\001 puts them back, for all games or the one in Settings for.\n"
    "\001THIS AMIGA\001 - the same for every game:\n"
    "\001HDMI rate:\001 config.txt's HDMI mode at 1080p 50 or 60 Hz, \001from the next power cycle\001 - for HDMI sync. "
    "It needs the boot partition mounted. 50.2 Hz: room for HDMI sync to follow games whose frames are a "
    "line short too; \001if a TV shows no picture\001, delete the hdmi_ lines in config.txt's AGA-PISTORM block "
    "on a PC.\n"
    "\001HDMI sync:\001 Follow the Amiga makes HDMI's frame a line or two longer to keep pace with the game: on "
    "a 50 Hz screen a PAL game (on 60 Hz an NTSC one) shows every frame exactly once.\n"
    "Every change shows at once. \001Save\001 keeps it for good, \001Use\001 until you switch off, \001Cancel\001 puts back "
    "what was.";

/* text into lines of at most `w` characters, broken at spaces (RGB2RTG's) */
static void wrap(char *t, int w)
{
    char *line = t, *sp = NULL;
    for (char *c = t; *c; c++) {
        if (*c == '\n') { line = c + 1; sp = NULL; continue; }
        if (*c == ' ') sp = c;
        if (c - line >= w && sp) { *sp = '\n'; line = sp + 1; sp = NULL; }
    }
}

/* A help text in a window of its own that fits the screen: its lines in a
   list with a scroll bar, and OK (1.118, RGB2RTG 0.76-dev83, EMM #2197).
   Before, a requester a screenful at a time with More... (RGB2RTG
   752d13a): a requester cannot scroll. The window under it takes no input
   meanwhile (an empty requester in it, and on OS 3.0 the busy pointer).
   The parts between two HB are bold - the settings' names, the headings,
   what not to miss - drawn by the list's own callback (GadTools V39, OS
   3.0); an older GadTools shows the text plain. Each line's bold state at
   its start rides in its node's ln_Type, so a bold part wrapped onto the
   next line stays bold. */
#define HB '\001'
enum { H_LIST = 1, H_OK };
static struct TextFont *help_tf;

/* a help line's parts in turn, bold between HB, from bold (the line before's
   bold part running on): drawn at x, y up to xmax, or only measured */
static int help_line(struct RastPort *rp, const char *s, int bold, int x, int y, int xmax, int draw)
{
    int x0 = x;
    while (*s) {
        const char *e = s;
        while (*e && *e != HB) e++;
        if (e > s) {
            SetSoftStyle(rp, bold ? FSF_BOLD : 0, FSF_BOLD);
            if (draw) {
                struct TextExtent te;
                LONG room = xmax - x + 1;
                ULONG fit = room > 0 ? TextFit(rp, (STRPTR)s, e - s, &te, NULL, 1, room, 32767) : 0;
                Move(rp, x, y);
                Text(rp, (STRPTR)s, fit);
                x = rp->cp_x;
            } else {
                x += TextLength(rp, (STRPTR)s, e - s) + (bold ? (int)(e - s) : 0);   /* the bold smear, a pixel each */
            }
        }
        if (*e == HB) {
            bold = !bold;
            e++;
        }
        s = e;
    }
    SetSoftStyle(rp, 0, FSF_BOLD);
    return x - x0;
}

/* the list's callback: a line drawn - the background, then its parts */
static ULONG help_draw(register struct Hook *hk __asm("a0"), register struct Node *node __asm("a2"),
                       register struct LVDrawMsg *m __asm("a1"))
{
    (void)hk;
    if (m->lvdm_MethodID != LV_DRAW)
        return LVCB_UNKNOWN;
    struct RastPort *rp = m->lvdm_RastPort;
    UWORD *pens = m->lvdm_DrawInfo->dri_Pens;
    struct Rectangle *b = &m->lvdm_Bounds;
    if (help_tf) SetFont(rp, help_tf);
    SetDrMd(rp, JAM1);
    SetAPen(rp, pens[BACKGROUNDPEN]);
    RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);
    SetAPen(rp, pens[TEXTPEN]);
    help_line(rp, node->ln_Name, node->ln_Type, b->MinX + 2, b->MinY + rp->TxBaseline, b->MaxX, 1);
    return LVCB_OK;
}

static struct Hook help_hook = { { NULL, NULL }, (HOOKFUNC)help_draw, NULL, NULL };

static void help_request(struct Window *win, const char *title, const char *text)
{
    struct Screen *scr = win->WScreen;
    int fh = scr->Font->ta_YSize;
    int hook_ok = GadToolsBase->lib_Version >= 39;
    int busy_ok = IntuitionBase->LibNode.lib_Version >= 39;       /* SetWindowPointer: OS 3.0 */
    int n = 1, used = 0, w = 0;
    for (const char *c = text; *c; c++)
        if (*c == '\n') n++;
    char *buf = (char *)AllocVec(strlen(text) + 1, MEMF_ANY);       /* the lines, the text's own untouched */
    struct Node *nodes = (struct Node *)AllocVec(n * sizeof(struct Node), MEMF_CLEAR);
    APTR vi = GetVisualInfo(scr, TAG_DONE);
    struct Gadget *glist = NULL, *g;
    struct Window *hw = NULL;
    struct Requester block;
    int blocked = 0;
    struct List list;
    list.lh_Head = (struct Node *)&list.lh_Tail;
    list.lh_Tail = NULL;
    list.lh_TailPred = (struct Node *)&list.lh_Head;
    help_tf = OpenFont(scr->Font);
    if (!buf || !nodes || !vi || !help_tf)
        goto out;
    char *d = buf;
    for (const char *c = text; *c; c++)
        if (hook_ok || *c != HB) *d++ = *c;             /* no callback: the text plain */
    *d = 0;
    struct RastPort trp = scr->RastPort;                /* to measure in, bold or not */
    SetFont(&trp, help_tf);
    int bold = 0;
    for (char *q = buf; used < n; ) {                   /* a node a line: bold at its start, its width */
        char *e = strchr(q, '\n');
        if (e) *e = 0;
        nodes[used].ln_Name = q;
        nodes[used].ln_Type = (UBYTE)bold;
        AddTail(&list, &nodes[used++]);
        int t = help_line(&trp, q, bold, 0, 0, 0, 0);
        if (t > w) w = t;
        for (char *c = q; *c; c++)
            if (*c == HB) bold = !bold;
        if (!e) break;
        q = e + 1;
    }
    int lw = w + 36;                                    /* the scroll bar, the frame, the text's offset */
    if (lw > scr->Width - 40) lw = scr->Width - 40;
    int rows = (scr->Height - scr->WBorTop - 2 * fh - 48) / fh;     /* less the title bar, OK and the gaps */
    if (rows > used) rows = used;
    if (rows < 4) rows = 4;
    int left = scr->WBorLeft + 8, top = scr->WBorTop + fh + 1 + 6;
    int bh = fh + 6, bw = TextLength(&trp, (STRPTR)"OK", 2) + 48;

    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    g = CreateContext(&glist);
    ng.ng_LeftEdge = left;
    ng.ng_TopEdge = top;
    ng.ng_Width = lw;
    ng.ng_Height = rows * fh + 4;
    ng.ng_GadgetID = H_LIST;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&list, GTLV_ReadOnly, TRUE,
                     hook_ok ? GTLV_CallBack : TAG_IGNORE, (ULONG)&help_hook, TAG_DONE);
    int by = top + rows * fh + 4 + 8;
    ng.ng_LeftEdge = left + (lw - bw) / 2;
    ng.ng_TopEdge = by;
    ng.ng_Width = bw;
    ng.ng_Height = bh;
    ng.ng_GadgetText = (UBYTE *)"OK";
    ng.ng_GadgetID = H_OK;
    ng.ng_Flags = PLACETEXT_IN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    if (!g)
        goto out;
    int iw = lw + 16, ih = by + bh + 8 - (scr->WBorTop + fh + 1);
    hw = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)title,
        WA_Gadgets, (ULONG)glist,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_Left, scr->Width > iw ? (scr->Width - iw) / 2 : 0,
        WA_Top, scr->Height > ih ? (scr->Height - ih) / 2 : 0,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE,
        WA_IDCMP, LISTVIEWIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
        TAG_DONE);
    if (!hw)
        goto out;
    GT_RefreshWindow(hw, NULL);
    InitRequester(&block);
    blocked = Request(&block, win);
    if (busy_ok) SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
    for (int done = 0; !done; ) {
        WaitPort(hw->UserPort);
        struct IntuiMessage *msg;
        while (!done && (msg = GT_GetIMsg(hw->UserPort))) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            struct Gadget *gad = (struct Gadget *)msg->IAddress;
            GT_ReplyIMsg(msg);
            if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(hw);
                GT_EndRefresh(hw, TRUE);
            } else if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_GADGETUP && gad->GadgetID == H_OK) ||
                       (cls == IDCMP_VANILLAKEY && (code == 13 || code == 27))) {
                done = 1;                               /* OK, the close gadget, Return or Esc */
            }
        }
    }
    if (busy_ok) SetWindowPointer(win, TAG_DONE);
out:
    if (blocked) EndRequest(&block, win);
    if (hw) CloseWindow(hw);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (help_tf) CloseFont(help_tf);
    help_tf = NULL;
    if (nodes) FreeVec(nodes);
    if (buf) FreeVec(buf);
}

/* Scale's list in the window, RGB2RTG's order: Integer and Fill screen, then
   the shapes, 4:3 first. The setting keeps its numbers (and AGA_ZOOM the
   words); only the window's order is this. */
static const UBYTE scale_row[8] = { 1, 2, 0, 3, 4, 5, 6, 7 };        /* row -> aga_picture.scale */
static int scale_to_row(int s)
{
    for (int i = 0; i < 8; i++) if (scale_row[i] == s) return i;
    return 2;
}

/* The window's order (1.116, RGB2RTG 0.76's grouping): Settings for and
   Add.../Remove at the top - the presets - then what one holds under two
   headings, Picture (Crop... and Position... under it) and CRT look, and
   at the bottom This Amiga: HDMI's own, the same for every game. The
   gadget IDs and the cycles' arrays keep the enum's order; this is only
   where each one goes. */
#define N_ROWS_PIC 6
#define N_ROWS_CRT 7
static const UBYTE row_order[] = { G_LOOK, G_SIZE, G_SCALE, G_BORDER, G_FLICKER, G_VIDEO,
                                   G_SCAN, G_LSCAN, G_NSCAN, G_SSHAPE, G_MASK, G_BRIGHT, G_GAMMA,
                                   G_RATE, G_SYNC };
static int row_slot(int id)
{
    for (int s = 0; s < (int)sizeof row_order; s++) if (row_order[s] == id) return s;
    return 0;
}

/* the cycles' positions for a picture (index 0, the target, is not one) */
static void cycles_of(const struct aga_picture *p, int *v)
{
    int b = p->bx > p->by ? p->bx : p->by;
    v[1] = (p->size >= 3 && !has_crop) ? 0 : p->size; v[2] = p->look; v[3] = scale_to_row(p->scale);
    v[4] = p->scan; v[5] = p->lscan;
    v[6] = p->sshape >= 0 && p->sshape <= 2 ? p->sshape : 0; v[7] = p->mask; v[8] = p->flicker ? 0 : 1;
    v[9] = p->bright; v[10] = p->gamma >= 0 && p->gamma <= 2 ? p->gamma : 0; v[11] = (b + 1) / 2;
    v[12] = p->ntsc ? 1 : 0; v[13] = win_rate; v[14] = win_sync;
    v[15] = p->nscan >= 0 && p->nscan <= 4 ? p->nscan + 1 : 0;
}

/* Scanline shape means nothing while both scanline rows are off (RGB2RTG's
   window greys it the same way) */
static void shape_avail(struct Window *win, struct Gadget *g, const struct aga_picture *p)
{
    GT_SetGadgetAttrs(g, win, NULL, GA_Disabled, !p->scan && !p->lscan && p->nscan <= 0, TAG_DONE);
}

/* ---- what "Settings for" offers ---------------------------------------------
   All games and demos first, then every game or demo with a picture of its
   own (the files in ENVARC: and ENV:AGA-PISTORM/Games, in name order), the
   one started last, and those Add... picks this time. Static, like all of
   this window's state (picture_load). Two label arrays taken in turn: a
   cycle gadget is told about a new entry with a new array. */
#define MAX_TARGETS 40
static char tkey[MAX_TARGETS][32];
static const char *tlabels[2][MAX_TARGETS + 1];
static int ntargets, tl_cur;

static int target_find(const char *key)
{
    for (int i = 1; i < ntargets; i++) if (!stricmp(tkey[i], key)) return i;
    return -1;
}

static int target_add(const char *key)
{
    int i = target_find(key);
    if (i >= 0 || !key[0] || ntargets >= MAX_TARGETS) return i;
    strncpy(tkey[ntargets], key, 30);
    tkey[ntargets][30] = 0;
    return ntargets++;
}

static void target_scan(const char *dir)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    if (fib && l && Examine(l, fib))
        while (ExNext(l, fib))
            if (fib->fib_DirEntryType < 0) target_add(fib->fib_FileName);
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
}

static const char *const *target_labels(void)
{
    tl_cur ^= 1;
    for (int i = 0; i < ntargets; i++) tlabels[tl_cur][i] = i ? tkey[i] : "All games and demos";
    tlabels[tl_cur][ntargets] = NULL;
    return tlabels[tl_cur];
}

/* Add...: a drawer requester for the game's or demo's drawer; the key is
   that drawer's own name, as the entry takes it (agaboot.c game_key) */
extern struct Library *AslBase;
static int pick_drawer(struct Window *w, char *key)
{
    static char last[256];
    int ok = 0;
    key[0] = 0;
    if (!(AslBase = OpenLibrary((STRPTR)"asl.library", 37))) return 0;
    struct FileRequester *fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (ULONG)w,
        ASLFR_TitleText, (ULONG)"The game's or demo's drawer",
        ASLFR_InitialDrawer, (ULONG)last,
        ASLFR_PositiveText, (ULONG)"Add",
        ASLFR_DrawersOnly, TRUE,
        TAG_DONE);
    if (fr && AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
        BPTR l = Lock(fr->fr_Drawer, ACCESS_READ);
        if (fib && l && Examine(l, fib)) {
            strncpy(key, fib->fib_FileName, 30);
            key[30] = 0;
            ok = key[0] != 0;
        }
        if (l) {
            BPTR up = ParentDir(l);              /* next time: where this one is */
            if (up) { NameFromLock(up, (STRPTR)last, sizeof last); UnLock(up); }
            UnLock(l);
        }
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    if (fr) FreeAslRequest(fr);
    CloseLibrary(AslBase);
    AslBase = NULL;
    return ok;
}

int picture_window(void)
{
    static const char *size_names[] = { "Full frame 800 x 286", "720 x 272", "640 x 256", "Custom", "Auto", NULL };
    static const char *const look_names[] = { "Sharp", "Smooth", "TV", NULL };
    static const char *const scale_names[] = { "Integer", "Fill screen", "4:3", "5:4 (square)", "16:9", "16:10",
                                               "21:9", "32:9", NULL };   /* in scale_row's order */
    static const char *const gamma_names[] = { "Exact", "CRT", "Lighter", NULL };
    static const char *const scan_names[] = { "Off", "25%", "50%", "75%", "100%", NULL };
    static const char *const shape_names[] = { "Even", "Crisp", "Whole lines", NULL };
    static const char *const flicker_names[] = { "On", "Off", NULL };
    static const char *const bright_names[] = { "Normal", "110%", "120%", "135%", "150%", NULL };
    static const char *const border_names[] = { "Off", "2%", "4%", "6%", "8%", "10%", NULL };
    static const char *const video_names[] = { "PAL", "NTSC", NULL };
    static const char *const rate_names[] = { "As config.txt says", "50 Hz (1080p50)", "60 Hz (1080p60)",
                                              "50.2 Hz (for sync)", NULL };
    static const char *const sync_names[] = { "Off", "Follow the Amiga", NULL };
    static const char *const nscan_names[] = { "As PAL", "Off", "25%", "50%", "75%", "100%", NULL };
    static const char *const labels[] = { "Settings for", "Size", "Look", "Scale", "Scanlines",
                                          "Scanlines (interlace)", "Scanline shape", "CRT mask", "Flicker fixer",
                                          "Brightness", "Gamma", "TV border", "Video", "HDMI rate *", "HDMI sync",
                                          "Scanlines (NTSC)", NULL };
    static const char *const headings[] = { "Picture", "CRT look", "This Amiga  (* from the next power cycle)", NULL };
    static char msg[400];
    int rate0, sync0;
    static const char *const buttons[] = { "Save", "Use", "Help", "Cancel", NULL };
    static const UWORD button_id[4] = { G_SAVE, G_USE, G_HELP, G_CANCEL };
    static char help[sizeof img_help];
    static const char *const side_buttons[] = { "Add...", "Remove", "Crop...", "Position...", NULL };
    static char game[32], key[32];
    static const char *const *target_names;         /* static: see picture_load */
    static struct aga_picture orig, pic[MAX_TARGETS];
    static int touched[MAX_TARGETS];
    static struct Gadget *cyc[N_CYCLES];
    static int active[N_CYCLES];
    static struct NewGadget ng;
    static const char *const *names[N_CYCLES] = { NULL, size_names, look_names, scale_names,
                                                  scan_names, scan_names, shape_names, scan_names,
                                                  flicker_names, bright_names, gamma_names, border_names,
                                                  video_names, rate_names, sync_names, nscan_names };
    struct libs l;
    int target = 0;
    struct Screen *scr = NULL;
    APTR vi = NULL;
    struct Gadget *glist = NULL, *g, *remove = NULL, *custom = NULL;
    struct Window *win = NULL;
    int rc = 20;

    ntargets = 1;                        /* All games and demos */
    rate0 = hdmirate_pref();
    win_rate = rate0 == 50 ? 1 : rate0 == 60 ? 2 : rate0 ? 3 : 0;
    win_sync = sync0 = hdmisync_pref();
    if (!libs_open(&l, 1)) goto out;
    has_crop = picture_has_crop();
    size_names[3] = has_crop ? "Custom" : NULL;          /* an older kernel shows Custom and Auto as Full */
    picture_load(&orig, 0);              /* games get FULL when nothing is saved */
    target_scan("ENVARC:AGA-PISTORM/Games");
    target_scan("ENV:AGA-PISTORM/Games");
    for (int i = 2; i < ntargets; i++)       /* in name order */
        for (int j = i; j > 1 && stricmp(tkey[j - 1], tkey[j]) > 0; j--) {
            strcpy(key, tkey[j]); strcpy(tkey[j], tkey[j - 1]); strcpy(tkey[j - 1], key);
        }
    game[0] = 0;
    if (GetVar((STRPTR)LASTGAME_VAR, (STRPTR)game, sizeof game, GVF_GLOBAL_ONLY) > 0 && game[0])
        target_add(game);                    /* the one started last, own picture or not */
    for (int i = 0; i < ntargets; i++) {
        pic[i] = orig;
        if (i) picture_load_game(&pic[i], tkey[i]);   /* its own, or all games' to start from */
        touched[i] = 0;
    }
    target_names = names[0] = target_labels();
    if (!(scr = LockPubScreen(NULL))) goto out;
    if (!(vi = GetVisualInfo(scr, TAG_DONE))) goto out;

    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize;
    int lw = widest(srp, labels) + 12;
    int cw = 0, t;
    for (int i = 0; i < N_CYCLES; i++) if ((t = widest(srp, names[i])) > cw) cw = t;
    cw += 40;                            /* the cycle glyph */
    int bw = widest(srp, buttons) + 24;
    if ((t = widest(srp, side_buttons) + 16) > lw - 8) lw = t + 8;   /* Add... and Crop... under the labels */
    static const char *const crt_left[] = { "1084S look", "Custom look", NULL };          /* under CRT look */
    static const char *const crt_right[] = { "No CRT look", "Save as Custom look", NULL };
    if ((t = widest(srp, crt_left) + 16) > lw - 8) lw = t + 8;
    if ((t = widest(srp, crt_right) + 16) > cw) cw = t;
    int gh = fh + 6, gap = 4, rh = gh + gap;
    int left = scr->WBorLeft + 12, top = scr->WBorTop + fh + 1 + 10;
    int iw = lw + cw + 24;
    if (4 * bw + 3 * 12 + 24 > iw) iw = 4 * bw + 3 * 12 + 24;
    if ((t = widest(srp, headings) + 24) > iw) iw = t;
    /* top to bottom: Settings for, Add... and Remove under it; Picture's
       heading, its rows, Crop... and Position...; CRT look's heading, rows,
       1084S look and No CRT look, Custom look and Save as Custom look; This
       Amiga's heading and rows; the buttons */
    int hy_pic = top + 2 * rh + 8;
    int py = hy_pic + rh * (1 + N_ROWS_PIC);
    int hy_crt = py + rh + 4;
    int cy = hy_crt + rh * (1 + N_ROWS_CRT);
    int hy_ami = cy + 2 * rh + 4;
    int heading_y[3] = { hy_pic, hy_crt, hy_ami };

    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_Height = gh;
    g = CreateContext(&glist);
    cycles_of(&pic[0], active);
    active[0] = 0;
    for (int i = 0; i < N_CYCLES; i++) {
        int s = row_slot(G_TARGET + i);
        ng.ng_LeftEdge = left + lw;
        ng.ng_TopEdge = i == 0 ? top
                      : s < N_ROWS_PIC ? hy_pic + rh * (1 + s)
                      : s < N_ROWS_PIC + N_ROWS_CRT ? hy_crt + rh * (1 + s - N_ROWS_PIC)
                      : hy_ami + rh * (1 + s - N_ROWS_PIC - N_ROWS_CRT);
        ng.ng_Width = cw;
        ng.ng_GadgetText = (UBYTE *)labels[i];
        ng.ng_GadgetID = G_TARGET + i;
        ng.ng_Flags = PLACETEXT_LEFT;
        g = cyc[i] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)names[i], GTCY_Active, active[i],
                                  GA_Disabled, i == C_SSHAPE && !pic[0].scan && !pic[0].lscan && pic[0].nscan <= 0,
                                  TAG_DONE);
    }
    /* the headings: text only, in the screen's font made bold where it has
       a bold face */
    static struct TextAttr bold;
    bold = *scr->Font;
    bold.ta_Style |= FSF_BOLD;
    ng.ng_TextAttr = &bold;
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = left;
        ng.ng_TopEdge = heading_y[i];
        ng.ng_Width = iw - 24;
        ng.ng_GadgetText = NULL;
        ng.ng_GadgetID = 0;
        ng.ng_Flags = 0;
        g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)headings[i], TAG_DONE);
    }
    ng.ng_TextAttr = scr->Font;
    ng.ng_Flags = PLACETEXT_IN;
    /* Add... under the labels and Remove under the choices, below Settings
       for; Crop... and Position... the same way at the end of Picture */
    static const UWORD side_id[4] = { G_ADD, G_REMOVE, G_CROP, G_POS };
    for (int i = 0; i < 4; i++) {
        ng.ng_LeftEdge = (i & 1) ? left + lw : left;
        ng.ng_TopEdge = i < 2 ? top + rh : py;
        ng.ng_Width = (i & 1) ? cw : lw - 8;
        ng.ng_GadgetText = (UBYTE *)side_buttons[i];
        ng.ng_GadgetID = side_id[i];
        g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, i == 1 || (i == 2 && !has_crop), TAG_DONE);
        if (i == 1) remove = g;
    }
    /* 1084S look and No CRT look under CRT look's rows, Custom look and Save
       as Custom look under them (RGB2RTG 0.76-dev83), placed as Crop... and
       Position... are under Picture's; Custom look greyed until one is saved */
    static const UWORD crt_id[4] = { G_CRT1084S, G_CRTNONE, G_CRTCUSTOM, G_CRTKEEP };
    static const char *const crt_text[4] = { "1084S look", "No CRT look", "Custom look", "Save as Custom look" };
    for (int i = 0; i < 4; i++) {
        ng.ng_LeftEdge = (i & 1) ? left + lw : left;
        ng.ng_TopEdge = cy + (i >> 1) * rh;
        ng.ng_Width = (i & 1) ? cw : lw - 8;
        ng.ng_GadgetText = (UBYTE *)crt_text[i];
        ng.ng_GadgetID = crt_id[i];
        g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, i == 2 && !picture_crt_custom_has(), TAG_DONE);
        if (i == 2) custom = g;
    }
    int by = hy_ami + rh * 3 + 8;
    int step = (iw - 24 - bw) / 3;
    for (int i = 0; i < 4; i++) {
        ng.ng_LeftEdge = left + i * step;
        ng.ng_TopEdge = by;
        ng.ng_Width = bw;
        ng.ng_GadgetText = (UBYTE *)buttons[i];
        ng.ng_GadgetID = button_id[i];
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    if (!g) goto out;

    int ih = by + gh + 10 - (scr->WBorTop + fh + 1);
    win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - RTG Image",
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
            struct aga_picture *cur = &pic[target];
            if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
            } else if (cls == IDCMP_CLOSEWINDOW) {
                picture_send(&orig);
                picture_border_desktop(orig.bx, orig.by);
                if (win_sync != sync0) hdmisync_send(sync0);
                done = 1;
            } else if (cls == IDCMP_GADGETUP) {
                int id = gad->GadgetID, changed = 1;
                switch (id) {
                case G_TARGET:
                    target = code < ntargets ? code : 0;
                    cur = &pic[target];
                    cycles_of(cur, active);
                    for (int i = 1; i < N_CYCLES; i++)
                        GT_SetGadgetAttrs(cyc[i], win, NULL, GTCY_Active, active[i], TAG_DONE);
                    shape_avail(win, cyc[C_SSHAPE], &pic[target]);
                    GT_SetGadgetAttrs(remove, win, NULL, GA_Disabled, target == 0, TAG_DONE);
                    picture_send(cur);
                    changed = 0;
                    break;
                case G_SIZE:    cur->size = code; break;
                case G_LOOK:    cur->look = code; break;
                case G_SCALE:   cur->scale = scale_row[code < 8 ? code : 2]; break;
                case G_SCAN:    cur->scan = code; shape_avail(win, cyc[C_SSHAPE], cur); break;
                case G_LSCAN:   cur->lscan = code; shape_avail(win, cyc[C_SSHAPE], cur); break;
                case G_NSCAN:   cur->nscan = code - 1; shape_avail(win, cyc[C_SSHAPE], cur); break;
                case G_SSHAPE:  cur->sshape = code; break;
                case G_MASK:    cur->mask = code; break;
                case G_FLICKER: cur->flicker = code == 0; break;
                case G_BRIGHT:  cur->bright = code; break;
                case G_GAMMA:   cur->gamma = code; break;
                case G_BORDER:
                    cur->bx = cur->by = code * 2;
                    if (target == 0) picture_border_desktop(cur->bx, cur->by);   /* the desktop is all games' */
                    break;
                case G_VIDEO:   cur->ntsc = code == 1; break;
                case G_RATE:    win_rate = code; changed = 0; break;     /* written by Save or Use */
                case G_SYNC:                                             /* heard at once in System wide */
                    win_sync = code;
                    hdmisync_send(win_sync);
                    changed = 0;
                    break;
                case G_POS:     changed = picture_position(cur); break;
                case G_CROP:
                    changed = picture_crop(cur);
                    if (changed) GT_SetGadgetAttrs(cyc[1], win, NULL, GTCY_Active, cur->size, TAG_DONE);   /* Custom */
                    break;
                case G_ADD:
                    /* a game or demo of the user's choice: from its own
                       picture if it has one, else from all games' */
                    changed = 0;
                    if (!pick_drawer(win, key)) break;
                    if ((t = target_find(key)) < 0) {
                        if ((t = target_add(key)) < 0) {
                            say(win, "Settings for: the list is full.", "OK");
                            break;
                        }
                        pic[t] = pic[0];
                        picture_load_game(&pic[t], key);
                        touched[t] = 1;              /* Save or Use keeps it, changed or not */
                        target_names = target_labels();
                        GT_SetGadgetAttrs(cyc[0], win, NULL, GTCY_Labels, (ULONG)target_names, TAG_DONE);
                    }
                    target = t;
                    cur = &pic[target];
                    GT_SetGadgetAttrs(cyc[0], win, NULL, GTCY_Active, target, TAG_DONE);
                    cycles_of(cur, active);
                    for (int i = 1; i < N_CYCLES; i++)
                        GT_SetGadgetAttrs(cyc[i], win, NULL, GTCY_Active, active[i], TAG_DONE);
                    shape_avail(win, cyc[C_SSHAPE], &pic[target]);
                    GT_SetGadgetAttrs(remove, win, NULL, GA_Disabled, FALSE, TAG_DONE);
                    picture_send(cur);
                    break;
                case G_REMOVE:
                    /* the game or demo gets the all-games picture again */
                    picture_forget_game(tkey[target]);
                    pic[target] = pic[0];
                    touched[target] = 0;
                    target = 0;
                    GT_SetGadgetAttrs(cyc[0], win, NULL, GTCY_Active, 0, TAG_DONE);
                    cycles_of(&pic[0], active);
                    for (int i = 1; i < N_CYCLES; i++)
                        GT_SetGadgetAttrs(cyc[i], win, NULL, GTCY_Active, active[i], TAG_DONE);
                    shape_avail(win, cyc[C_SSHAPE], &pic[target]);
                    GT_SetGadgetAttrs(remove, win, NULL, GA_Disabled, TRUE, TAG_DONE);
                    changed = 0;
                    break;
                case G_SAVE:
                case G_USE: {
                    int save = id == G_SAVE;
                    if (touched[0] || target == 0) picture_store(&pic[0], save);
                    for (int i = 1; i < ntargets; i++)
                        if (touched[i] || target == i) picture_store_game(&pic[i], tkey[i], save);
                    picture_send(&pic[0]);       /* the desktop's own until a game starts */
                    picture_border_desktop(pic[0].bx, pic[0].by);
                    hdmisync_store(win_sync, save);
                    {   /* the HDMI rate: config.txt, for good, either way */
                        int rate = win_rate == 1 ? 50 : win_rate == 2 ? 60 : win_rate == 3 ? 502 : 0;   /* 502: 50.2 Hz */
                        if (rate != rate0 && hdmirate_set(rate, msg, sizeof msg) && msg[0]) say(win, msg, "OK");
                    }
                    done = 1;
                    changed = 0;
                    break;
                }
                case G_CANCEL:
                    picture_send(&orig);
                    picture_border_desktop(orig.bx, orig.by);
                    if (win_sync != sync0) hdmisync_send(sync0);
                    done = 1;
                    changed = 0;
                    break;
                case G_CRTKEEP:
                    /* the target's look as it is now, kept as the user's own */
                    say(win, picture_crt_keep(cur) ? "This CRT look is saved as your Custom look:\n"
                                                     "Custom look puts it back, for any game."
                                                   : "The Custom look could not be written\n"
                                                     "(ENVARC:AGA-PISTORM/CRTLook).", "OK");
                    GT_SetGadgetAttrs(custom, win, NULL, GA_Disabled, !picture_crt_custom_has(), TAG_DONE);
                    changed = 0;
                    break;
                case G_CRT1084S:
                case G_CRTNONE:
                case G_CRTCUSTOM:
                    /* the rows set at once, for the target - shown, and kept by
                       Save or Use as a change by hand is */
                    if (id == G_CRT1084S) picture_crt_1084s(cur);
                    else if (id == G_CRTNONE) picture_crt_none(cur);
                    else if (!picture_crt_custom(cur)) { changed = 0; break; }
                    cycles_of(cur, active);
                    for (int i = 1; i < N_CYCLES; i++)
                        GT_SetGadgetAttrs(cyc[i], win, NULL, GTCY_Active, active[i], TAG_DONE);
                    shape_avail(win, cyc[C_SSHAPE], cur);
                    break;
                case G_HELP:
                    strcpy(help, img_help);              /* the same size: it fits */
                    wrap(help, 70);
                    help_request(win, "AGA-PISTORM - RTG Image: help", help);
                    changed = 0;
                    break;
                default: changed = 0; break;
                }
                if (changed) {
                    touched[target] = 1;
                    picture_send(&pic[target]);
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
