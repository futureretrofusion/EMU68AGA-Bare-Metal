/* agaboot - HiFi sound (Chipset menu - Sound, 1.114): what AHI programs
 * play - music players, trackers, the desktop's sounds - mixed by AHI in 32
 * bits and played straight on HDMI in 24 (16 on a screen that takes no
 * more), at the program's own rate, not through Paula's four 8-bit channels
 * (tools/ahi/aga-hdmi.c, the kernel's arm/aga_hdmi.c "AHI"). About damned
 * time.
 *
 * Off by default. On:
 *   - the driver and its modes into DEVS:AHI/aga-hdmi.audio and
 *     DEVS:AudioModes/AGA-HDMI - agaboot carries both (ahi16_blob.h, made by
 *     tools/ahi/mkblob.py), written when missing or not the same - and
 *     AddAudioModes REFRESH, so AHI knows them without a reboot;
 *   - AHI's unit 0 (what most programs open) and its Music unit (AHI's
 *     default mode, what Prefs/AHI's Play uses) at "AGA-PISTORM HDMI:HiFi 24
 *     bit stereo++", 48 kHz, in ENV: and ENVARC:Sys/ahi.prefs - with the
 *     file as it was kept once in ENVARC:AGA-PISTORM/ahi.prefs.before;
 *   - ENVARC:AGA_AHI16 ON, so it stays on - every install after keeps it.
 * On, low latency (1.116): the same with "AGA-PISTORM HDMI:HiFi 24 bit
 * stereo++ low latency" (driver 6.2: about a buffer and 12 ms in the ring,
 * the kernel starting it at 10 ms - for games), AGA_AHI16 LOWLATENCY.
 * Off: every unit at one of my modes gets the mode and frequency it had
 * before (the kept file's; another of its modes, Paula's as a rule, when it
 * had none of its own), the mode file goes, the variable too. The driver
 * stays: without its mode file nobody opens it. Other units and settings
 * changed meanwhile in Prefs/AHI are left alone.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include "ahi16.h"
#include "ahi16_blob.h"

#define AHI16_VAR  "AGA_AHI16"
#define DRV_PATH   "DEVS:AHI/aga-hdmi.audio"
#define MODE_PATH  "DEVS:AudioModes/AGA-HDMI"
#define PREFS_ENV  "ENV:Sys/ahi.prefs"
#define PREFS_ARC  "ENVARC:Sys/ahi.prefs"
#define BACKUP     "ENVARC:AGA-PISTORM/ahi.prefs.before"
#define MODE_HIFI  0x0E680002UL
#define MODE_LOWLAT 0x0E680004UL          /* ...the same with low latency (driver 6.2) */
#define OURS(m)    (((m) >> 16) == 0x0E68)
#define D_AHI      511                  /* the kernel's AHI counter: bit 0 understood */
#define MUSIC_UNIT 255                  /* AHI_NO_UNIT: Prefs/AHI's "Music unit" */

ULONG diag_read(UWORD sel);             /* agaboot.c */
void  kernel_boards(int *built, int *runs);

/* the kernel running here takes AHI's ring: mine (a stock Emu68's counters
   are noise), and its AHI counter says it understands */
static int kernel_ahi(void)
{
    int built, runs;
    kernel_boards(&built, &runs);
    return built && (diag_read(D_AHI) & 1);
}

int ahi16_pref(void)
{
    char v[16];
    if (GetVar((STRPTR)AHI16_VAR, (STRPTR)v, sizeof v, 0) <= 0) return 0;
    return !stricmp(v, "ON") ? 1 : !stricmp(v, "LOWLATENCY") ? 2 : 0;
}

static int exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

/* a whole file into AllocVec'd memory; NULL when there is none */
static UBYTE *read_file(const char *path, LONG *len)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) return NULL;
    Seek(f, 0, OFFSET_END);
    LONG n = Seek(f, 0, OFFSET_BEGINNING);
    UBYTE *b = n > 0 && n < 65536 ? AllocVec(n + 64, MEMF_PUBLIC | MEMF_CLEAR) : NULL;
    if (b && Read(f, b, n) != n) { FreeVec(b); b = NULL; }
    Close(f);
    *len = b ? n : 0;
    return b;
}

static int write_file(const char *path, const void *data, LONG len)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    if (!f) return 0;
    LONG w = Write(f, (APTR)data, len);
    Close(f);
    return w == len;
}

/* the file there already, byte for byte */
static int same_file(const char *path, const void *data, LONG len)
{
    LONG n;
    UBYTE *b = read_file(path, &n);
    int same = b && n == len && !memcmp(b, data, len);
    if (b) FreeVec(b);
    return same;
}

static ULONG get32(const UBYTE *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }
static void put32(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16); p[2] = (UBYTE)(v >> 8); p[3] = (UBYTE)v; }

/* The AHIU chunks of an ahi.prefs (FORM PREF): struct AHIUnitPrefs - unit,
   a pad, channels, then the audio mode at 4 and the frequency at 8. Calls
   fn for each; returns how many there were, -1 when it is no prefs file. */
static int each_unit(UBYTE *b, LONG n, void (*fn)(UBYTE *u, void *ctx), void *ctx)
{
    if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "PREF", 4)) return -1;
    int units = 0;
    for (LONG o = 12; o + 8 <= n; ) {
        ULONG len = get32(b + o + 4);
        if (o + 8 + (LONG)len > n) break;
        if (!memcmp(b + o, "AHIU", 4) && len >= 12) {
            fn(b + o + 8, ctx);
            units++;
        }
        o += 8 + len + (len & 1);
    }
    return units;
}

/* The mode and frequency each unit had before (the kept file). A unit that
   was not there, or was at one of my modes already (picked by hand in
   Prefs/AHI before the switch), gets the first other mode the file has -
   Paula's, as a rule - or 0, AHI's default, when it has none. */
static ULONG before[256], before_hz[256], other_mode, other_hz;
static void note_before(UBYTE *u, void *ctx)
{
    (void)ctx;
    ULONG m = get32(u + 4);
    if (OURS(m)) return;
    before[u[0]] = m;
    before_hz[u[0]] = get32(u + 8);
    if (!other_mode && m) { other_mode = m; other_hz = get32(u + 8); }
}
static void note_other(UBYTE *u, void *ctx)
{
    (void)ctx;
    ULONG m = get32(u + 4);
    if (!OURS(m) && m && !other_mode) { other_mode = m; other_hz = get32(u + 8); }
}

struct edit { int on, changed, seen0, seen_music; };
static void edit_unit(UBYTE *u, void *ctx)
{
    struct edit *e = ctx;
    int unit = u[0];
    ULONG mode = get32(u + 4);
    if (unit == 0) e->seen0 = 1;
    if (unit == MUSIC_UNIT) e->seen_music = 1;
    ULONG want = e->on == 2 ? MODE_LOWLAT : MODE_HIFI;
    if (e->on && (unit == 0 || unit == MUSIC_UNIT)) {
        if (mode != want || get32(u + 8) != 48000) {
            put32(u + 4, want);
            put32(u + 8, 48000);
            e->changed = 1;
        }
    } else if (!e->on && OURS(mode)) {
        int had = before[unit] != 0;
        put32(u + 4, had ? before[unit] : other_mode);
        if (had || other_mode) put32(u + 8, had ? before_hz[unit] : other_hz);
        e->changed = 1;
    }
}

/* an AHIU chunk for a unit the file did not have: Prefs/AHI's own values,
   at my HiFi mode (or its low latency one) */
static LONG add_unit(UBYTE *b, LONG n, int unit, int level)
{
    static const UBYTE tmpl[8 + 32] = { 'A', 'H', 'I', 'U', 0, 0, 0, 32,
        0, 0, 0, 16,  0x0E, 0x68, 0, 2,  0, 0, 0xBB, 0x80,  0, 0, 0, 0,  0, 1, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
    memcpy(b + n, tmpl, sizeof tmpl);
    b[n + 8] = (UBYTE)unit;
    put32(b + n + 12, level == 2 ? MODE_LOWLAT : MODE_HIFI);
    if (unit == MUSIC_UNIT) b[n + 11] = 0;               /* channels: the Music unit has none of its own */
    n += sizeof tmpl;
    put32(b + 4, (ULONG)n - 8);                          /* the FORM's size */
    return n;
}

/* One ahi.prefs: On or Off as above. 1 changed, 0 nothing to do, -1 failed. */
static int edit_prefs(const char *path, int on)
{
    LONG n = 0;
    UBYTE *b = read_file(path, &n);
    if (!b) {
        if (!on) return 0;
        /* none yet (Prefs/AHI never saved): FORM PREF, its header, two units */
        b = AllocVec(256, MEMF_PUBLIC | MEMF_CLEAR);
        if (!b) return -1;
        static const UBYTE head[] = { 'F', 'O', 'R', 'M', 0, 0, 0, 18, 'P', 'R', 'E', 'F',
                                      'P', 'R', 'H', 'D', 0, 0, 0, 6, 0, 0, 0, 0, 0, 0 };
        memcpy(b, head, sizeof head);
        n = sizeof head;
    } else {
        UBYTE *big = AllocVec(n + 128, MEMF_PUBLIC | MEMF_CLEAR);   /* room for two more units */
        if (!big) { FreeVec(b); return -1; }
        memcpy(big, b, n);
        FreeVec(b);
        b = big;
    }
    struct edit e = { on, 0, 0, 0 };
    if (each_unit(b, n, edit_unit, &e) < 0) { FreeVec(b); return -1; }
    if (on && !e.seen0) { n = add_unit(b, n, 0, on); e.changed = 1; }
    if (on && !e.seen_music) { n = add_unit(b, n, MUSIC_UNIT, on); e.changed = 1; }
    int rc = 0;
    if (e.changed) rc = write_file(path, b, n) ? 1 : -1;
    FreeVec(b);
    return rc;
}

/* the kept file's modes, for Off */
static void load_before(void)
{
    LONG n;
    memset(before, 0, sizeof before);
    memset(before_hz, 0, sizeof before_hz);
    other_mode = other_hz = 0;
    UBYTE *b = read_file(BACKUP, &n);
    if (b) { each_unit(b, n, note_before, NULL); FreeVec(b); }
    /* nothing kept (no ahi.prefs at On): the other mode from what is there now */
    if (!other_mode && (b = read_file(PREFS_ENV, &n))) {
        each_unit(b, n, note_other, NULL);
        FreeVec(b);
    }
}

/* ENVARC:Sys/ahi.prefs as it is, once: what Off goes back to */
static void keep_before(void)
{
    if (exists(BACKUP)) return;
    LONG n;
    UBYTE *b = read_file(PREFS_ARC, &n);
    BPTR l = CreateDir((STRPTR)"ENVARC:AGA-PISTORM");
    if (l) UnLock(l);
    write_file(BACKUP, b, b ? n : 0);                    /* empty: there was none */
    if (b) FreeVec(b);
}

/* the kept file is empty: AHI had no settings file before On */
static int had_none(void)
{
    BPTR f = Open((STRPTR)BACKUP, MODE_OLDFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    LONG n = Seek(f, 0, OFFSET_BEGINNING);
    Close(f);
    return n == 0;
}

/* the driver and its modes in DEVS: as agaboot carries them; 0 fine, else
   the path that could not be written */
static const char *put_files(void)
{
    int wrote = 0;
    if (!same_file(DRV_PATH, ahi16_driver, sizeof ahi16_driver)) {
        if (!write_file(DRV_PATH, ahi16_driver, sizeof ahi16_driver)) return DRV_PATH;
        wrote = 1;
    }
    if (!same_file(MODE_PATH, ahi16_modefile, sizeof ahi16_modefile)) {
        if (!write_file(MODE_PATH, ahi16_modefile, sizeof ahi16_modefile)) return MODE_PATH;
        wrote = 1;
    }
    if (wrote) {
        /* AHI's database from the files again; an older driver in memory goes first */
        SystemTags((STRPTR)"Avail >NIL: FLUSH", TAG_DONE);
        SystemTags((STRPTR)"AddAudioModes >NIL: REFRESH", TAG_DONE);
    }
    return NULL;
}

void ahi16_refresh(void)
{
    if (ahi16_pref() && exists("DEVS:ahi.device") && exists("DEVS:AHI") && exists("DEVS:AudioModes")) {
        const char *bad = put_files();
        if (bad) Printf("agaboot: HiFi sound - %s could not be written\n", (ULONG)bad);
    }
}

int ahi16_set(int on, char *msg, int len)
{
    msg[0] = 0;
    if (on) {
        if (!exists("DEVS:ahi.device") || !exists("DEVS:AHI") || !exists("DEVS:AudioModes")) {
            snprintf(msg, len, "AHI is not installed on this Amiga\n(DEVS:ahi.device, DEVS:AHI, DEVS:AudioModes):\nHiFi sound needs it.");
            return 20;
        }
        if (!kernel_ahi()) {
            snprintf(msg, len, "The kernel running here cannot play AHI's\nsound on HDMI: install the current\nAGA-PISTORM, then switch it on again.");
            return 20;
        }
        const char *bad = put_files();
        if (bad) {
            snprintf(msg, len, "%s\ncould not be written.", bad);
            return 20;
        }
        keep_before();
        int a = edit_prefs(PREFS_ENV, on), b = edit_prefs(PREFS_ARC, on);   /* 1 HiFi, 2 its low latency mode */
        const char *val = on == 2 ? "LOWLATENCY" : "ON";
        SetVar((STRPTR)AHI16_VAR, (STRPTR)val, -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)AHI16_VAR, (STRPTR)val, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        if (a < 0 || b < 0) {
            snprintf(msg, len, "HiFi sound is on, but AHI's settings could\nnot be changed: in Prefs/AHI pick\n\"AGA-PISTORM HDMI:HiFi 24 bit stereo++%s\".",
                     on == 2 ? " low latency" : "");
            return 5;
        }
        if (on == 2)
            snprintf(msg, len, "HiFi sound is on, with low latency: AHI\n"
                               "programs play on HDMI some 60 ms sooner -\n"
                               "for games. A stall of the Amiga (a window\n"
                               "dragged, a disk read) can leave a short gap,\n"
                               "after which it plays safe for 10 seconds.\n"
                               "Programs already playing keep their sound\nuntil they start again.");
        else
        snprintf(msg, len, "HiFi sound is on: AHI programs play on HDMI\n"
                           "in up to 24 bits, at their own rate (AHI's\n"
                           "unit 0 and Music unit at \"AGA-PISTORM\n"
                           "HDMI:HiFi 24 bit stereo++\", 48 kHz - set\n"
                           "44100 Hz in Prefs/AHI for music from CDs).\n"
                           "Programs already playing keep their sound\nuntil they start again.");
        return 0;
    }
    int a = 0, b = 0;
    if (had_none()) {
        /* AHI had no settings before On made them: none again, its own defaults */
        DeleteFile((STRPTR)PREFS_ENV);
        DeleteFile((STRPTR)PREFS_ARC);
    } else {
        load_before();
        a = edit_prefs(PREFS_ENV, 0);
        b = edit_prefs(PREFS_ARC, 0);
    }
    DeleteFile((STRPTR)BACKUP);                          /* the next On keeps a fresh one */
    DeleteFile((STRPTR)MODE_PATH);
    DeleteVar((STRPTR)AHI16_VAR, GVF_GLOBAL_ONLY);
    DeleteVar((STRPTR)AHI16_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    if (a < 0 || b < 0) {
        snprintf(msg, len, "HiFi sound is off, but AHI's settings could\nnot be changed: check the modes in Prefs/AHI.");
        return 5;
    }
    snprintf(msg, len, "HiFi sound is off: AHI's units have their\nearlier modes back (gone after a reboot from\nthe mode list too).");
    return 0;
}

/* how one prefs file's unit 0 and Music unit stand */
struct look { ULONG unit0, music; };
static void look_unit(UBYTE *u, void *ctx)
{
    struct look *l = ctx;
    if (u[0] == 0) l->unit0 = get32(u + 4);
    if (u[0] == MUSIC_UNIT) l->music = get32(u + 4);
}

void ahi16_check(void)
{
    struct look l = { ~0UL, ~0UL };
    LONG n;
    UBYTE *b = read_file(PREFS_ENV, &n);
    if (b) { each_unit(b, n, look_unit, &l); FreeVec(b); }
    Printf("  HiFi sound (AHI)         %s, driver %s, modes %s\n",
           (ULONG)(ahi16_pref() == 2 ? "ON, low latency" : ahi16_pref() ? "ON" : "off"),
           (ULONG)(same_file(DRV_PATH, ahi16_driver, sizeof ahi16_driver) ? "current" : exists(DRV_PATH) ? "OLDER" : "not there"),
           (ULONG)(exists(MODE_PATH) ? "there" : "not there"));
    if (b) Printf("    AHI unit 0 / Music     %s / %s\n",
                  (ULONG)(l.unit0 == ~0UL ? "-" : l.unit0 == MODE_HIFI ? "HDMI HiFi" : l.unit0 == MODE_LOWLAT ? "HDMI HiFi low latency"
                          : OURS(l.unit0) ? "HDMI 16 bit" : "another mode"),
                  (ULONG)(l.music == ~0UL ? "-" : l.music == MODE_HIFI ? "HDMI HiFi" : l.music == MODE_LOWLAT ? "HDMI HiFi low latency"
                          : OURS(l.music) ? "HDMI 16 bit" : "another mode"));
}
