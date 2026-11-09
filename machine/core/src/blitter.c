/* AGA-STORM — Blitter (immediate mode).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Blit engine je UAE-ov, iz WinUAE blitter.cpp i mjestimice iz njegovog
 * starijeg oblika sačuvanog u E-UAE blitter.c: do_fill je fill korak
 * blitter_dofast (kopiran, imena promijenjena); build_fill_table gradi UAE-ov
 * blit_filltable; blit_normal prevodi blitter_dofast / blitter_dofast_desc,
 * a blit_line line mode, njegove korake i rotaciju teksture u starijem obliku
 * (blitter_line_proc, blitter_nxline) redoslijedom WinUAE actually_do_blit
 * (svaki piksel upisan u BLTDPT nakon koraka, SING točka, blitter_line_adat);
 * BLTBDAT upis pomiče kao WinUAE blitter_loadbdat; ciklusi po riječi u
 * blit_duration su WinUAE-ovi (blit_cycle_diagram). Minterm funkcija, ostali
 * upisi registara te pokretanje, završavanje i mjerenje vremena blita su moji.
 *   WinUAE blitter.cpp: (c) 1995 Bernd Schmidt, Alessandro Bissacco
 *                       (c) 2002 - 2025 Toni Wilen
 *   E-UAE blitter.c:    (c) 1995 Bernd Schmidt, Alessandro Bissacco
 *                       (c) 2002 - 2005 Toni Wilen
 * Promijenjeno u odnosu na UAE/WinUAE kod za AGA-STORM 2026.
 * UAE i WinUAE su GPL, kao i ova datoteka.
 */
#include "internal.h"

static uint8_t fill_table[256][4][2];   /* [byte][ife*2 + carry_in] -> {out, carry_out} */
static int fill_table_ready;

static void build_fill_table(void)
{
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < 4; i++) {
            int fc = i & 1;
            int ife = i >> 1;
            int out = 0;
            for (int bit = 0; bit < 8; bit++) {
                int b = (d >> bit) & 1;
                int nfc = fc ^ b;
                if (ife) {
                    /* uključivi fill: bit postavljen ako je fc ili b ili novi fc */
                    if (fc || b || nfc) out |= 1 << bit;
                } else {
                    /* isključivi fill: postavljen ako je (fc xor b) - da, XOR,
                       tko bi rekao */
                    if (fc ^ b) out |= 1 << bit;
                }
                fc = nfc;
            }
            fill_table[d][i][0] = (uint8_t)out;
            fill_table[d][i][1] = (uint8_t)fc;
        }
    }
    fill_table_ready = 1;
}

void blt_reset(aga_t *a)
{
    memset(&a->blt, 0, sizeof a->blt);
    if (!fill_table_ready) build_fill_table();
}

uint16_t blt_dmaconr_bits(const aga_t *a)
{
    return (uint16_t)((a->blt.busy ? DMAF_BBUSY : 0) | (a->blt.zero ? DMAF_BZERO : 0));
}

static inline uint32_t blit_func(uint32_t sa, uint32_t sb, uint32_t sc, uint8_t mt)
{
    uint32_t d = 0;
    if (mt & 0x80) d |=  sa &  sb &  sc;
    if (mt & 0x40) d |=  sa &  sb & ~sc;
    if (mt & 0x20) d |=  sa & ~sb &  sc;
    if (mt & 0x10) d |=  sa & ~sb & ~sc;
    if (mt & 0x08) d |= ~sa &  sb &  sc;
    if (mt & 0x04) d |= ~sa &  sb & ~sc;
    if (mt & 0x02) d |= ~sa & ~sb &  sc;
    if (mt & 0x01) d |= ~sa & ~sb & ~sc;
    return d & 0xFFFF;
}

static inline uint16_t do_fill(const blitter_t *b, uint16_t d, int *fc)
{
    /* IFE i EFE zajedno postavljaju isključivi fill (WinUAE-ov "weird fill
       mode"); ime je zasluženo */
    int ife = (b->con1 & (BLTCON1_IFE | BLTCON1_EFE)) == BLTCON1_IFE ? 2 : 0;
    int fc1 = fill_table[d & 255][ife + *fc][1];
    uint16_t out = fill_table[d & 255][ife + *fc][0] | (fill_table[d >> 8][ife + fc1][0] << 8);
    *fc = fill_table[d >> 8][ife + fc1][1];
    return out;
}

static void blit_normal(aga_t *a, int hsize, int vsize)
{
    blitter_t *b = &a->blt;
    uint8_t mt = (uint8_t)(b->con0 & 0xFF);
    int usea = b->con0 & BLTCON0_USEA, useb = b->con0 & BLTCON0_USEB;
    int usec = b->con0 & BLTCON0_USEC, used = b->con0 & BLTCON0_USED;
    int ash = (b->con0 >> 12) & 15, bsh = (b->con1 >> 12) & 15;
    int desc = b->con1 & BLTCON1_DESC;
    /* fill radi i uzlazno, a carry prolazi obrnuto kroz riječi (WinUAE izvodi
       fill; njegov debugger samo upozorava "fill without desc" - hrabro) */
    int fill = (b->con1 & (BLTCON1_IFE | BLTCON1_EFE)) != 0;
    uint32_t apt = b->apt, bpt = b->bpt, cpt = b->cpt, dpt = b->dpt;
    /* prethodne riječi shiftera, očišćene pri pokretanju blita (blit_start) */
    uint32_t preva = b->aold, prevb = b->bold;
    uint32_t bhold = b->bhold;
    uint16_t cdat = b->cdat, ddat = b->ddat;
    int zero = 1;
    int dodst = 0;
    uint32_t dstp = 0;

    for (int y = 0; y < vsize; y++) {
        int fc = (b->con1 & BLTCON1_FCI) ? 1 : 0;
        for (int x = 0; x < hsize; x++) {
            uint32_t adat, ahold;
            uint16_t mask = 0xFFFF;
            if (x == 0) mask &= b->afwm;
            if (x == hsize - 1) mask &= b->alwm;

            if (usea) {
                b->adat = chip_rd16(a, apt);
                apt += desc ? -2 : 2;
            }
            adat = b->adat & mask;
            if (desc) {
                ahold = ((adat << 16) | preva) >> (16 - ash);
            } else {
                ahold = ((preva << 16) | adat) >> ash;
            }
            preva = adat;

            if (useb) {
                uint32_t bdat = b->bdat = chip_rd16(a, bpt);
                bpt += desc ? -2 : 2;
                if (desc) bhold = ((bdat << 16) | prevb) >> (16 - bsh);
                else      bhold = ((prevb << 16) | bdat) >> bsh;
                prevb = bdat;
            }
            if (usec) {
                cdat = b->cdat = chip_rd16(a, cpt);
                cpt += desc ? -2 : 2;
            }
            if (dodst) chip_wr16(a, dstp, ddat);
            ddat = (uint16_t)blit_func(ahold, bhold, cdat, mt);
            if (fill) ddat = do_fill(b, ddat, &fc);
            if (ddat) zero = 0;
            if (used) {
                dodst = 1;
                dstp = dpt;
                dpt += desc ? -2 : 2;
            }
        }
        if (usea) apt += desc ? -b->amod : b->amod;
        if (useb) bpt += desc ? -b->bmod : b->bmod;
        if (usec) cpt += desc ? -b->cmod : b->cmod;
        if (used) dpt += desc ? -b->dmod : b->dmod;
    }
    if (dodst) chip_wr16(a, dstp, ddat);

    b->apt = apt; b->bpt = bpt; b->cpt = cpt; b->dpt = dpt;
    b->aold = (uint16_t)preva; b->bold = (uint16_t)prevb;
    b->bhold = (uint16_t)bhold;
    b->ddat = ddat;
    b->zero = zero;
}

/* ---- line mode ----------------------------------------------------------- */
static void blit_line(aga_t *a, int count)
{
    blitter_t *b = &a->blt;
    uint8_t mt = (uint8_t)(b->con0 & 0xFF);
    int shift = (b->con0 >> 12) & 15;
    int bsh = (b->con1 >> 12) & 15;
    int sign = (b->con1 & BLTCON1_SIGN) ? 1 : 0;
    int sing = b->con1 & BLTCON1_SING;
    int usea = b->con0 & BLTCON0_USEA;
    int usec = b->con0 & BLTCON0_USEC;
    uint32_t cpt = b->cpt, dpt = b->dpt;
    int16_t apt = (int16_t)b->apt;   /* akumulator pogreške je u BLTAPTL */
    uint16_t blineb = (uint16_t)((b->bdat >> bsh) | (b->bdat << (16 - bsh)));
    int zero = 1;
    int onedot = 0;                  /* SING: ovaj red ima svoju točku */

    /* Kao WinUAE line mode (actually_do_blit): svaki piksel izračuna se iz
       C na BLTCPT i upiše u BLTDPT tek nakon koraka, a D zatim slijedi C.
       Zato prva točka ide kamo je program usmjerio BLTDPT - rutina za
       popunjene vektore usmjerava ga na scratch riječ da prva točka svake
       linije ostane izvan fill-a; upisana na BLTCPT, prekidala je fill tamo
       i razderala oblik - a svaka druga kamo je C pročitan. Upis traži USEC,
       ne USED (C mora biti uključen ili se ništa ne crta), i samo s USEC
       Y korak pomiče C i započinje novi red za SING. 9 Fingers i State of
       the Art poklopili su se sa stvarnim chip RAM-om nakon ovoga u RGB2RTG
       (od RGB2RTG 6429272). */
    for (int i = 0; i < count; i++) {
        int pixel = !sing || !onedot;
        onedot = 1;
        uint16_t ahold = (uint16_t)((b->adat & b->afwm & 0x8000) >> shift);
        b->aold = (uint16_t)((((uint32_t)b->aold << 16) | (b->adat & b->afwm)) >> shift);   /* blitter_line_adat */
        /* bit teksture je bit 0 BLTBDAT-a rotiran udesno za BSH: bit BSH za
           prvi piksel, zatim BSH-1, BSH-2... (WinUAE blitter_line_minterm,
           blitter_nxline). Bit 15 crtao je svaku crticu jedan piksel ranije. */
        uint16_t bhold = (blineb & 1) ? 0xFFFF : 0;
        uint16_t cdat = usec ? chip_rd16(a, cpt) : b->cdat;
        uint16_t ddat = (uint16_t)blit_func(ahold, bhold, cdat, mt);
        if (ddat) zero = 0;
        b->ddat = ddat;

        /* korak */
        if (usea) apt += sign ? b->bmod : b->amod;
        if (!sign) {
            if (b->con1 & BLTCON1_SUD) {
                if (usec) {
                    if (b->con1 & BLTCON1_SUL) cpt -= b->cmod; else cpt += b->cmod;
                    onedot = 0;
                }
            } else {
                if (b->con1 & BLTCON1_SUL) { if (shift-- == 0) { shift = 15; cpt -= 2; } }
                else                       { if (++shift == 16) { shift = 0; cpt += 2; } }
            }
        }
        if (b->con1 & BLTCON1_SUD) {
            if (b->con1 & BLTCON1_AUL) { if (shift-- == 0) { shift = 15; cpt -= 2; } }
            else                       { if (++shift == 16) { shift = 0; cpt += 2; } }
        } else if (usec) {
            if (b->con1 & BLTCON1_AUL) cpt -= b->cmod; else cpt += b->cmod;
            onedot = 0;
        }
        sign = apt < 0;
        blineb = (uint16_t)((blineb << 1) | (blineb >> 15));
        if (pixel && usec)
            chip_wr16(a, dpt, ddat);
        dpt = cpt;
    }
    b->apt = (b->apt & 0xFFFF0000u) | (uint16_t)apt;
    b->cpt = cpt;
    b->dpt = dpt;
    b->con0 = (uint16_t)((b->con0 & 0x0FFF) | (shift << 12));
    /* BSH se smanjuje za jedan po pikselu, kako se tekstura pomiče (WinUAE) */
    b->con1 = (uint16_t)((b->con1 & ~(BLTCON1_SIGN | 0xF000)) | (sign ? BLTCON1_SIGN : 0)
                         | (((bsh - count) & 15) << 12));
    b->bhold = (blineb & 1) ? 0xFFFF : 0;     /* B-ov hold: bit teksture na kojem se zaustavio */
    b->zero = zero;
}

/* Prijavi završetak blita: spusti BBUSY, podigni prekid, oslobodi copper
   koji čeka na BLTDONE. */
static void blit_finish(aga_t *a)
{
    blitter_t *b = &a->blt;
    b->busy = 0;
    b->irq_cck = 0;
    b->fin_count++;
    aga_intreq_set(a, INTF_BLIT);       /* uvijek odmah - vidi INTREQ_write */
    cop_blitter_done(a);
}

/* NAPOMENA O KAŠNJENJU, koje je sada posao GLUE-a.
 *
 * Blit ne smije prijaviti završetak unutar vlastitog upisa BLTSIZE, inače igra
 * koja postavi svoje čekanje NAKON pokretanja blita propusti prekid i čeka
 * zauvijek. To se nije promijenilo.
 *
 * Promijenio se SAT prema kojem se mjeri kašnjenje. Prije su to bili colour
 * clocks, umanjeni iz chipsetove vlastite petlje ciklusa, a to ima smisla samo
 * dok petlja vrti te cikluse stvarnom brzinom. Ne vrti: prikazivanje framea
 * stoji oko 900us, pa petlja uvijek zaostaje, a kad joj se dopusti SUSTIZANJE,
 * ona odigra tisuće colour clocks u nekoliko mikrosekundi - istekom
 * kašnjenja davno prije nego igra postavi svoje čekanje, što je upravo ta
 * utrka koju kašnjenje postoji da spriječi. To je sprezanje razlog zašto se
 * petlji nije smjelo dopustiti sustizanje i zašto je audio generiran iz istog
 * sata radio 4,5% presporo.
 *
 * Zato jezgra sada samo kaže "blit je na čekanju", a glue, koji ima pravi
 * timer, odlučuje kada je prošlo dovoljno WALL TIME-a. Dva sata su opet
 * neovisna: petlja se smije slobodno sustizati, a blit još ne može završiti
 * unutar vlastitog upisa. */

/* Koliko dugo držati blit "u radu" prije nego se prijavi završenim.
 *
 * Fiksno, kratko kašnjenje - NE vrijeme koje bi stvarni blitter uzeo.
 *
 * Modeliranje stvarnog trajanja (jedan ciklus po aktivnom kanalu po riječi)
 * točnije je i ovdje pogrešno, jer trajanje postaje nosivo čim bilo što na
 * njemu čeka. Copper lista koja čeka na BLTDONE staje za cijeli blit, a veliki
 * blit košta frameove: s cijenom po riječi, Banshee je nacrtao naslov i zatim
 * puzao, zvuk je zapeo, a njegova tipka za izlaz odgovarala je minutama.
 *
 * Kašnjenje postoji zbog točno jednog razloga: blit ne smije prijaviti
 * završetak unutar vlastitog upisa BLTSIZE, inače igra koja postavi svoje
 * čekanje NAKON pokretanja blita propusti prekid i čeka zauvijek. 64 colour
 * clocks je oko 9 mikrosekundi - mnogo više od šačice instrukcija koje igra
 * treba da postavi zastavicu, i 0,1% framea, pa ništa što čeka na blitter ne
 * primjećuje. Blit DATA gotovi su prije povratka u svakom slučaju. */
#define BLIT_DELAY_CCK 64

/* Colour clocks koje stvarni blitter potroši na blit: riječi x ciklusi po
   riječi iz kanala koje koristi (WinUAE blit_cycle_diagram: 2 za nijedan/jedan
   od A i D sam, 3 za tri kanala, 4 za ABCD; fill mode dodaje jedan kad D
   radi bez C), line mode oko 4 po pikselu. Bez sukoba s bitplane DMA, dakle
   donja granica - a glue je samo za to i treba. */
static uint32_t blit_duration(const blitter_t *b, int hsize, int vsize)
{
    static const uint8_t cyc[16] = { 2,2,2,3, 3,3,3,4, 2,2,2,3, 3,3,3,4 };
    uint32_t n;
    if (b->con1 & BLTCON1_LINE) {
        n = 4;
    } else {
        unsigned ch = (b->con0 >> 8) & 0xF;           /* USEA USEB USEC USED */
        n = cyc[ch];
        if ((b->con1 & 0x18) && (ch & 1) && !(ch & 2)) n++;   /* fill, D bez C */
    }
    return n * (uint32_t)hsize * (uint32_t)vsize + 4;
}

static void blit_start(aga_t *a, int hsize, int vsize)
{
    blitter_t *b = &a->blt;
    b->dur_cck = blit_duration(b, hsize, vsize);
    if (!aga_dma_enabled(a, DMAF_BLTEN)) {
        /* Blitter DMA onemogućen: blit je zapamćen i pokrenuo bi se kad se
           omogući. Radi jednostavnosti ipak ga izvrši - igre ga uvijek
           omoguće, koji kurac, valjda. */
    }
    if (b->busy) blit_finish(a);       /* novi blit dok je prethodni na čekanju */
    b->busy = 1;
    /* Prethodne riječi shiftera počinju od 0 (WinUAE, početak blita: bltaold =
       bltbold = 0). Nakon toga zadržavaju ono što je blit ostavio u njima, pa
       BLTBDAT upisan nakon blita bez B ubacuje nule: $FFFF sa
       BSH 12 je maska $000F, kojom Arteov rotirajući logo spaja svaki
       stupac u svoju riječ. */
    b->aold = b->bold = 0;
    a->blit_count++;
    if (b->con1 & BLTCON1_LINE) blit_line(a, vsize);
    else                        blit_normal(a, hsize, vsize);

    /* DATA je sada gotov, ali blit još ne smije prijaviti završetak.
     *
     * Dovršetak unutar upisa BLTSIZE podiže INTF_BLIT prije nego što je upisna
     * instrukcija uopće završila, a igra koja pokrene blit i tek onda postavi
     * svoje čekanje - postavi svoju zastavicu "blit running" NAKON upisa
     * BLTSIZE - prvo dobije prekid. Njezin handler očisti zastavicu koja nije
     * postavljena, igra ju postavi trenutak kasnije i zatim čeka prekid koji
     * se već dogodio. Banshee sjedi točno u takvoj petlji zauvijek:
     *
     *   3618cd9e: move.w $3618b926(pc),d7
     *   3618cda2: bne.w  $3618cd9e            ; čekaj da ga handler nulira
     *   3618cda6: lea    $dff000,a6
     *   3618cdac: move.w #$0040,$9a(a6)       ; INTENA: gotovo s INTF_BLIT
     *
     * pa njezina glavna petlja nikad ne stigne do upisa copper liste ispod,
     * ništa se ne crta i zaslon ostaje prazan dok igra veselo radi.
     *
     * Odgađanje za vrijeme koje bi stvarni blitter uzeo popravlja
     * handshake i to hardver ionako radi. Ono i BBUSY u DMACONR-u i
     * copperovo čekanje na BLTDONE čini ispravnima, a oboje već čita
     * b->busy. */
    if (a->cfg.blit_immediate) blit_finish(a);
    else                       b->irq_cck = 1;   /* na čekanju; glue mu mjeri vrijeme */
}

/* Čeka li blit da bude prijavljen završenim? */
int aga_blit_pending(aga_t *a)
{
    return a->blt.busy && a->blt.irq_cck;
}

uint32_t aga_blit_duration(aga_t *a)
{
    return a->blt.dur_cck;
}

/* 0: blitovi završeni, 1: potvrde koje su očistile dva ili više dovršetaka
   odjednom (program koji odgovara na svaki BLIT prekid izgubio je jedan), 2:
   frame posljednjeg takvog */
void aga_blit_set_nohold(aga_t *a, int on) { a->blt.no_hold = on; }

uint32_t aga_peek_blit_irq(const aga_t *a, int i)
{
    return i == 0 ? a->blt.fin_count : i == 1 ? a->blt.lost : i == 2 ? a->blt.lost_frame : a->blt.held_count;
}

/* Prošlo je dovoljno wall time-a: prijavi. */
void aga_blit_complete(aga_t *a)
{
    if (a->blt.busy && a->blt.irq_cck) blit_finish(a);
}

void blt_write(aga_t *a, uint32_t reg, uint16_t v)
{
    blitter_t *b = &a->blt;
    switch (reg) {
    case BLTCON0:  b->con0 = v; break;
    case BLTCON0L: b->con0 = (uint16_t)((b->con0 & 0xFF00) | (v & 0xFF)); break;
    case BLTCON1:  b->con1 = v; break;
    case BLTAFWM:  b->afwm = v; break;
    case BLTALWM:  b->alwm = v; break;
    case BLTCPTH:  b->cpt = (b->cpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTCPTL:  b->cpt = (b->cpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTBPTH:  b->bpt = (b->bpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTBPTL:  b->bpt = (b->bpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTAPTH:  b->apt = (b->apt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTAPTL:  b->apt = (b->apt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTDPTH:  b->dpt = (b->dpt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case BLTDPTL:  b->dpt = (b->dpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTSIZE: {
        int h = v & 0x3F; if (h == 0) h = 64;
        int vv = v >> 6;  if (vv == 0) vv = 1024;
        blit_start(a, h, vv);
        break;
    }
    case BLTSIZV:  b->sizev = v & 0x7FFF; break;
    case BLTSIZH: {
        int h = v & 0x7FF; if (h == 0) h = 2048;
        int vv = b->sizev; if (vv == 0) vv = 32768;
        blit_start(a, h, vv);
        break;
    }
    case BLTCMOD:  b->cmod = (int16_t)(v & 0xFFFE); break;
    case BLTBMOD:  b->bmod = (int16_t)(v & 0xFFFE); break;
    case BLTAMOD:  b->amod = (int16_t)(v & 0xFFFE); break;
    case BLTDMOD:  b->dmod = (int16_t)(v & 0xFFFE); break;
    case BLTCDAT:  b->cdat = v; break;
    case BLTBDAT: {                     /* pomiče se pri učitavanju (HRM; WinUAE blitter_loadbdat) */
        int sh = b->con1 >> 12;
        b->bdat = v;
        b->bhold = (b->con1 & BLTCON1_DESC) ? (uint16_t)((((uint32_t)v << 16) | b->bold) >> (16 - sh))
                                            : (uint16_t)((((uint32_t)b->bold << 16) | v) >> sh);
        b->bold = v;
        break;
    }
    case BLTADAT:  b->adat = v; break;
    default: break;
    }
}
