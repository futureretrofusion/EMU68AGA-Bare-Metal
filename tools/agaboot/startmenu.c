/* startmenu.c - an AGA-PISTORM sub-menu buried in Directory Opus 5's start
 * menus (1.118, from RGB2RTG 0.76-dev84's, EMM #2198): CaffeineOS's Caffeine
 * button opens one (DOpus5:Buttons/CaffeineBlue_Start, and its themes'
 * Caffeine_Start, Biggun_Start, Kids_Start). Portable C, so that the host
 * can test it on real banks (HOST_TEST) - I trust a binary format the day
 * it round-trips, and not a fucking second before.
 *
 * A start menu is a button bank, IFF FORM OPUS (or EPUS, Opus 5.102's), with
 * a STRT chunk: each entry a BUTN chunk followed by its FUNC chunks, laid out
 * as dopusmenu.c says. As CaffeineOS's banks have them (read from the user's
 * A1200 by RGB2RTG, 2026-10-20):
 *   - a top-level entry: BUTN flags 0x50 (title + new format); its FUNC
 *     flags 0x10 (run async), flags2 0x0c, then its icon (string, type 5)
 *     and its command (string, type 1, AmigaDOS);
 *   - a sub-menu: its heading a top-level BUTN whose FUNC has flags
 *     0x00010011 and only the icon, its entries the BUTNs after it with
 *     flags 0x10, up to the next top-level one ("Mount Device", "Network");
 *   - a separator: a BUTN "---" and a FUNC with no instructions.
 * AGA-PISTORM's sub-menu goes before Karma (or Reboot, or at the end), with
 * a separator after it; done again it replaces itself, and removed it takes
 * the separator along - the bank as it was, byte for byte. I touch my own
 * damn label and nothing else, so RGB2RTG's sub-menu on the same card stays.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SM_TITLE   0x50                 /* BUTN flags: a top-level entry */
#define SM_ITEM    0x10                 /* ... an entry of a sub-menu */
#define SM_LABEL   "AGA-PISTORM"

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }

static unsigned char *chunk(unsigned char *o, const char *id, const unsigned char *body, unsigned len)
{
    memcpy(o, id, 4); put32(o + 4, len); memcpy(o + 8, body, len);
    o += 8 + len;
    if (len & 1) *o++ = 0;
    return o;
}

static unsigned char *emit_butn(unsigned char *o, unsigned flags, const char *label)
{
    unsigned char b[14 + 64];
    memset(b, 0, sizeof b);
    b[0] = 1; put32(b + 2, flags); put16(b + 6, 1);
    unsigned n = 14;
    strcpy((char *)b + n, label); n += strlen(label) + 1;
    b[n++] = 1; b[n++] = 0;                        /* its name: the label's */
    return chunk(o, "BUTN", b, n);
}

/* a FUNC: flags, then the icon (type 5) and the command (type 1), either
   left out when NULL; code 0xffff as the banks have it */
static unsigned char *emit_func(unsigned char *o, unsigned flags, unsigned flags2, const char *icon, const char *cmd,
                                int sep)
{
    unsigned char f[22 + 192];
    memset(f, 0, sizeof f);
    put32(f, flags); put32(f + 4, flags2);
    put16(f + 12, 0xffff);
    if (sep) put16(f + 18, 0xffff);             /* a separator's qualifier mask, as theirs */
    unsigned n = 22;
    if (icon) { strcpy((char *)f + n, icon); n += strlen(icon) + 1; put16(f + n, 5); n += 2; }
    if (cmd)  { strcpy((char *)f + n, cmd);  n += strlen(cmd) + 1;  put16(f + n, 1); n += 2; }
    return chunk(o, "FUNC", f, n);
}

/* The icons: a chip, 32x32 with "AGA" on it for the heading, 20x22 for the
   entries - the sizes of CaffeineOS's own (a sub-menu's entries share one
   small icon there, as Mount Device's and Network's do); agaboot MENU writes
   them (chip_icon.h). The entries: the Chipset menu's own, as the SYS:Chipset
   drawer has them (agaboot.c chipset_menu - keep them in step); Monitor
   through its program, which gives it the stack it needs. */
#define SM_ICON    "DOpus5:Icons/AGA-PISTORM.info"
#define SM_ICON22  "DOpus5:Icons/AGA-PISTORM22.info"
static const char *const sm_items[][2] = {      /* label, command */
    { "Init",    "C:agaboot AGAGAMES ASK" },
    { "Output",  "C:agaboot OUTPUT ASK" },
    { "Sound",   "C:agaboot SOUND ASK" },
    { "Monitor", "SYS:Utilities/AGA-SoundMonitor" },
    { "Expert",  "C:agaboot EXPERT ASK" },
    { "About",   "C:agaboot ABOUT" },
};

static unsigned char *emit_block(unsigned char *o)
{
    o = emit_butn(o, SM_TITLE, SM_LABEL);
    o = emit_func(o, 0x00010011, 0x0c, SM_ICON, NULL, 0);
    for (unsigned i = 0; i < sizeof sm_items / sizeof sm_items[0]; i++) {
        o = emit_butn(o, SM_ITEM, sm_items[i][0]);
        o = emit_func(o, 0x10, 0x0c, SM_ICON22, sm_items[i][1], 0);
    }
    o = emit_butn(o, SM_TITLE, "---");
    return emit_func(o, 0, 0x04, NULL, NULL, 1);
}

/* a BUTN chunk's flags and label */
static unsigned butn_flags(const unsigned char *c) { return be32(c + 8 + 2); }
static const char *butn_label(const unsigned char *c) { return (const char *)c + 8 + 14; }
static int is(const unsigned char *c, const char *id) { return memcmp(c, id, 4) == 0; }

/* 1: a start menu (a FORM OPUS/EPUS with a STRT chunk); the chunks' offsets in at[], n */
static int scan(const unsigned char *d, long len, long *at, int max, int *n)
{
    *n = 0;
    if (len < 12 || memcmp(d, "FORM", 4) || (memcmp(d + 8, "OPUS", 4) && memcmp(d + 8, "EPUS", 4)))
        return 0;
    long end = 8 + (long)be32(d + 4);
    if (end > len) end = len;
    int strt = 0;
    for (long i = 12; i + 8 <= end; ) {
        long sz = (long)be32(d + i + 4);
        if (i + 8 + sz > end || *n >= max) return 0;
        if (is(d + i, "STRT")) strt = 1;
        at[(*n)++] = i;
        i += 8 + sz + (sz & 1);
    }
    return strt;
}

/* 0 changed, 1 nothing to do, -1 cannot read, -2 not a start menu, -3 cannot write */
int dopus_start_update(const char *path, int remove)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc(len > 0 ? len : 1);
    unsigned char *out = (unsigned char *)malloc(len + 2048);
    static long at[1024];
    int n = 0;
    if (!d || !out || fread(d, 1, len, f) != (size_t)len) {
        fclose(f); free(d); free(out);
        return -1;
    }
    fclose(f);
    if (!scan(d, len, at, 1024, &n)) {
        free(d); free(out);
        return -2;
    }
    /* mine: the AGA-PISTORM heading, its entries, and the separator after
       them */
    int ours = -1, ours_end = -1, karma = -1, reboot = -1;
    for (int k = 0; k < n; k++) {
        const unsigned char *c = d + at[k];
        if (!is(c, "BUTN") || be32(c + 4) < 15) continue;
        unsigned fl = butn_flags(c);
        const char *lab = butn_label(c);
        if ((fl & 0x40) && !strcmp(lab, SM_LABEL) && ours < 0) {
            ours = k;
            int j = k + 1;
            while (j < n && !(is(d + at[j], "BUTN") && (butn_flags(d + at[j]) & 0x40))) j++;   /* its entries */
            if (j < n && !strcmp(butn_label(d + at[j]), "---")) {                              /* my separator */
                j++;
                while (j < n && is(d + at[j], "FUNC")) j++;
            }
            ours_end = j;
        }
        if ((fl & 0x40) && !strcmp(lab, "Karma") && karma < 0) karma = k;
        if ((fl & 0x40) && !strcmp(lab, "Reboot") && reboot < 0) reboot = k;
    }
    if (remove && ours < 0) {
        free(d); free(out);
        return 1;
    }
    /* write: the chunks before, mine (unless removing), the rest - mine
       left out where they were */
    int place = ours >= 0 ? ours : karma >= 0 ? karma : reboot >= 0 ? reboot : n;
    unsigned char *o = out + 12;
    for (int k = 0; k <= n; k++) {
        if (k == place && !remove) o = emit_block(o);
        if (k == n) break;
        if (ours >= 0 && k >= ours && k < ours_end) continue;
        long sz = (long)be32(d + at[k] + 4);
        long full = 8 + sz + (sz & 1);
        memcpy(o, d + at[k], full);
        o += full;
    }
    long olen = o - out;
    memcpy(out, d, 12);
    put32(out + 4, (unsigned)(olen - 8));
    int same = olen == len && memcmp(out, d, len) == 0;
    int rc = 1;
    if (!same) {
        FILE *w = fopen(path, "wb");
        rc = -3;
        if (w) {
            if (fwrite(out, 1, olen, w) == (size_t)olen) rc = 0;
            if (fclose(w) != 0) rc = -3;
        }
    }
    free(d); free(out);
    return rc;
}

#ifdef HOST_TEST
/* startmenu DIR banks...: each bank copied to DIR and there given my
   sub-menu (changed, mine before Karma or Reboot with its six entries and
   a separator), given it again (nothing to do), and taken out again (the
   bank byte for byte); a file that is no start menu is left alone (-2). */
static long slurp(const char *p, unsigned char **d)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *d = (unsigned char *)malloc(n > 0 ? n : 1);
    if (!*d || fread(*d, 1, n, f) != (size_t)n) n = -1;
    fclose(f);
    return n;
}

static int put(const char *p, const unsigned char *d, long n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == (size_t)n;
    return fclose(f) == 0 && ok;
}

/* the top-level labels in order, into s */
static void tops(const unsigned char *d, long len, char *s, size_t cap)
{
    static long at[1024];
    int n;
    s[0] = 0;
    if (!scan(d, len, at, 1024, &n)) return;
    for (int k = 0; k < n; k++)
        if (is(d + at[k], "BUTN") && (butn_flags(d + at[k]) & 0x40) && strlen(s) + 40 < cap) {
            strcat(s, butn_label(d + at[k]));
            strcat(s, "|");
        }
}

int main(int argc, char **argv)
{
    int all = 1;
    if (argc < 3) { fprintf(stderr, "startmenu DIR banks...\n"); return 2; }
    for (int k = 2; k < argc; k++) {
        unsigned char *orig = NULL, *a = NULL, *b = NULL, *c = NULL;
        long no = slurp(argv[k], &orig);
        const char *base = strrchr(argv[k], '/');
        base = base ? base + 1 : argv[k];
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[1], base);
        if (no < 0 || !put(path, orig, no)) { printf("%s: cannot copy\n", base); all = 0; continue; }
        int r1 = dopus_start_update(path, 0);
        long na = slurp(path, &a);
        int r2 = dopus_start_update(path, 0);
        long nb = slurp(path, &b);
        int r3 = dopus_start_update(path, 1);
        long nc = slurp(path, &c);
        int r4 = dopus_start_update(path, 1);
        if (r1 == -2) {
            printf("%-22s not a start menu: left alone (%d)\n", base, r1);
            free(orig); free(a); free(b); free(c);
            continue;
        }
        static char t0[1024], t1[1024];
        tops(orig, no, t0, sizeof t0);
        tops(a, na, t1, sizeof t1);
        const char *before = strstr(t0, "Karma|") ? "Karma|" : strstr(t0, "Reboot|") ? "Reboot|" : "";
        char want[64];
        snprintf(want, sizeof want, "%s|---|%s", SM_LABEL, before);
        int placed = strstr(t1, want) != NULL;
        int items = 1;
        for (unsigned i = 0; i < sizeof sm_items / sizeof sm_items[0]; i++)
            if (!memmem(a, na, sm_items[i][1], strlen(sm_items[i][1]))) items = 0;
        int ok = r1 == 0 && placed && items && r2 == 1 && nb == na && !memcmp(a, b, na) && r3 == 0 &&
                 nc == no && !memcmp(c, orig, no) && r4 == 1;
        printf("%-22s added (%d) %s, again (%d), removed (%d) %s, again (%d)%s\n", base, r1,
               placed ? "before its own" : "IN THE WRONG PLACE", r2, r3,
               nc == no && !memcmp(c, orig, no) ? "byte for byte" : "DIFFERENT", r4, ok ? "" : " - WRONG");
        if (!ok) { all = 0; printf("  before: %s\n  after:  %s\n", t0, t1); }
        free(orig); free(a); free(b); free(c);
    }
    printf(all ? "ALL OK\n" : "FAILURES\n");
    return all ? 0 : 1;
}
#endif
