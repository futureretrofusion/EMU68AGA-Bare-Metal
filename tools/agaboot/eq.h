/* agaboot - the Chipset menu's Equalizer (eq.c): the effects on sound over HDMI
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_EQ_H
#define AGABOOT_EQ_H

#include <exec/types.h>
#include <intuition/screens.h>

#define EQ_BANDS 10

/* what arm/aga_fx.h runs: the equalizer's part (on, bands, preamp,
   crossfeed - presets set these) and two of their own (limiter, balance) */
struct eq_set {
    BYTE on;                    /* the equalizer, its preamp and crossfeed */
    BYTE gain[EQ_BANDS];        /* -12..12 dB: 31, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz */
    BYTE pre;                   /* preamp, -12..12 dB */
    BYTE xfeed;                 /* headphone crossfeed: 0 off, 1 light, 2 strong */
    BYTE lim;                   /* limiter */
    BYTE bal;                   /* balance, -12 (left) .. 12 (right): the other side down by that many dB */
    BYTE preset;                /* which preset it came from (eq_preset_name), 0 Custom */
};

void eq_load(struct eq_set *e);                 /* ENV:AGA_EQ; off and flat when not set */
void eq_store(const struct eq_set *e, int save);/* ENV: (and ENVARC: when save) */
void eq_send(const struct eq_set *e);           /* every setting to the kernel */
int  eq_is_set(const struct eq_set *e);         /* anything not off or flat */
const char *eq_preset_name(int i);              /* NULL past the last */
int  eq_parse_preset(const char *w);            /* "Rock", "bassboost", "user1": index, -1 */
void eq_preset(struct eq_set *e, int i);        /* bands, preamp and crossfeed from preset i, on */
void eq_describe(const struct eq_set *e, char *o, int len);   /* "on, Rock, preamp -5 dB, limiter on" */

/* the window on its own (Sound window, agaboot SOUND EQ ASK): changes are
   heard at once; 0 Cancel (the old ones are sent back), 1 Use, 2 Save -
   stored already */
int  eq_window(struct eq_set *e);

/* the window beside the Sound monitor, served by its loop */
struct eqw;
struct eqw *eqw_open(struct Screen *scr, struct eq_set *e);
ULONG eqw_sigs(struct eqw *w);
int  eqw_handle(struct eqw *w);                 /* 0 still open; 1 Cancel, 2 Use, 3 Save: done and stored */
void eqw_close(struct eqw *w);
void eqw_front(struct eqw *w);

#endif
