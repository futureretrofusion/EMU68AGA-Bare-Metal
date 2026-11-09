/* agaboot - the game's picture on the Amiga's own RGB port (rgbport.c)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_RGBPORT_H
#define AGABOOT_RGBPORT_H

struct rgb_port {
    int mode;           /* 0 off, 1 HAM6 with HDMI as usual, 2 HAM6 with HDMI black */
    int size;           /* 0 384x286 (all of it), 1 368x280, 2 352x272, 3 320x256 */
    int pace;           /* 0 as many pictures as get through, 1 a steady 25 a second, 2 Auto: a steady 25 while 50 does not get through */
    int exact;          /* 1: the colours rounded into 4 bits (the default); 0: dithered - smooth, true hues */
    int c32;            /* 1: 32 colours, five planes - fewer words for the bus, smoother motion; 0: HAM6 (the default) */
    int x, y;           /* the picture right / down on the RGB monitor: lores pixels, lines */
};

void rgbport_load(struct rgb_port *p);
void rgbport_store(const struct rgb_port *p, int save);      /* ENV:, and ENVARC: when save */
int  rgbport_parse(const char *s, struct rgb_port *p);        /* words; returns how many it knew */
void rgbport_format(const struct rgb_port *p, char *buf);     /* at least 64 bytes (53 at most) */
void rgbport_entry(void);       /* sandbox entry, before the enter word: lend the chip RAM, send the words */
void rgbport_leave(void);       /* after the leave: free that chip RAM */
int  rgbport_window(void);      /* Chipset menu - RGB port */
int  rgbport_position(struct rgb_port *p);   /* the calibration frame on the RGB port; 1 when kept */

#endif
