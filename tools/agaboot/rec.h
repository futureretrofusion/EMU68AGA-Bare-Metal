/* agaboot - the Sound monitor's recorder (rec.c): what HDMI plays, and each of
 * Paula's voices on its own, into WAV files
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_REC_H
#define AGABOOT_REC_H

#include <exec/types.h>
#include <dos/dos.h>

struct rec {
    BPTR fh[5];                 /* 0 the mix (16-bit stereo), 1-4 the voices (16-bit mono) */
    ULONG bytes[5];             /* data written to each */
    ULONG rate, vrate;
    ULONG rd, vrd;              /* the monitor's ring counts read up to */
    ULONG dropped;              /* frames the disk was too slow for */
    int on, voices, err;
    char path[160];             /* the mix's file, for the status line */
    UBYTE buf[16384];
};

/* folder dir (NULL: ENV:AGA_RECDIR, else RAM:), the mix at rate, and with
   voices the four voices at vrate: AGA-rec-NNN.wav and AGA-rec-NNN-v1..4.wav
   at the first NNN not taken. 1 recording, 0 with msg saying why not */
int  rec_start(struct rec *r, const char *dir, ULONG rate, int voices, ULONG vrate, char *msg, int len);
void rec_mix(struct rec *r, const ULONG *frames, ULONG n);     /* L in the top word, big-endian 16-bit */
void rec_voices(struct rec *r, const ULONG *words, ULONG n);   /* voice 0 in the top byte */
void rec_stop(struct rec *r);                                  /* the headers' sizes, and closed */
/* ask for the folder (asl.library), kept in ENV: and ENVARC:AGA_RECDIR; 0 cancelled */
int  rec_ask_dir(struct Window *win, char *dir, int len);
void rec_dir(char *dir, int len);                              /* the folder as kept, RAM: when none */

#endif
