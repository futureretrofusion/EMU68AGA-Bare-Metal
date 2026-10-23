/* agaboot - the Sound monitor's recorder: what HDMI plays, as the screen's
 * speakers get it (after the mix, the volumes, filters and effects), into a
 * WAV file - and, while a game runs in the sandbox, each of Paula's four
 * voices into a file of its own: a game's music taken apart into its
 * channels, as no recording of the speakers could.
 *
 * The monitor reads its rings (monitor.c) and hands the new frames here at
 * every tick; a disk too slow for the ring's 0.3 to 1.4 s loses frames,
 * counted in dropped. The mix is 16 bits at HDMI's rate - its top 16, the
 * monitor's - the voices 16 bits (their 8 bits times the volume) at the
 * mix's own rate, 27,928 a second on a PAL Amiga.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <string.h>
#include <stdio.h>
#include "rec.h"

#define DIR_VAR "AGA_RECDIR"

static void le16(UBYTE *p, ULONG v) { p[0] = (UBYTE)v; p[1] = (UBYTE)(v >> 8); }
static void le32(UBYTE *p, ULONG v) { le16(p, v); le16(p + 2, v >> 16); }

static void wav_header(UBYTE *h, ULONG rate, int ch, ULONG data)
{
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);                                   /* PCM */
    le16(h + 22, (ULONG)ch);
    le32(h + 24, rate);
    le32(h + 28, rate * 2 * (ULONG)ch);
    le16(h + 32, 2 * (ULONG)ch);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

void rec_dir(char *dir, int len)
{
    if (GetVar((STRPTR)DIR_VAR, (STRPTR)dir, len, 0) <= 0) strncpy(dir, "RAM:", len);
}

int rec_ask_dir(struct Window *win, char *dir, int len)
{
    struct Library *AslBase = OpenLibrary((STRPTR)"asl.library", 38);
    int ok = 0;
    if (!AslBase) return 0;
    rec_dir(dir, len);
    struct FileRequester *fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (ULONG)win, ASLFR_TitleText, (ULONG)"Record into which drawer?",
        ASLFR_InitialDrawer, (ULONG)dir, ASLFR_DrawersOnly, TRUE, ASLFR_DoSaveMode, TRUE, TAG_DONE);
    if (fr) {
        if (AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
            strncpy(dir, (const char *)fr->fr_Drawer, len - 1);
            dir[len - 1] = 0;
            SetVar((STRPTR)DIR_VAR, (STRPTR)dir, -1, GVF_GLOBAL_ONLY);
            SetVar((STRPTR)DIR_VAR, (STRPTR)dir, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
            ok = 1;
        }
        FreeAslRequest(fr);
    }
    CloseLibrary(AslBase);
    return ok;
}

static void fail(struct rec *r)
{
    r->err = 1;
    rec_stop(r);
}

int rec_start(struct rec *r, const char *dir, ULONG rate, int voices, ULONG vrate, char *msg, int len)
{
    char d[128], name[40], p[160];
    UBYTE h[44];
    memset(r, 0, sizeof *r);
    if (dir) { strncpy(d, dir, sizeof d - 1); d[sizeof d - 1] = 0; }
    else rec_dir(d, sizeof d);
    int nr = 1;
    for (; nr < 1000; nr++) {                          /* the first number not taken */
        sprintf(name, "AGA-rec-%03d.wav", nr);
        strcpy(p, d);
        AddPart((STRPTR)p, (STRPTR)name, sizeof p);
        BPTR l = Lock((STRPTR)p, ACCESS_READ);
        if (!l) break;
        UnLock(l);
    }
    r->rate = rate;
    r->vrate = vrate;
    r->voices = voices;
    for (int i = 0; i < (voices ? 5 : 1); i++) {
        if (i) sprintf(name, "AGA-rec-%03d-v%d.wav", nr, i);
        else sprintf(name, "AGA-rec-%03d.wav", nr);
        strcpy(p, d);
        AddPart((STRPTR)p, (STRPTR)name, sizeof p);
        if (!i) strcpy(r->path, p);
        if (!(r->fh[i] = Open((STRPTR)p, MODE_NEWFILE))) {
            snprintf(msg, len, "could not make %s", p);
            rec_stop(r);
            return 0;
        }
        wav_header(h, i ? vrate : rate, i ? 1 : 2, 0);   /* sizes put right at the end */
        if (Write(r->fh[i], h, 44) != 44) {
            snprintf(msg, len, "could not write to %s", p);
            rec_stop(r);
            return 0;
        }
    }
    r->on = 1;
    return 1;
}

static void out(struct rec *r, int i, ULONG n)
{
    if (!r->on || !n) return;
    if (Write(r->fh[i], r->buf, (LONG)n) != (LONG)n) { fail(r); return; }
    r->bytes[i] += n;
}

void rec_mix(struct rec *r, const ULONG *f, ULONG n)
{
    while (r->on && n) {
        ULONG k = n > sizeof r->buf / 4 ? sizeof r->buf / 4 : n;
        UBYTE *b = r->buf;
        for (ULONG i = 0; i < k; i++, b += 4) {        /* big-endian L, R to little-endian */
            ULONG v = f[i];
            b[0] = (UBYTE)(v >> 16); b[1] = (UBYTE)(v >> 24);
            b[2] = (UBYTE)v;         b[3] = (UBYTE)(v >> 8);
        }
        out(r, 0, k * 4);
        f += k;
        n -= k;
    }
}

void rec_voices(struct rec *r, const ULONG *w, ULONG n)
{
    if (!r->voices) return;
    while (r->on && n) {
        ULONG k = n > sizeof r->buf / 2 ? sizeof r->buf / 2 : n;
        for (int v = 0; v < 4 && r->on; v++) {
            UBYTE *b = r->buf;
            for (ULONG i = 0; i < k; i++, b += 2) {    /* 8 bits times the volume, as 16 */
                b[0] = 0;
                b[1] = (UBYTE)(w[i] >> (24 - 8 * v));
            }
            out(r, 1 + v, k * 2);
        }
        w += k;
        n -= k;
    }
}

void rec_stop(struct rec *r)
{
    UBYTE h[44];
    for (int i = 0; i < 5; i++) {
        if (!r->fh[i]) continue;
        if (!r->err) {                                 /* the sizes, now they are known */
            wav_header(h, i ? r->vrate : r->rate, i ? 1 : 2, r->bytes[i]);
            Seek(r->fh[i], 0, OFFSET_BEGINNING);
            Write(r->fh[i], h, 44);
        }
        Close(r->fh[i]);
        r->fh[i] = 0;
    }
    r->on = 0;
}
