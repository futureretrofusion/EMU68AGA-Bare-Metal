/* AGA-PISTORM - virtual floppy drives (ADF images) on the emulated Paula.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Model: up to AGA_MAX_DRIVES drives holding ADF images in memory. The
 * current track is kept MFM-encoded (standard AmigaDOS layout: 11/22 sectors
 * of 544 MFM words, then a gap of 0xAAAA words; 6334 words per DD revolution).
 * The drive rotates whenever its motor is on: DSK_WORDS_PER_LINE words pass
 * the head per scan line (2 us per MFM bit = 32 us per word, a revolution
 * every 200 ms). Paula's disk DMA (DSKPT/DSKLEN/DSKSYNC, WORDSYNC in ADKCON)
 * reads those words into chip RAM or writes chip RAM words onto the track;
 * a written track is decoded back into the image when the transfer ends.
 * The drive control lines are the real CIAs' job on the machine; the glue
 * mirrors the CIA-B PRB writes here (aga_disk_cia_prb) and substitutes the
 * CIA-A PRA status bits (aga_disk_cia_pra) while a virtual drive is selected.
 *
 * The DD track length and gap (6334 words, 175 longs of gap) and the
 * 83-cylinder limit are WinUAE disk.cpp's values, and the DSKBYTR bits are
 * put together as there; the rest is mine, bugs and all.
 *   WinUAE disk.cpp: Copyright 1995 Hannu Rummukainen,
 *     Copyright 1995-2001 Bernd Schmidt, Copyright 2000-2021 Toni Wilen
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#define DSK_DD_SIZE      901120u
#define DSK_HD_SIZE      1802240u
#define DSK_SECTOR_WORDS 544
#define DSK_TRACK_WORDS_DD 6334
#define DSK_TRACK_WORDS_HD 12668
#define DSK_GAP_LONGS_DD   175
#define DSK_GAP_LONGS_HD   350
#define DSK_MAX_CYL      83

static disk_drive_t *cur_drive(aga_t *a)
{
    disk_t *d = a->dsk;
    if (!d || d->selected < 0) return NULL;
    return &d->drv[d->selected];
}

/* ---- MFM ------------------------------------------------------------------ */
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* odd/even split: data bits land on the even bit positions of the output longs */
static void split_block(uint32_t *out, const uint8_t *in, int nlongs)
{
    for (int i = 0; i < nlongs; i++) {
        uint32_t v = be32(in + 4 * i);
        out[i] = (v >> 1) & 0x55555555;          /* odd bits */
        out[nlongs + i] = v & 0x55555555;        /* even bits */
    }
}

static uint32_t join_block(const uint32_t *odd, const uint32_t *even)
{
    return ((*odd & 0x55555555) << 1) | (*even & 0x55555555);
}

/* insert clock bits into a run of longs whose data bits sit on even positions */
static void fill_clocks(uint32_t *l, int n, int prev_bit0)
{
    for (int i = 0; i < n; i++) {
        uint32_t d = l[i] & 0x55555555;
        uint32_t clock = ~((d << 1) | (d >> 1)) & 0xAAAAAAAA;
        if (prev_bit0) clock &= ~0x80000000u;
        l[i] = d | clock;
        prev_bit0 = d & 1;
    }
}

static void encode_track(disk_drive_t *dr)
{
    int nsec = dr->sectors;
    int track = dr->cyl * 2 + dr->side;
    uint32_t *L = (uint32_t *)dr->mfm;              /* work in longs, then swap to words */
    int nl = 0;
    uint8_t zero_label[16] = {0};
    for (int s = 0; s < nsec; s++) {
        uint32_t *sec = L + nl;
        sec[0] = 0;                                  /* pre-sync gap: data 0 -> 0xAAAAAAAA */
        sec[1] = 0x44014401;                         /* sync, patched after the clock pass */
        uint8_t info[4] = { 0xFF, (uint8_t)track, (uint8_t)s, (uint8_t)(nsec - s) };
        split_block(sec + 2, info, 1);               /* 2 longs */
        split_block(sec + 4, zero_label, 4);         /* 8 longs */
        uint32_t hchk = 0;
        for (int i = 2; i < 12; i++) hchk ^= sec[i];
        hchk &= 0x55555555;
        uint8_t hb[4]; put_be32(hb, hchk);
        split_block(sec + 12, hb, 1);                /* 2 longs */
        const uint8_t *data = dr->image + ((uint32_t)track * nsec + s) * 512;
        split_block(sec + 16, data, 128);            /* 256 longs */
        uint32_t dchk = 0;
        for (int i = 16; i < 272; i++) dchk ^= sec[i];
        dchk &= 0x55555555;
        uint8_t db[4]; put_be32(db, dchk);
        split_block(sec + 14, db, 1);                /* 2 longs */
        nl += 272;
    }
    int gap = (nsec == 11) ? DSK_GAP_LONGS_DD : DSK_GAP_LONGS_HD;
    for (int i = 0; i < gap; i++) L[nl + i] = 0;
    nl += gap;
    fill_clocks(L, nl, 0);
    for (int s = 0; s < nsec; s++) L[s * 272 + 1] = 0x44894489;
    /* longs -> big-endian word stream */
    for (int i = 0; i < nl; i++) {
        uint32_t v = L[i];
        dr->mfm[2 * i] = (uint16_t)(v >> 16);
        dr->mfm[2 * i + 1] = (uint16_t)v;
    }
    dr->mfm_words = nl * 2;
    dr->mfm_track = track;
    dr->mfm_valid = 1;
}

/* decode the (possibly rewritten) track buffer back into the image */
static void decode_track(disk_drive_t *dr)
{
    int n = dr->mfm_words, nsec = dr->sectors, found = 0;
    for (int p = 0; p < n; p++) {
        /* the header follows the last sync word of a run (a sync run is normally
           two words, but a track written after a WORDSYNC read may hold one) */
        if (dr->mfm[p] != 0x4489 || dr->mfm[(p + 1) % n] == 0x4489) continue;
        uint32_t w[272];
        for (int i = 0; i < 270; i++)
            w[i] = ((uint32_t)dr->mfm[(p + 1 + 2 * i) % n] << 16) | dr->mfm[(p + 2 + 2 * i) % n];
        uint32_t info = join_block(&w[0], &w[1]);
        int track = (info >> 16) & 0xFF, sector = (info >> 8) & 0xFF;
        if (((info >> 24) & 0xFF) != 0xFF || sector >= nsec || track != dr->mfm_track) continue;
        uint32_t hchk = 0;
        for (int i = 0; i < 10; i++) hchk ^= w[i];
        if ((hchk & 0x55555555) != join_block(&w[10], &w[11])) continue;
        uint32_t dchk = 0;
        for (int i = 14; i < 270; i++) dchk ^= w[i];
        if ((dchk & 0x55555555) != join_block(&w[12], &w[13])) continue;
        uint8_t *dst = dr->image + ((uint32_t)track * nsec + sector) * 512;
        for (int i = 0; i < 128; i++) put_be32(dst + 4 * i, join_block(&w[14 + i], &w[14 + 128 + i]));
        found++;
        p += 540;
    }
    if (found) dr->dirty = 1;
}

/* ---- drive API (glue) ------------------------------------------------------ */
int aga_disk_insert(aga_t *a, int drive, const uint8_t *adf, uint32_t size, int wprot)
{
    disk_t *d = a->dsk;
    if (!d || drive < 0 || drive >= AGA_MAX_DRIVES) return -1;
    if (size != DSK_DD_SIZE && size != DSK_HD_SIZE) return -2;
    disk_drive_t *dr = &d->drv[drive];
    aga_disk_eject(a, drive);
    dr->image = calloc(1, size);
    dr->mfm = calloc(DSK_TRACK_WORDS_HD, sizeof(uint16_t));
    if (!dr->image || !dr->mfm) { aga_disk_eject(a, drive); return -3; }
    memcpy(dr->image, adf, size);
    dr->size = size;
    dr->sectors = (size == DSK_HD_SIZE) ? 22 : 11;
    dr->wprot = wprot ? 1 : 0;
    dr->inserted = 1;
    dr->changed = 1;                 /* CHNG stays asserted until a step pulse sees the disk */
    dr->mfm_valid = 0;
    dr->dirty = 0;
    aga_logf(a, "disk: DF%d: %u KB image inserted%s", drive, size >> 10, wprot ? " (write protected)" : "");
    return 0;
}

void aga_disk_eject(aga_t *a, int drive)
{
    disk_t *d = a->dsk;
    if (!d || drive < 0 || drive >= AGA_MAX_DRIVES) return;
    disk_drive_t *dr = &d->drv[drive];
    if (dr->image) free(dr->image);
    if (dr->mfm) free(dr->mfm);
    dr->image = NULL; dr->mfm = NULL;
    dr->inserted = 0; dr->mfm_valid = 0; dr->size = 0;
    dr->changed = 1;
}

int aga_disk_inserted(const aga_t *a, int drive)
{
    return a->dsk && drive >= 0 && drive < AGA_MAX_DRIVES && a->dsk->drv[drive].inserted;
}

int aga_disk_active(const aga_t *a)
{
    if (!a->dsk) return 0;
    for (int i = 0; i < AGA_MAX_DRIVES; i++)
        if (a->dsk->drv[i].inserted) return 1;
    return 0;
}

const uint8_t *aga_disk_image(const aga_t *a, int drive, uint32_t *size, int *dirty)
{
    if (!aga_disk_inserted(a, drive)) return NULL;
    const disk_drive_t *dr = &a->dsk->drv[drive];
    if (size) *size = dr->size;
    if (dirty) *dirty = dr->dirty;
    return dr->image;
}

uint8_t aga_disk_cia_prb_mask(const aga_t *a)
{
    uint8_t m = 0;
    if (!a->dsk) return 0;
    for (int i = 0; i < AGA_MAX_DRIVES; i++)
        if (a->dsk->drv[i].inserted) m |= (uint8_t)(0x08 << i);   /* SELx bits to force high on the real CIA */
    return m;
}

/* CIA-B PRB: 7 MTR, 6 SEL3, 5 SEL2, 4 SEL1, 3 SEL0, 2 SIDE, 1 DIR, 0 STEP (all active low) */
void aga_disk_cia_prb(aga_t *a, uint8_t prb)
{
    disk_t *d = a->dsk;
    if (!d) return;
    uint8_t old = d->prb;
    d->prb = prb;
    d->selected = -1;
    for (int i = 0; i < AGA_MAX_DRIVES; i++) {
        disk_drive_t *dr = &d->drv[i];
        int sel_bit = 0x08 << i;
        int sel = !(prb & sel_bit), was_sel = !(old & sel_bit);
        if (sel && !dr->inserted) continue;
        if (sel && d->selected < 0) d->selected = i;
        if (sel && !was_sel)
            dr->motor = !(prb & 0x80);                       /* MTR is latched on the SEL falling edge */
        dr->side = (prb & 0x04) ? 0 : 1;
        if (sel && (old & 1) && !(prb & 1)) {                /* STEP falling edge */
            if (prb & 0x02) { if (dr->cyl > 0) dr->cyl--; }
            else            { if (dr->cyl < DSK_MAX_CYL - 1) dr->cyl++; }
            if (dr->inserted) dr->changed = 0;
        }
    }
}

/* CIA-A PRA status bits for the selected virtual drive: 5 RDY, 4 TK0, 3 WPROT, 2 CHNG (active low).
   Returns 0 if no virtual drive is selected (the real CIA value applies). */
int aga_disk_cia_pra(aga_t *a, uint8_t *bits)
{
    disk_drive_t *dr = cur_drive(a);
    if (!dr) return 0;
    uint8_t v = 0x3C;                                        /* all lines inactive (high) */
    if (dr->motor) {
        if (dr->inserted) v &= ~0x20;                        /* RDY */
    } else {
        /* motor off: RDY carries the drive id, one bit per SEL pulse; a present
           3.5" DD drive answers $FFFFFFFF, i.e. RDY active on every bit */
        v &= ~0x20;
    }
    if (dr->cyl == 0) v &= ~0x10;                            /* TK0 */
    if (dr->wprot || !dr->inserted) v &= ~0x08;              /* WPROT */
    if (!dr->inserted || dr->changed) v &= ~0x04;            /* CHNG */
    *bits = v;
    return 1;
}

/* ---- Paula side ----------------------------------------------------------- */
void dsk_reset(aga_t *a)
{
    disk_t *d = a->dsk;
    if (!d) return;
    d->dsklen = 0; d->dsklen_armed = 0; d->dskpt = 0; d->dsksync = 0x4489;
    d->dma_active = 0; d->dma_write = 0; d->dma_left = 0; d->synced = 0;
    d->dskbytr = 0; d->dskdat = 0; d->wordequal = 0;
    d->selected = -1; d->prb = 0xFF;
    for (int i = 0; i < AGA_MAX_DRIVES; i++) {
        d->drv[i].motor = 0; d->drv[i].mfm_valid = 0; d->drv[i].pos = 0;
        d->drv[i].cyl = 0; d->drv[i].side = 0;
    }
}

int dsk_read(aga_t *a, uint32_t reg, uint16_t *v)
{
    disk_t *d = a->dsk;
    if (!d || !aga_disk_active(a)) return 0;
    switch (reg) {
    case DSKDATR: *v = d->dskdat; return 1;
    case DSKBYTR: {
        uint16_t r = d->dskbytr & 0x80FF;
        d->dskbytr &= 0x7FFF;                                /* DSKBYT is cleared by the read */
        if ((d->dsklen & 0x8000) && aga_dma_enabled(a, DMAF_DSKEN)) r |= 0x4000;
        if (d->dsklen & 0x4000) r |= 0x2000;
        if (d->wordequal) r |= 0x1000;
        *v = r;
        return 1;
    }
    default: return 0;
    }
}

int dsk_write(aga_t *a, uint32_t reg, uint16_t v)
{
    disk_t *d = a->dsk;
    if (!d || !aga_disk_active(a)) return 0;
    switch (reg) {
    case DSKPTH: d->dskpt = ((uint32_t)v << 16) | (d->dskpt & 0xFFFF); return 1;
    case DSKPTL: d->dskpt = (d->dskpt & 0xFFFF0000u) | (v & 0xFFFE); return 1;
    case DSKSYNC: d->dsksync = v; return 1;
    case DSKDAT: return 1;
    case DSKLEN:
        if (!(v & 0x8000)) {
            d->dma_active = 0;
            d->dsklen_armed = 0;
        } else if (d->dsklen_armed) {
            /* second write with DMAEN set: the transfer starts */
            d->dma_active = 1;
            d->dma_write = (v & 0x4000) ? 1 : 0;
            d->dma_left = v & 0x3FFF;
            d->synced = (a->adkcon & 0x0400) ? 0 : 1;        /* WORDSYNC: wait for DSKSYNC first */
            d->dsklen_armed = 0;
            if (d->dma_left == 0) {                           /* nothing to do */
                d->dma_active = 0;
                aga_intreq_set(a, INTF_DSKBLK);
            }
        } else {
            d->dsklen_armed = 1;
        }
        d->dsklen = v;
        return 1;
    default: return 0;
    }
}

static void dma_finished(aga_t *a, disk_drive_t *dr)
{
    disk_t *d = a->dsk;
    d->dma_active = 0;
    aga_intreq_set(a, INTF_DSKBLK);
    if (d->dma_write && dr && dr->mfm_valid) decode_track(dr);
}

/* one word passes the head */
static void head_word(aga_t *a, disk_drive_t *dr)
{
    disk_t *d = a->dsk;
    int dma_ok = d->dma_active && aga_dma_enabled(a, DMAF_DSKEN);
    if (dma_ok && d->dma_write) {
        if (!dr->wprot) {
            dr->mfm[dr->pos] = chip_rd16(a, d->dskpt);
            d->dskpt += 2;
        }
        if (--d->dma_left <= 0) dma_finished(a, dr);
    } else {
        uint16_t w = dr->mfm[dr->pos];
        d->dskdat = w;
        d->dskbytr = 0x8000 | (w & 0xFF);
        d->wordequal = 0;
        if (w == d->dsksync) {
            d->wordequal = 1;
            aga_intreq_set(a, INTF_DSKSYN);
            if (dma_ok && !d->synced) { d->synced = 1; goto next; }   /* the sync word itself is not stored */
        }
        if (dma_ok && d->synced) {
            chip_wr16(a, d->dskpt, w);
            d->dskpt += 2;
            if (--d->dma_left <= 0) dma_finished(a, dr);
        }
    }
next:
    if (++dr->pos >= dr->mfm_words) dr->pos = 0;
}

void dsk_line_tick(aga_t *a)
{
    disk_t *d = a->dsk;
    if (!d) return;
    /* NOT cur_drive(): that returns NULL the moment the drive is deselected,
       and d->selected is reset to -1 on every CIA-B port B write. A real drive
       keeps spinning once its motor is on, whether or not /SEL is asserted -
       damn Kickstart routinely deselects while it sets Paula's DMA up.
       Stopping rotation there froze the disk mid-sector, which is why booting
       an ADF left the machine polling a drive that never came ready. */
    disk_drive_t *dr = NULL;
    for (int i = 0; i < AGA_MAX_DRIVES; i++) {
        disk_drive_t *c = &a->dsk->drv[i];
        if (c->inserted && c->motor) { dr = c; break; }
    }
    if (!dr) return;
    int track = dr->cyl * 2 + dr->side;
    if (!dr->mfm_valid || dr->mfm_track != track) {
        if (dr->mfm_valid && dr->dirty) { /* image already updated at DMA end */ }
        encode_track(dr);
        if (dr->pos >= dr->mfm_words) dr->pos = 0;
    }
    int words = (dr->sectors == 11) ? DSK_WORDS_PER_LINE : 2 * DSK_WORDS_PER_LINE;
    for (int i = 0; i < words; i++) head_word(a, dr);
}

void dsk_create(aga_t *a)
{
    a->dsk = calloc(1, sizeof(disk_t));
    dsk_reset(a);
}

void dsk_destroy(aga_t *a)
{
    if (!a->dsk) return;
    for (int i = 0; i < AGA_MAX_DRIVES; i++) aga_disk_eject(a, i);
    free(a->dsk);
    a->dsk = NULL;
}
