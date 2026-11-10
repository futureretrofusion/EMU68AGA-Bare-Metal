/* AGA-STORM — virtualni disketni pogoni (ADF slike) na emuliranoj Pauli.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Model: do AGA_MAX_DRIVES pogona koji drže ADF slike u memoriji. Trenutačna
 * staza drži se MFM-kodirana (standardni AmigaDOS raspored: 11/22 sektora od
 * 544 MFM riječi, zatim gap od 0xAAAA riječi; 6334 riječi po DD okretaju).
 * Pogon se okreće kad god mu je motor uključen: DSK_WORDS_PER_LINE riječi
 * prođe glavom po scanlineu (2 us po MFM bitu = 32 us po riječi, okretaj
 * svakih 200 ms). Paulin disk DMA (DSKPT/DSKLEN/DSKSYNC, WORDSYNC u ADKCON)
 * čita te riječi u chip RAM ili piše riječi chip RAM-a na stazu; zapisana
 * staza dekodira se natrag u sliku kad prijenos završi. Upravljačke linije
 * pogona posao su pravih CIA na stroju; glue ovdje preslikava CIA-B PRB
 * upise (aga_disk_cia_prb) i zamjenjuje statusne bitove CIA-A PRA
 * (aga_disk_cia_pra) dok je virtualni pogon odabran.
 *
 * The DD track length and gap (6334 words, 175 longs of gap) and the
 * 83-cylinder limit are WinUAE disk.cpp's values, and the DSKBYTR bits are
 * put together as there; the rest is mine, bugs and all.
 *   WinUAE disk.cpp: Copyright 1995 Hannu Rummukainen,
 *     Copyright 1995-2001 Bernd Schmidt, Copyright 2000-2021 Toni Wilen
 * Promijenjeno u odnosu na UAE/WinUAE kod za AGA-STORM 2026.
 * UAE i WinUAE su GPL, kao i ova datoteka.
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

/* neparni/parni split: bitovi podataka padaju na parne bit-pozicije izlaznih longova */
static void split_block(uint32_t *out, const uint8_t *in, int nlongs)
{
    for (int i = 0; i < nlongs; i++) {
        uint32_t v = be32(in + 4 * i);
        out[i] = (v >> 1) & 0x55555555;          /* neparni bitovi */
        out[nlongs + i] = v & 0x55555555;        /* parni bitovi */
    }
}

static uint32_t join_block(const uint32_t *odd, const uint32_t *even)
{
    return ((*odd & 0x55555555) << 1) | (*even & 0x55555555);
}

/* umetni clock bitove u niz longova kojima bitovi podataka sjede na parnim pozicijama */
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
    uint32_t *L = (uint32_t *)dr->mfm;              /* radi u longovima, zatim zamijeni u riječi */
    int nl = 0;
    uint8_t zero_label[16] = {0};
    for (int s = 0; s < nsec; s++) {
        uint32_t *sec = L + nl;
        sec[0] = 0;                                  /* gap prije synca: podaci 0 -> 0xAAAAAAAA */
        sec[1] = 0x44014401;                         /* sync, zakrpan nakon prolaza clockova */
        uint8_t info[4] = { 0xFF, (uint8_t)track, (uint8_t)s, (uint8_t)(nsec - s) };
        split_block(sec + 2, info, 1);               /* 2 longa */
        split_block(sec + 4, zero_label, 4);         /* 8 longova */
        uint32_t hchk = 0;
        for (int i = 2; i < 12; i++) hchk ^= sec[i];
        hchk &= 0x55555555;
        uint8_t hb[4]; put_be32(hb, hchk);
        split_block(sec + 12, hb, 1);                /* 2 longa */
        const uint8_t *data = dr->image + ((uint32_t)track * nsec + s) * 512;
        split_block(sec + 16, data, 128);            /* 256 longova */
        uint32_t dchk = 0;
        for (int i = 16; i < 272; i++) dchk ^= sec[i];
        dchk &= 0x55555555;
        uint8_t db[4]; put_be32(db, dchk);
        split_block(sec + 14, db, 1);                /* 2 longa */
        nl += 272;
    }
    int gap = (nsec == 11) ? DSK_GAP_LONGS_DD : DSK_GAP_LONGS_HD;
    for (int i = 0; i < gap; i++) L[nl + i] = 0;
    nl += gap;
    fill_clocks(L, nl, 0);
    for (int s = 0; s < nsec; s++) L[s * 272 + 1] = 0x44894489;
    /* longovi -> big-endian tok riječi */
    for (int i = 0; i < nl; i++) {
        uint32_t v = L[i];
        dr->mfm[2 * i] = (uint16_t)(v >> 16);
        dr->mfm[2 * i + 1] = (uint16_t)v;
    }
    dr->mfm_words = nl * 2;
    dr->mfm_track = track;
    dr->mfm_valid = 1;
}

/* dekodiraj (možda prepisani) međuspremnik staze natrag u sliku */
static void decode_track(disk_drive_t *dr)
{
    int n = dr->mfm_words, nsec = dr->sectors, found = 0;
    for (int p = 0; p < n; p++) {
        /* zaglavlje slijedi nakon posljednje sync riječi niza (sync niz obično
           ima dvije riječi, ali staza zapisana nakon WORDSYNC čitanja može
           imati jednu - naravno da može) */
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

/* ---- API pogona (glue) ----------------------------------------------------- */
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
    dr->changed = 1;                 /* CHNG ostaje aktivan dok step impuls ne vidi disk */
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
    if (dr->pending_image) free(dr->pending_image);
    if (dr->pending_mfm) free(dr->pending_mfm);
    dr->pending_image = NULL; dr->pending_mfm = NULL;
    dr->pending_size = 0; dr->pending_wprot = 0; dr->swap_lines = 0;
    dr->image = NULL; dr->mfm = NULL;
    dr->inserted = 0; dr->mfm_valid = 0; dr->size = 0;
    dr->changed = 1;
}

/* RC11: a direct ADF replacement skipped the empty-drive interval.
 * AmigaOS retained the old trackdisk volume/CHANGE counter. Stage a
 * ~0.55-second absent interval so it sees /CHNG, then new media and
 * INDEX pulses; the actual ADF and MFM buffers are allocated before
 * changing disk state, not on the realtime chipset scanline. */
int aga_disk_swap(aga_t *a, int drive, const uint8_t *adf, uint32_t size, int wprot)
{
    disk_t *d=a ? a->dsk : NULL;
    if (!d || !adf || drive<0 || drive>=AGA_MAX_DRIVES) return -1;
    if (size!=DSK_DD_SIZE && size!=DSK_HD_SIZE) return -2;
    disk_drive_t *dr=&d->drv[drive];
    if (dr->inserted && dr->dirty) return -4;
    /* If empty and no delayed replacement is running, mount immediately
     * (cold boot / first insert). Replacements need an observable gap. */
    if (!dr->inserted && !dr->pending_image)
        return aga_disk_insert(a,drive,adf,size,wprot);
    uint8_t *copy=calloc(1,size);
    uint16_t *track=calloc(DSK_TRACK_WORDS_HD,sizeof(uint16_t));
    if (!copy || !track) { free(copy); free(track); return -3; }
    /* Avoid host libc __memcpy_chk generated by -D_FORTIFY_SOURCE=3:
     * bare metal deliberately has no glibc fortify runtime. The
     * volatile destination keeps this slow-path copy explicit. */
    volatile uint8_t *out=(volatile uint8_t *)copy;
    for(uint32_t j=0;j<size;j++)out[j]=adf[j];
    aga_disk_eject(a,drive);
    dr->pending_image=copy;
    dr->pending_mfm=track;
    dr->pending_size=size;
    dr->pending_wprot=!!wprot;
    /* Both PAL and NTSC deliver approximately 15.6k lines/second. */
    dr->swap_lines=8600u; /* ~0.55 sec */
    d->dma_active=0; d->dsklen_armed=0;
    dr->changed=1;
    if(a->hooks.disk_index)a->hooks.disk_index(a->hooks.user,drive);
    aga_logf(a,"disk: DF%d: removed, staging %u KB replacement",drive,size>>10);
    return 0;
}

int aga_disk_inserted(const aga_t *a, int drive)
{
    return a->dsk && drive >= 0 && drive < AGA_MAX_DRIVES && a->dsk->drv[drive].inserted;
}

int aga_disk_active(const aga_t *a)
{
    if (!a->dsk) return 0;
#ifdef FRF_VIRTUAL_AGA
    /* Standalone virtual machine: DF0 is a physical drive even if empty.
     * CIA RDY/CHNG/TK0 must stay functional; no fake blank ADF. */
    return 1;
#endif
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
        if (a->dsk->drv[i].inserted) m |= (uint8_t)(0x08 << i);   /* SELx bitovi koje treba forsirati visoko na pravoj CIA */
    return m;
}

/* CIA-B PRB: 7 MTR, 6 SEL3, 5 SEL2, 4 SEL1, 3 SEL0, 2 SIDE, 1 DIR, 0 STEP (svi aktivni nisko) */
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
#ifdef FRF_VIRTUAL_AGA
        /* RC12: both drives are physically present in our virtual Amiga
         * even when empty. Kickstart probes external DF1 while the motor
         * is stopped, before a user loads any ADF; treating an empty DF1
         * as nonexistent prevents trackdisk from registering the drive.
         * Presence and inserted media are separate properties. */
#else
        if (sel && !dr->inserted) continue;
#endif
        if (sel && d->selected < 0) d->selected = i;
        if (sel && !was_sel)
            dr->motor = !(prb & 0x80);                       /* MTR se pamti na silaznom rubu SEL-a */
        dr->side = (prb & 0x04) ? 0 : 1;
        if (sel && (old & 1) && !(prb & 1)) {                /* silazni rub STEP-a */
            if (prb & 0x02) { if (dr->cyl > 0) dr->cyl--; }
            else            { if (dr->cyl < DSK_MAX_CYL - 1) dr->cyl++; }
            if (dr->inserted) dr->changed = 0;
        }
    }
}

/* statusni bitovi CIA-A PRA za odabrani virtualni pogon: 5 RDY, 4 TK0, 3
   WPROT, 2 CHNG (aktivni nisko). Vraća 0 ako nijedan virtualni pogon nije
   odabran (vrijedi vrijednost prave CIA) - i tada se ne pretvaram da sam
   disketa. */
int aga_disk_cia_pra(aga_t *a, uint8_t *bits)
{
    disk_drive_t *dr = cur_drive(a);
    if (!dr) return 0;
    uint8_t v = 0x3C;                                        /* sve linije neaktivne (visoko) */
    if (dr->motor) {
        if (dr->inserted) v &= ~0x20;                        /* RDY */
    } else {
        /* motor isključen: RDY nosi id pogona, jedan bit po SEL impulsu; prisutan
           3.5" DD pogon odgovara $FFFFFFFF, tj. RDY je aktivan na svakom bitu */
        v &= ~0x20;
    }
    if (dr->cyl == 0) v &= ~0x10;                            /* TK0 */
    if (dr->wprot || !dr->inserted) v &= ~0x08;              /* WPROT */
    if (!dr->inserted || dr->changed) v &= ~0x04;            /* CHNG */
    *bits = v;
    return 1;
}

/* ---- Paula strana --------------------------------------------------------- */
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
        d->dskbytr &= 0x7FFF;                                /* DSKBYT se čisti čitanjem */
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
            /* drugi upis s postavljenim DMAEN: prijenos počinje */
            d->dma_active = 1;
            d->dma_write = (v & 0x4000) ? 1 : 0;
            d->dma_left = v & 0x3FFF;
            d->synced = (a->adkcon & 0x0400) ? 0 : 1;        /* WORDSYNC: prvo čekaj DSKSYNC */
            d->dsklen_armed = 0;
            if (d->dma_left == 0) {                           /* nema što raditi */
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

/* jedna riječ prolazi glavom - polako, kao i sve na disketi */
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
            if (dma_ok && !d->synced) { d->synced = 1; goto next; }   /* sama sync riječ se ne pohranjuje */
        }
        if (dma_ok && d->synced) {
            chip_wr16(a, d->dskpt, w);
            d->dskpt += 2;
            if (--d->dma_left <= 0) dma_finished(a, dr);
        }
    }
next:
    if (++dr->pos >= dr->mfm_words) {
        dr->pos=0;
        /* Every revolution is an index edge on CIA-B FLAG.  The callback
         * only queues an atomic event; never take CIA lock inside AGA lock. */
        if(a->hooks.disk_index)
            a->hooks.disk_index(a->hooks.user,(int)(dr-d->drv));
    }
}

void dsk_line_tick(aga_t *a)
{
    disk_t *d = a->dsk;
    if (!d) return;
    /* Complete queued mechanical changes even when no drive is spinning. */
    for (int i=0;i<AGA_MAX_DRIVES;i++) {
        disk_drive_t *pd=&d->drv[i];
        if (!pd->pending_image || !pd->swap_lines) continue;
        if (--pd->swap_lines==0) {
            pd->image=pd->pending_image;
            pd->mfm=pd->pending_mfm;
            pd->size=pd->pending_size;
            pd->wprot=pd->pending_wprot;
            pd->sectors=pd->size==DSK_HD_SIZE ? 22 : 11;
            pd->pending_image=NULL; pd->pending_mfm=NULL;
            pd->pending_size=0; pd->pending_wprot=0;
            pd->inserted=1; pd->changed=1;
            pd->dirty=0; pd->mfm_valid=0; pd->pos=0;
            pd->cyl=0; pd->side=0;
            /* A selected drive may not receive another PRB edge after an
             * OSD swap. Restore its motor state from the held line. */
            pd->motor=!(d->prb & (0x08u<<i)) && !(d->prb & 0x80u);
            if(a->hooks.disk_index)a->hooks.disk_index(a->hooks.user,i);
            aga_logf(a,"disk: DF%d: replacement mounted after change interval",i);
        }
    }
    /* Each motor rotates independently. Previously only the *first*
     * spinning drive moved, even if DF1 was selected; that routed DF1 DMA
     * to DF0 and starved DF1 of INDEX events. Preserve the prior bootable
     * DF0 fallback when no drive is selected (Kickstart deselects while
     * arming disk DMA). Paula uses the selected spinning drive otherwise. */
    int reader=-1,first=-1;
    for (int i=0;i<AGA_MAX_DRIVES;i++) {
        disk_drive_t *dr=&d->drv[i];
        if(!dr->inserted || !dr->motor)continue;
        if(first<0)first=i;
        if(d->selected==i)reader=i;
    }
    if(reader<0)reader=first;
    for (int i=0;i<AGA_MAX_DRIVES;i++) {
        disk_drive_t *dr=&d->drv[i];
        if(!dr->inserted || !dr->motor)continue;
        int track=dr->cyl*2+dr->side;
        if(!dr->mfm_valid || dr->mfm_track!=track) {
            encode_track(dr);
            if(dr->pos>=dr->mfm_words)dr->pos=0;
        }
        int words=dr->sectors==11 ? DSK_WORDS_PER_LINE :
                                       2*DSK_WORDS_PER_LINE;
        if(i==reader) {
            for(int n=0;n<words;n++)head_word(a,dr);
        } else {
            /* Unselected drives still rotate and fire index pulses, but
             * must not overwrite the selected drive's DSKDAT or DMA. */
            dr->pos+=words;
            if(dr->pos>=dr->mfm_words) {
                dr->pos-=dr->mfm_words;
                if(a->hooks.disk_index)a->hooks.disk_index(a->hooks.user,i);
            }
        }
    }
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
