/* agaboot - the AGA picture on HDMI (picture.c): size, look, scale,
 * scanlines, position, since 1.114 a custom crop, and since 1.110 the flicker
 * fixer, scanlines for interlaced screens, brightness, the TV border and the
 * standard games start in - for all games, or for one. Get one of these
 * wrong and the picture looks like shit.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_PICTURE_H
#define AGABOOT_PICTURE_H

struct aga_picture {
    int size;       /* 0 FULL (800 x 286), 1 720x272, 2 640x256, 3 CUSTOM (cut, below) */
    int look;       /* 0 SHARP, 1 SMOOTH, 2 TV */
    int scale;      /* 0 4:3, 1 INTEGER, 2 FILL, 3 5:4 (square pixels), 4 16:9, 5 16:10, 6 21:9, 7 32:9 */
    int scan;       /* scanlines: 0 off, 1-4 = 25% to 100% dark */
    int x, y;       /* the picture right / down: hires pixels, lines */
    int lscan;      /* scanlines for interlaced screens shown woven, as scan */
    int flicker;    /* the flicker fixer: 1 on (the default), 0 off */
    int bright;     /* brightness: 0 normal, 1-4 = 110, 120, 135, 150% */
    int bx, by;     /* the TV border across / down: 0 off, 1-10% */
    int ntsc;       /* the standard games start in: 0 PAL, 1 NTSC */
    int cut[2][4];  /* CUSTOM's edges, PAL's and NTSC's: left, top, right, bottom cut off
                       the 800 x 286 frame (NTSC's 800 x 237), hires pixels and lines */
    int mask;       /* the CRT mask, an aperture grille: 0 off, 1-4 = 25% to 100% dark */
    int sshape;     /* the scanlines' shape: 0 Even (the default), 1 Crisp, 2 Whole lines */
    int gamma;      /* 0 Exact (the default), 1 CRT, 2 Lighter */
    int nscan;      /* NTSC screens' scanlines (1.118): -1 as scan (the default), 0 off, 1-4 = 25% to 100% */
};

#define PICTURE_TEXT 256     /* picture_format's buffer, at least: its longest is ~180 since 1.116 */

void picture_default(struct aga_picture *p, int size);
int  picture_parse(const char *s, struct aga_picture *p);      /* words understood; 0 = none */
void picture_format(const struct aga_picture *p, char *buf);   /* at least PICTURE_TEXT bytes */
void picture_load(struct aga_picture *p, int size_default);    /* defaults, then ENV:AGA_ZOOM */
void picture_store(const struct aga_picture *p, int save);     /* ENV:, and ENVARC: when save */
void picture_words(const struct aga_picture *p);               /* the control words, caller Disable()s */
void picture_send(const struct aga_picture *p);                /* the same, Disable()d here */
int  picture_window(void);                                      /* Chipset menu - RTG Image */
int  picture_border_desktop(int bx, int by);                    /* the TV border on the RTG desktop: 1 if the card took it */
int  picture_position(struct aga_picture *p);                   /* Position...: 1 when kept */
int  picture_crop(struct aga_picture *p);                       /* Crop...: 1 when kept (size CUSTOM) */
int  picture_has_crop(void);                                    /* the running kernel cuts CUSTOM edges */

/* One game's own picture, kept as ENV(ARC):AGA-PISTORM/Games/<key> - the key
   is the name of the game's drawer, the one WHDLoad starts it in. A game with
   one gets it instead of AGA_ZOOM when it starts. */
int  picture_load_game(struct aga_picture *p, const char *key); /* 1 if the game has its own */
int  picture_store_game(const struct aga_picture *p, const char *key, int save);
void picture_forget_game(const char *key);
/* CRT look in one (1.118, RGB2RTG 0.76-dev83): a Commodore 1084S's, none, or
   the user's own (ENV(ARC):AGA-PISTORM/CRTLook) - RTG Image's buttons under
   CRT look, ZOOM's CRTLOOK=1084S|NONE|CUSTOM|SAVE */
void picture_crt_1084s(struct aga_picture *p);
void picture_crt_none(struct aga_picture *p);
int  picture_crt_custom(struct aga_picture *p);                 /* 0: none saved */
int  picture_crt_custom_has(void);
int  picture_crt_keep(const struct aga_picture *p);             /* 0: could not be written */
#define LASTGAME_VAR "AGA_LASTGAME"  /* ENV: the key of the game started last */

#endif
