/* dopusmenu.c - add/refresh a "Chipset" title in a Directory Opus 5 Magellan
 * user-menu button bank (IFF FORM OPUS, chunks BTNW/BUTN/FUNC).
 *
 * Layout (from the Dopus 5 sources, Library/config_save.c):
 *   BUTN: CFG_BUTN {fpen,bpen,flags(4),count(2),pad(2),pad(4)} followed by
 *         count x (label\0 name\0), name "\1" meaning "same as label"
 *   FUNC: CFG_FUNC {flags(4),flags2(4),pad(4),code(2),qual(2),type(2),
 *         qual_mask(2),qual_same(2)} followed by (string\0 type(2))*
 * A menu title is a BUTN with BUTNF_TITLE, items are plain BUTNs until the
 * next title. An item with a sub-menu is ONE button with several functions:
 * the first one's label names the item, every further function is a
 * sub-item (Program/menus.c builds NM_ITEM, then NM_SUB for the rest; the
 * menu editor gives the sub-items FTYPE_LIST, config_menus.c). The loader
 * hands the BUTN's labels to the FUNC chunks after it, in order
 * (Library/config_open.c). Portable C, so it can be unit-tested on the host -
 * I am not debugging this IFF shit on the Amiga.
 *
 * Directory Opus 5.102 (the open-source Magellan II, BlitterStudio/dopus5)
 * writes its button banks as FORM EPUS, its "current schema": only the BTNW
 * window chunk grew, BUTN and FUNC are byte for byte the old layout
 * (Library/config_save.c, L_SaveButton/L_SaveFunction). BTNW is copied as it
 * is, so an EPUS bank takes the same edit and keeps its own FORM type. Read
 * for that fact only (AROS Public License); a tester's user menu was EPUS and
 * was refused as "not an Opus bank" (-2), 2026-10-02.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"

#define BUTNF_NEW_FORMAT 0x10
#define BUTNF_TITLE      0x40
#define FUNCF_RUN_ASYNC  0x10
#define FUNCF2_VALID_IX  0x04
#define INST_AMIGADOS    1
#define FTYPE_LEFT_BUTTON 0
#define FTYPE_LIST       100
#define MAX_LABELS       8

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

/* BUTN with n function labels (more than one: an item and its sub-items) */
static unsigned char *emit_button(unsigned char *o, unsigned flags, const char *const *label, int n)
{
    unsigned char b[14 + MAX_LABELS * 48];
    memset(b, 0, sizeof b);
    b[0] = 1; b[1] = 0; put32(b + 2, flags); put16(b + 6, (unsigned)n);
    unsigned k = 14;
    for (int i = 0; i < n; i++) {
        strcpy((char *)b + k, label[i]); k += strlen(label[i]) + 1;
        b[k++] = 1; b[k++] = 0;                    /* name = label */
    }
    return chunk(o, "BUTN", b, k);
}

/* FUNC: empty (a title, or an item that only heads sub-items) or one
   AmigaDOS instruction; type FTYPE_LIST for a sub-item */
static unsigned char *emit_func(unsigned char *o, const char *cmd, unsigned type)
{
    unsigned char f[22 + 128];
    memset(f, 0, sizeof f);
    put32(f, cmd ? FUNCF_RUN_ASYNC : 0); put32(f + 4, FUNCF2_VALID_IX);
    put16(f + 12, 0xFFFF); put16(f + 16, type); put16(f + 18, cmd ? 0 : 0xFFFF);
    unsigned n = 22;
    if (cmd) {
        strcpy((char *)f + n, cmd); n += strlen(cmd) + 1;
        put16(f + n, INST_AMIGADOS); n += 2;
    }
    return chunk(o, "FUNC", f, n);
}

static int butn_is_title(const unsigned char *body, unsigned len, const char *label)
{
    if (len < 14) return 0;
    if (!(be32(body + 2) & BUTNF_TITLE)) return 0;
    return label ? strncmp((const char *)body + 14, label, len - 14) == 0 : 1;
}

/* The items of m (menu.h) under title, in place of an earlier title of
   that name; m NULL: the title just goes. Returns 0 on success, -1 file
   error, -2 not an Opus bank. */
int dopus_menu_update(const char *path, const char *title, const struct menu_entry *m, int n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *in = malloc(size + 16), *out = malloc(size + 4096);
    if (!in || !out || fread(in, 1, size, f) != (size_t)size) { fclose(f); free(in); free(out); return -1; }
    fclose(f);
    if (size < 12 || memcmp(in, "FORM", 4) || (memcmp(in + 8, "OPUS", 4) && memcmp(in + 8, "EPUS", 4))) {
        free(in); free(out); return -2;
    }
    char type[4];                                  /* OPUS, or EPUS from DOpus 5.102 on */
    memcpy(type, in + 8, 4);

    unsigned char *o = out + 12;
    long p = 12; int skipping = 0;
    while (p + 8 <= size) {
        const char *id = (const char *)in + p;
        unsigned len = be32(in + p + 4);
        const unsigned char *body = in + p + 8;
        if (p + 8 + len > size) break;
        if (!memcmp(id, "BUTN", 4) && butn_is_title(body, len, NULL))
            skipping = butn_is_title(body, len, title);          /* drop my old block */
        if (!skipping) {
            memcpy(o, in + p, 8 + len); o += 8 + len;
            if (len & 1) *o++ = 0;
        }
        p += 8 + len + (len & 1);
    }
    if (m) {                                       /* NULL: just take my title out */
        o = emit_button(o, BUTNF_NEW_FORMAT | BUTNF_TITLE, &title, 1);
        o = emit_func(o, NULL, FTYPE_LEFT_BUTTON);
        for (int i = 0; i < n; i++) {
            if (m[i].sub) continue;                /* went out with its header */
            const char *labels[MAX_LABELS];
            int k = 0, head = menu_is_header(m, n, i);
            labels[k++] = m[i].label;
            for (int j = i + 1; head && j < n && m[j].sub && k < MAX_LABELS; j++) labels[k++] = m[j].label;
            o = emit_button(o, BUTNF_NEW_FORMAT, labels, k);
            o = emit_func(o, head ? NULL : m[i].cmd, FTYPE_LEFT_BUTTON);   /* a header runs nothing */
            for (int j = i + 1; head && j < i + k; j++) o = emit_func(o, m[j].cmd, FTYPE_LIST);
        }
    }

    memcpy(out, "FORM", 4); put32(out + 4, (unsigned)(o - out - 8)); memcpy(out + 8, type, 4);
    f = fopen(path, "wb");
    if (!f) { free(in); free(out); return -1; }
    fwrite(out, 1, o - out, f);
    fclose(f);
    free(in); free(out);
    return 0;
}

#ifdef HOST_TEST
/* dopusmenu FILE [remove|dump]: put agaboot's items in (the same table as
   agaboot.c's chipset_menu), take them out, or print the menus the way
   Opus builds them - a BUTN's labels go to the FUNC chunks after it, the
   first a title (BUTNF_TITLE) or an item, every further one a sub-item */
static const struct menu_entry test_menu[] = {
    { "Init", "C:agaboot AGAGAMES ASK", 4096, 0 },
    { "Output", "C:agaboot OUTPUT ASK", 4096, 0 },
    { "RTG Image...", "C:agaboot ZOOM ASK", 4096, 1 },
    { "RGB port...", "C:agaboot RGBPORT ASK", 4096, 1 },
    { "Sound", "C:agaboot SOUND ASK", 4096, 0 },
    { "Expert", "C:agaboot EXPERT ASK", 16384, 0 },
    { "System wide...", "C:agaboot SYSTEMWIDE ASK", 16384, 1 },
    { "Run program...", "C:agaboot EXPERT RUN", 16384, 1 },
    { "About", "C:agaboot ABOUT", 4096, 0 },
};

static int dump(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    static unsigned char in[1 << 20];
    long size = (long)fread(in, 1, sizeof in, f);
    fclose(f);
    char labels[MAX_LABELS * 4][64];
    int nl = 0, next = 0, title = 0;
    for (long p = 12; p + 8 <= size; ) {
        unsigned len = be32(in + p + 4);
        const unsigned char *b = in + p + 8;
        if (!memcmp(in + p, "BUTN", 4)) {
            title = (be32(b + 2) & BUTNF_TITLE) != 0;
            nl = (int)((b[6] << 8) | b[7]);
            next = 0;
            const char *s = (const char *)b + 14;
            for (int i = 0; i < nl && i < MAX_LABELS * 4; i++) {
                snprintf(labels[i], sizeof labels[i], "%s", s);
                s += strlen(s) + 1;
                s += strlen(s) + 1;                /* the name, \1 = the label */
            }
        } else if (!memcmp(in + p, "FUNC", 4) && next < nl) {
            unsigned type = (b[16] << 8) | b[17];
            const char *cmd = len > 22 ? (const char *)b + 22 : "";
            if (title) printf("TITLE %s\n", labels[next]);
            else printf("%s%s%s%s [type %u]\n", next ? "    SUB " : "  ITEM ", labels[next], *cmd ? " -> " : "", cmd, type);
            next++;
        }
        p += 8 + len + (len & 1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[2], "dump")) return dump(argv[1]);
    int remove = argc > 2 && !strcmp(argv[2], "remove");
    int n = (int)(sizeof test_menu / sizeof test_menu[0]);
    int r = dopus_menu_update(argv[1], "Chipset", remove ? NULL : test_menu, n);
    printf("result %d\n", r);
    return r ? 1 : 0;
}
#endif
