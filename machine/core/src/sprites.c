/* AGA-STORM — sprite DMA i crtanje spriteova po retku.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Iz UAE/WinUAE: spr_startstop je WinUAE-ov sprstartstop, pozvan oko upisa
 * SPRxPOS/SPRxCTL kao tamo, a DMA koraci ciklusa u spr_dma_slot su
 * generate_sprites (custom.cpp; oboje kopirano); SPRES preklop slijedi
 * update_sprres (drawing.cpp), a VSTART/VSTOP dekodira SPRxCTLPOS
 * (custom.cpp); vblank zaštita i dekodiranje slota slijede do_sprites u
 * E-UAE custom.c. Sprite vblank retci (25/20) i pomak pozicije +9
 * (DIW_DDF_OFFSET) su UAE-ove konstante, a pozicija sprite DMA slota
 * izmjerena je iz WinUAE DMA zapisa. SSCAN2 slijedi WinUAE: osmobitno
 * podudaranje pozicije je denise_xposmask (drawing.cpp, expand_fmode
 * i matchsprites2_aga), preskočeno dohvaćanje generate_spritesov dblscan test
 * (custom.cpp). Crtanje, reprodukcija prepisivanja usred retka i grupe
 * sudara vlastite su ovom projektu. Spriteovi su jednostavni, dok ne počnu
 * mijenjati rezoluciju usred retka.
 *   WinUAE custom.cpp:  Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   WinUAE drawing.cpp: Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   E-UAE custom.c:     Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2004 Toni Wilen
 * Promijenjeno u odnosu na UAE/WinUAE kod za AGA-STORM 2026.
 * UAE i WinUAE su GPL, kao i ova datoteka.
 */
#include "internal.h"

/* Sprite DMA slotovi: WinUAE zapisi stavljaju SPRxPOS na 0x19+4x i SPRxCTL na
   0x1B+4x (sprite 0 u ciklusima 25 i 27), pa sprite 7 završava u ciklusu 55. */
#define SPR0_HPOS 0x19

/* pozicija u sprlineu (shres) na kojoj upis registra učinjen sada stupa na snagu:
   ista pipeline odgoda kao promjena boje (playfield.c: hpos*4+2 hires). Ništa
   ne stupa na snagu odmah, sranje jedno. */
#define SPR_NOW(a) ((a)->hpos * 8 + 4)

void spr_reset(aga_t *a)
{
    memset(a->spr, 0, sizeof a->spr);
    a->sprite_width = 16;
    a->sprres = RES_LORES;
}

void spr_expand_res(aga_t *a)
{
    int old_res = a->sprres;
    switch ((pf_con3(&a->pf) >> 6) & 3) {
    case 0:  a->sprres = (a->pf.res == RES_SHRES) ? RES_HIRES : RES_LORES; break;
    case 1:  a->sprres = RES_LORES; break;
    case 2:  a->sprres = RES_HIRES; break;
    default: a->sprres = RES_SHRES; break;
    }
    if (a->sprres != old_res && a->nsprres_ev < 16) {
        a->sprres_ev_sx[a->nsprres_ev] = SPR_NOW(a);
        a->sprres_ev_res[a->nsprres_ev] = (int8_t)a->sprres;
        a->nsprres_ev++;
    }
    switch ((a->pf.fmode >> 2) & 3) {
    case 0:  a->sprite_width = 16; break;
    case 3:  a->sprite_width = 64; break;
    default: a->sprite_width = 32; break;
    }
}

static void spr_update_posctl(aga_t *a, int n)
{
    sprite_t *s = &a->spr[n];
    /* xpos u shres pikselima (4 po lores pikselu), DIW koordinate */
    s->xpos = (((s->pos & 0xFF) << 1) | (s->ctl & 1)) << 2;
    s->xpos |= (s->ctl >> 3) & 3;
    /* Izlaz je hires: WinUAE preslikava spriteov superhires početak na
   hires piksel u koji pada. Ostavljen na neparnoj superhires poziciji, sprite
   crtao je jedan hires piksel previše udesno - tekstne sjene Reshoot Proxima 3
   (SH0/SH1 = 3) izašle su dvostruko šire. */
    s->xpos &= ~1;
    s->vstart = (s->pos >> 8) | ((s->ctl & 0x04) ? 0x100 : 0) | ((s->ctl & 0x40) ? 0x200 : 0);
    s->vstop  = (s->ctl >> 8) | ((s->ctl & 0x02) ? 0x100 : 0) | ((s->ctl & 0x20) ? 0x200 : 0);
    s->attach = (s->ctl & 0x80) ? 1 : 0;
}

/* Gdje se sprite na xpos (shres, DIW koordinate) pokazuje na retku. Sa
   SSCAN2 (FMODE bit 15) Denise uspoređuje samo niskih osam bitova lores
   pozicije - SPRxPOS bit 7, bit od 256 piksela, tada je spriteova zastavica
   scan-doublinga - pa se svaki sprite pokazuje dvaput po retku, 256 lores
   piksela razmaknut. Castlevania AGA (Scorpion Engine) tako popločuje svoj
   udaljeni sloj: četiri spritea od 64 piksela jedan uz drugi, ponovljeni.
   Po jedna kopija crtala je oblake i planine preko lijevih 128 piksela
   područja igre, a ništa desno od njih. */
static inline int spr_copies(const aga_t *a, int xpos, int x[2])
{
    if (!(a->pf.fmode & 0x8000)) { x[0] = xpos; return 1; }
    x[0] = xpos & ~0x400;
    x[1] = x[0] | 0x400;
    return 2;
}

/* WinUAE sprstartstop(): početni i završni redak uspoređuju se s trenutačnim
   retkom kad god se upiše SPRxPOS ili SPRxCTL - od coppera, CPU-a ili samog
   sprite DMA - ne samo u spriteovu DMA slotu. Zato copper lista može
   isključiti sprite DMA (završni redak = ovaj redak) ili ga uključiti (početni
   redak = ovaj redak) usred zaslona; provjereno samo u slotu, stop upisan
   nakon njega nikad se ne poklopi i sprite nastavlja čitati memoriju sve do
   dna zaslona. */
static void spr_startstop(aga_t *a, sprite_t *s)
{
    if (a->vpos < a->sprite_vblank) return;
    if (a->vpos == s->vstart) s->dmastate = 1;
    if (a->vpos == s->vstop || a->vpos == a->sprite_vblank) { s->dmastate = 0; s->dmacycle = 0; }
}

/* ---- spriteovi prepisani dok je zraka na retku ------------------------------
   Sprite se pokazuje gdje zraka naiđe na njegovu vodoravnu poziciju dok je
   aktiviran: podaci koje tada drži učitaju se u njegov shifter i ispomaknu
   van, a kasnije podudaranje - copper ga je pomaknuo dalje - učitava ih
   ponovno. Zato se jedan sprite može pojaviti nekoliko puta na retku
   (Reshoot Proxima 3 pomiče sprite 6 tri puta u svakom retku dviju razina).
   Crtanje svakog spritea jednom, iz njegovog stanja na kraju retka, pokazuje
   samo posljednju kopiju.
   Prvi put kad copper ili CPU upiše sprite tijekom retka, njegovo dotadašnje
   stanje se zabilježi, a od tada svaka promjena, pa spr_build_line može
   odigrati taj redak. Redak u kojem se ništa ne upisuje u sprite crta se
   točno kao prije. */
static void spr_log(aga_t *a, int n, int sx)
{
    if (a->nsprev >= AGA_SPR_EVENTS) { a->sprev_full = 1; return; }
    const sprite_t *s = &a->spr[n];
    sprev_t *e = &a->sprev[a->nsprev++];
    e->sx = sx;
    e->n = (uint8_t)n;
    e->armed = (uint8_t)(s->armed != 0);
    e->xpos = s->xpos;
    e->data = s->data;
    e->datb = s->datb;
}

static void spr_touch(aga_t *a, int n)
{
    if (a->spr_touched & (1u << n)) return;
    a->spr_touched |= 1u << n;
    spr_log(a, n, -0x40000000);          /* stanje koje je imao od početka retka */
}

void spr_start_line(aga_t *a)
{
    a->sprres_line0 = a->sprres;
    a->nsprres_ev = 0;
    a->nsprev = 0;
    a->sprev_full = 0;
    a->spr_touched = 0;
}

static inline uint64_t widen16(uint16_t v)
{
    /* CPU upis od 16 bitova popunjava sve riječi širine dohvaćanja (UAE ponašanje) */
    uint64_t x = v;
    return x | (x << 16) | (x << 32) | (x << 48);
}

void spr_write(aga_t *a, uint32_t reg, uint16_t v)
{
    if (reg >= SPR0PTH && reg < SPR0PTH + 8 * 4) {
        int n = (reg - SPR0PTH) >> 2;
        sprite_t *s = &a->spr[n];
        if ((reg & 2) == 0) s->pt = (s->pt & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16);
        else                s->pt = (s->pt & 0xFFFF0000u) | (v & 0xFFFE);
        return;
    }
    if (reg >= SPR0POS && reg < SPR0POS + 8 * 8) {
        int n = (reg - SPR0POS) >> 3;
        sprite_t *s = &a->spr[n];
        spr_touch(a, n);
        switch (reg & 6) {
        case 0: s->pos = v; spr_startstop(a, s); spr_update_posctl(a, n); spr_startstop(a, s); break;
        case 2: s->ctl = v; s->armed = 0; spr_startstop(a, s); spr_update_posctl(a, n); spr_startstop(a, s); break;
        case 4: s->data = widen16(v); s->armed = 1; break;
        case 6: s->datb = widen16(v); break;
        }
        spr_log(a, n, SPR_NOW(a));
    }
}

void spr_vsync(aga_t *a)
{
    for (int i = 0; i < AGA_MAX_SPRITES; i++) a->spr[i].dmastate = a->spr[i].dmacycle = 0;
}

static uint64_t spr_fetch(aga_t *a, uint32_t addr)
{
    switch (a->sprite_width) {
    case 16: return (uint64_t)chip_rd16(a, addr) << 48;
    case 32: return (uint64_t)chip_rd32(a, addr) << 32;
    default: return chip_rd64(a, addr);
    }
}

/* Poziva se na colour clocks 0x19.. za svaki sprite slot; dva pristupa po
   spriteu: (hpos - 0x19) & 3 == 0 -> prva riječ (POS ili DATA), == 2 -> druga
   (CTL ili DATB). Stanje stroja je WinUAE-ov (generate_sprites): dmacycle
   kaže dohvaća li ovaj redak uopće, dmastate dohvaća li podatke ili
   kontrolne riječi. Dekodiranje slota i vblank zaštita slijede E-UAE
   do_sprites - ručno, kao i sve ovdje. */
void spr_dma_slot(aga_t *a, int hpos)
{
    int off = hpos - SPR0_HPOS;
    int n = off >> 2;
    int cycle = off & 3;
    if (n < 0 || n >= AGA_MAX_SPRITES || (cycle & 1)) return;
    cycle >>= 1;
    sprite_t *s = &a->spr[n];
    int vblank_end = a->sprite_vblank;   /* spriteovi se ponovno pokreću na vlastitom retku, ne na retku prikaza */

    if (a->vpos < vblank_end) return;
    /* nema sprite DMA na posljednjem retku framea (WinUAE: Super Stardust ne
   dohvaća ništa na retku 312, gdje sam dohvatio osam podatkovnih riječi) */
    if (a->vpos >= a->maxvpos + (a->lof ? 1 : 0) - 1) return;

    if (cycle == 0) {
        if (!s->dmacycle && s->dmastate) s->dmacycle = 1;
        if (a->vpos == s->vstart) { s->dmastate = 1; s->dmacycle = 1; }
        if (a->vpos == s->vstop || a->vpos == vblank_end) { s->dmastate = 0; s->dmacycle = 1; }
        s->dmadata = s->dmastate;        /* obje riječi ovog retka iste su vrste */
    }
    /* SSCAN2 scan-doubling: sprite s postavljenim bitom 7 u SPRxPOS dohvaća
   podatke samo na retcima parnosti svog početnog retka, pa se svaki podatkovni
   redak pokazuje dvaput */
    int dbl = s->dmadata && (a->pf.fmode & 0x8000) && (s->pos & 0x80) && ((a->vpos ^ s->vstart) & 1);
    /* Nema sprite slota dok traje dohvaćanje bitplanea u retku (WinUAE do_sprites:
   samo ako bprun != 1): ništa se ne dohvaća i pokazivač ostaje, iako
   stanje stroja ispod ipak korača. Copper u Brian the Lion postavlja
   DDFSTRT $30, pa sprite 7 slotovi pripadaju bitplaneima - dohvatio sam ga
   sa zastarjelog pokazivača i aktivirao smećem (tamno-plave crtice;
   iz RGB2RTG, EMM #2072). OCS Agnusov off-by-one i rub
   DDFSTOP bplconflict nisu modelirani. */
    if (s->dmacycle && aga_dma_enabled(a, DMAF_SPREN) && !dbl && !pf_bpl_running(a, hpos)) {
        uint32_t bytes = a->sprite_width >> 3;
        if (!s->dmadata) {
            aga_trace_at(a, hpos, AGA_DMA_SPRITE,
                         (uint16_t)(SPR0POS + 8 * n + 2 * cycle), s->pt);
            uint16_t w = chip_rd16(a, s->pt);
            s->pt += bytes;
            if (cycle == 0) { s->pos = w; spr_update_posctl(a, n); }
            else            { s->ctl = w; s->armed = 0; spr_update_posctl(a, n); }
            /* Dohvaćene riječi stižu u registre kasnije u WinUAE pipelineu, nakon što
   je odlučen drugi slot ovog retka: usporedi nove početne/završne retke tek
   kad su oba unutra. Usporedba nakon POS-a, dok se stari završni redak još
   poklapa, otkazala bi CTL dohvaćanje. */
            if (cycle == 1) spr_startstop(a, s);
        } else {
            aga_trace_at(a, hpos, AGA_DMA_SPRITE,
                         (uint16_t)(SPR0DATA + 8 * n + 2 * cycle), s->pt);
            uint64_t d = spr_fetch(a, s->pt);
            s->pt += bytes;
            if (cycle == 0) { s->data = d; s->armed = 1; }
            else            { s->datb = d; }
        }
        if (a->spr_touched & (1u << n)) spr_log(a, n, hpos * 8 + 4);
    }
    if (s->dmacycle) {
        s->dmacycle++;
        if (s->dmacycle > 2) s->dmacycle = s->dmastate ? 1 : 0;
    }
}

/* zabilježi zapisani raspon sprlinea, da ga sljedeća izgradnja očisti -
   čišćenje cijelih 2 KB po retku je za one s vremenom na bacanje */
static inline void spr_mark(aga_t *a, int x0, int x1, int linelen)
{
    if (x0 < 0) x0 = 0;
    if (x1 > linelen) x1 = linelen;
    if (x0 < a->sprline_lo) a->sprline_lo = x0;
    if (x1 > a->sprline_hi) a->sprline_hi = x1;
}

/* sljedeći događaj istog spritea za svaki događaj: njegovo vrijeme */
static void spr_next_events(const aga_t *a, int *t_after)
{
    int last[AGA_MAX_SPRITES];
    for (int n = 0; n < AGA_MAX_SPRITES; n++) last[n] = 0x40000000;
    for (int i = a->nsprev - 1; i >= 0; i--) {
        t_after[i] = last[a->sprev[i].n];
        last[a->sprev[i].n] = a->sprev[i].sx;
    }
}

/* Gdje se sprite n pokazuje na odigranom retku (pozicija u sprlineu, s
   uključenim +9 lores pomakom), i s kojim podacima: njegove kopije redom
   nastanka, najviše AGA_SPR_EVENTS. Kopija završava gdje počinje sljedeća
   kopija istog spritea. Spriteovi u koje se ništa nije upisivalo zadržavaju
   svoju jedinu kopiju s kraja retka (dvije sa SSCAN2). */
static int spr_line_copies(const aga_t *a, int n, const int *t_after, int *cx, uint64_t *cda, uint64_t *cdb)
{
    const sprite_t *s = &a->spr[n];
    int w = a->sprite_width;
    const int hwoff = 9 * 4;
    int xs[2], ncp, nc = 0;
    if (!(a->spr_touched & (1u << n))) {
        if (s->armed) {
            ncp = spr_copies(a, s->xpos, xs);
            for (int c = 0; c < ncp; c++, nc++) { cx[nc] = xs[c] + hwoff; cda[nc] = s->data; cdb[nc] = s->datb; }
        }
        return nc;
    }
    for (int i = 0; i < a->nsprev; i++) {
        const sprev_t *e = &a->sprev[i];
        if (e->n != n) continue;
        int t_next = t_after[i];
        /* aktiviran kad ga zraka naiđe, s podacima za učitavanje (WinUAE
   pokreće shifter samo za podatke različite od nule) */
        if (!e->armed || !((e->data | e->datb) >> (64 - w))) continue;
        ncp = spr_copies(a, e->xpos, xs);
        for (int c = 0; c < ncp && nc < AGA_SPR_EVENTS; c++) {
            /* E-UAE decide_sprites: prije upisa na colour clocku h svaki
               sprite čija je pozicija komparatora (bez pomaka prikaza) ispod
               h*2-6 lores piksela poklapa se sa starim stanjem, a onaj točno
               na granici već vidi upis: dakle >= sx-28 i
               < t_next-28 u jedinicama sprlinea. WinUAE-ov Denise brojač stoji
               na 2h-8 (drawing.cpp: 3 refresh pomak + 2 pipeline odgode), a
               upis registra stupa na snagu jedan colour clock nakon svog slota,
               što granicu stavlja na 2h-6. Oblaci u Brian the Lion - sprite
               7 koji copper postavlja 2-4 puta po retku, SPR7DATA točno na
               clocku kad njegova kopija počinje - pokazivali su svaki komad s
               podacima prethodnog komada; ReshootR pomiče sprite 6 (njegov
               sloj stijena) 4-5 puta po retku sa svakim SPR6POS točno na
               granici, a s h*2-3 svaka kopija je ispuštena i stijena je izašla
               kao jednolične trake boja (iz RGB2RTG, EMM #2072 pa #2099). */
            if (xs[c] >= e->sx - 28 && xs[c] < t_next - 28) { cx[nc] = xs[c] + hwoff; cda[nc] = e->data; cdb[nc] = e->datb; nc++; }
        }
    }
    return nc;
}

/* Jedna kopija spritea u njegov međuspremnik v, zajedno s prozirnim
   pikselima, do limita: vraća gdje je završila. Rezolucija se može mijenjati
   duž retka (sprres_ev): sprite piksel širok je koliko i rezolucija u kojoj je
   zraka. */
static int spr_copy_to_buffer(const aga_t *a, uint8_t *v, int x, int limit, uint64_t da, uint64_t db)
{
    int w = a->sprite_width;
    if (!a->nsprres_ev) {
        /* jedna rezolucija cijeli redak: sprite piksel i pokriva 1 << sh izlaznih
   piksela, pa se njegova vrijednost izračuna jednom i upiše kao niz (od
   RGB2RTG a861e51: piksel po piksel ova petlja bila je oko osmine
   chipset vremena Jim Powera na PC-u; slike su iste) */
        int sh = 2 - a->sprres_line0;
        int end = x + (w << sh);
        if (end > limit) end = limit;
        if (end < x) end = x;
        int px = x < 0 ? 0 : x;
        while (px < end) {
            int i = (px - x) >> sh, b = 63 - i;
            uint8_t val = (uint8_t)(((da >> b) & 1) | (((db >> b) & 1) << 1));
            int nxt = x + ((i + 1) << sh);
            if (nxt > end) nxt = end;
            do v[px] = val; while (++px < nxt);
        }
        return end;
    }
    /* rezolucija na snazi gdje ova kopija počinje */
    int res = a->sprres_line0, ev = 0;
    while (ev < a->nsprres_ev && a->sprres_ev_sx[ev] <= x) res = a->sprres_ev_res[ev++];
    int pixw = 4 >> res, step = 0, bit = 0;
    for (; x < limit && bit < w; x++) {
        /* promjena rezolucije koju je zraka dosegla: odavde pomiči na novoj širini */
        while (ev < a->nsprres_ev && a->sprres_ev_sx[ev] <= x) {
            pixw = 4 >> a->sprres_ev_res[ev++];
            if (step >= pixw) { step = 0; bit++; }
        }
        if (bit >= w) break;
        if (x >= 0) {
            int sh = 63 - bit;
            v[x] = (uint8_t)(((da >> sh) & 1) | (((db >> sh) & 1) << 1));
        }
        if (++step >= pixw) { step = 0; bit++; }
    }
    return x;
}

/* Reprodukcija kroz vlastite međuspremnike spriteova, za ono što treba
   vlastite piksele spritea: sudari i promjena rezolucije duž retka. Gleda
   samo gdje su kopije: svaka kopija bilježi raspon svog međuspremnika koji je
   upisala, a prioritet se primjenjuje i međuspremnici čiste samo nad tim
   rasponima - prije je to bio cijeli raspon od krajnjeg lijevog do krajnjeg
   desnog sprite piksela, u dubini od osam međuspremnika, svaki redak. Od
   RGB2RTG 0.75-dev13 (istog autora). */
#define SPR_SPANS (2 * AGA_SPR_EVENTS + 2 * AGA_MAX_SPRITES)
static void spr_replay_buffers(aga_t *a, int clx)
{
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    int lo = linelen, hi = 0;
    int16_t sp0[SPR_SPANS], sp1[SPR_SPANS];
    uint8_t spn[SPR_SPANS];
    int nsp = 0;
    int t_after[AGA_SPR_EVENTS];
    spr_next_events(a, t_after);

    for (int n = 0; n < AGA_MAX_SPRITES; n++) {
        int cx[AGA_SPR_EVENTS];
        uint64_t cda[AGA_SPR_EVENTS], cdb[AGA_SPR_EVENTS];
        int nc = spr_line_copies(a, n, t_after, cx, cda, cdb);
        for (int k = 0; k < nc; k++) {
            int limit = (k + 1 < nc) ? cx[k + 1] : linelen;   /* sljedeća kopija preuzima */
            if (limit > linelen) limit = linelen;
            int xs = cx[k];
            int x = spr_copy_to_buffer(a, a->spv[n], xs, limit, cda[k], cdb[k]);
            if (xs < 0) xs = 0;
            if (xs < lo) lo = xs;
            if (x > hi) hi = x;
            if (x > xs && nsp < SPR_SPANS) {
                sp0[nsp] = (int16_t)xs; sp1[nsp] = (int16_t)x; spn[nsp] = (uint8_t)n; nsp++;
            }
        }
    }

    a->sprclx_valid = clx;
    if (clx) memset(a->sprclx, 0, sizeof a->sprclx);
    /* rasponi poredani po početku, preklapajući spojeni: svaki piksel koji je
   bilo koja kopija upisala, jednom */
    for (int i = 1; i < nsp; i++) {
        int16_t a0 = sp0[i], a1 = sp1[i];
        uint8_t an = spn[i];
        int j = i - 1;
        for (; j >= 0 && sp0[j] > a0; j--) { sp0[j + 1] = sp0[j]; sp1[j + 1] = sp1[j]; spn[j + 1] = spn[j]; }
        sp0[j + 1] = a0; sp1[j + 1] = a1; spn[j + 1] = an;
    }
    int attach[4];
    for (int pair = 0; pair < 4; pair++) attach[pair] = a->spr[pair * 2 + 1].attach;
    for (int i = 0; i < nsp; ) {
        int x0 = sp0[i], x1 = sp1[i];
        for (i++; i < nsp && sp0[i] <= x1; i++)
            if (sp1[i] > x1) x1 = sp1[i];
        for (int x = x0; x < x1; x++) {
            uint8_t v = 0;
            for (int pair = 0; pair < 4; pair++) {
                int e = a->spv[pair * 2][x], o = a->spv[pair * 2 + 1][x];
                if (!(e | o)) continue;
                if (attach[pair]) v = (uint8_t)(e | (o << 2) | 0x30 | (pair << 6));
                else if (e)       v = (uint8_t)(e | (pair << 6));
                else              v = (uint8_t)(o | 0x20 | (pair << 6));
                break;
            }
            line[x] = v;
            if (clx) {
                uint8_t m = 0;
                for (int n = 0; n < AGA_MAX_SPRITES; n++) if (a->spv[n][x]) m |= (uint8_t)(1u << n);
                a->sprclx[x] = m;
            }
        }
    }
    if (lo < a->sprline_lo) a->sprline_lo = lo;
    if (hi > a->sprline_hi) a->sprline_hi = hi;
    /* ostavi međuspremnike čistima za sljedeći odigrani redak: svaki tamo gdje su njegove kopije pisale */
    for (int i = 0; i < nsp; i++)
        memset(&a->spv[spn[i]][sp0[i]], 0, (size_t)(sp1[i] - sp0[i]));
}

/* jedna kopija izravno u sprite redak, a njezini prozirni pikseli ostavljaju
   ono što je tamo: piksel px iz [x, end) pokazuje bit 63 - ((px - x) >> sh) */
static inline void spr_draw_copy(uint8_t *line, int x, int end, int sh, uint64_t da, uint64_t db, unsigned flags)
{
    for (int px = x < 0 ? 0 : x; px < end; ) {
        int i = (px - x) >> sh;
        int nxt = x + ((i + 1) << sh);
        if (nxt > end) nxt = end;
        unsigned val = (unsigned)((da >> (63 - i)) & 1) | (unsigned)(((db >> (63 - i)) & 1) << 1);
        if (val) {
            uint8_t v = (uint8_t)(val | flags);
            do line[px] = v; while (++px < nxt);
        } else
            px = nxt;
    }
}

/* Odigraj redak u kojem je copper ili CPU prepisao spriteove. Svaka kopija
   svakog spritea crta se izravno u sprite redak, najniži prioritet prvi -
   par 3 do par 0, u paru neparni sprite prije parnog - svaki puni piksel
   preko onoga što je tamo: gdje se spriteovi sretnu, pokazuje se najniže
   numerirani s pikselom, kako Denise ima. Međuspremnici ostaju za ono što
   treba vlastite piksele spritea: spojeni par (njegova boja su bitovi obaju
   spriteova), kopije jednog spritea jedna preko druge (prozirni pikseli
   kasnije skrivaju ranije), sudari i promjena rezolucije duž retka
   (spr_replay_buffers). Isti bajtovi izlaze kao iz međuspremnika:
   host/tests/test_sprreplay.c crta slučajne retke na oba načina. Od
   RGB2RTG 0.75-dev14 (istog autora): copper Jim Powera ponavlja spriteove
   duž svojih redaka, 49 -> 10 us po odigranom retku na Pi 3. */
#ifdef AGA_SPR_REPLAY_CHECK
unsigned long aga_spr_replay_checked, aga_spr_replay_mismatch, aga_spr_replay_dirty;
static int spr_buffers_clear(const aga_t *a)
{
    for (int n = 0; n < AGA_MAX_SPRITES; n++)
        for (size_t x = 0; x < sizeof a->spv[n]; x++)
            if (a->spv[n][x]) return 0;
    return 1;
}
#endif

static void spr_replay_line(aga_t *a, int clx)
{
    if (clx || a->nsprres_ev) { spr_replay_buffers(a, clx); return; }
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    int w = a->sprite_width, sh = 2 - a->sprres_line0;
    int lo = linelen, hi = 0;
    int t_after[AGA_SPR_EVENTS];
    spr_next_events(a, t_after);
    int cx[AGA_SPR_EVENTS], ex[AGA_SPR_EVENTS];
    uint64_t cda[AGA_SPR_EVENTS], cdb[AGA_SPR_EVENTS];

    a->sprclx_valid = 0;
    for (int pair = 3; pair >= 0; pair--) {
        int att = a->spr[pair * 2 + 1].attach;
        for (int which = 1; which >= 0; which--) {
            int n = pair * 2 + which;
            int nc = spr_line_copies(a, n, t_after, cx, cda, cdb);
            int overlap = 0, maxend = -0x40000000;
            for (int k = 0; k < nc; k++) {
                int limit = (k + 1 < nc) ? cx[k + 1] : linelen;   /* sljedeća kopija preuzima */
                if (limit > linelen) limit = linelen;
                int end = cx[k] + (w << sh);
                if (end > limit) end = limit;
                if (end < cx[k]) end = cx[k];
                ex[k] = end;
                int xs = cx[k] < 0 ? 0 : cx[k];
                if (xs < lo) lo = xs;
                if (end > hi) hi = end;
                if (end > xs) {
                    if (xs < maxend) overlap = 1;
                    if (end > maxend) maxend = end;
                }
            }
            if (!att && !overlap) {
                unsigned flags = (unsigned)(pair << 6) | (which ? 0x20u : 0);
                for (int k = 0; k < nc; k++) spr_draw_copy(line, cx[k], ex[k], sh, cda[k], cdb[k], flags);
                continue;
            }
            /* u vlastiti međuspremnik spritea, kopije svojim redoslijedom */
            for (int k = 0; k < nc; k++) spr_copy_to_buffer(a, a->spv[n], cx[k], ex[k], cda[k], cdb[k]);
            if (att && which)
                continue;                   /* spojeni par: zatim parni sprite, pa oba odjednom */
            const uint8_t *ve = a->spv[pair * 2], *vo = a->spv[pair * 2 + 1];
            if (!att) {
                unsigned flags = (unsigned)(pair << 6) | (which ? 0x20u : 0);
                const uint8_t *v = a->spv[n];
                for (int k = 0; k < nc; k++)
                    for (int px = cx[k] < 0 ? 0 : cx[k]; px < ex[k]; px++)
                        if (v[px]) line[px] = (uint8_t)(v[px] | flags);
                for (int k = 0; k < nc; k++)
                    if (ex[k] > (cx[k] < 0 ? 0 : cx[k]))
                        memset(&a->spv[n][cx[k] < 0 ? 0 : cx[k]], 0, (size_t)(ex[k] - (cx[k] < 0 ? 0 : cx[k])));
                continue;
            }
            /* spojeni par: četiri bita boje gdje bilo koji ima piksel, preko kopija
   obaju spriteova - rasponi neparnog spritea nalaze se ponovno (njegove
   kopije pregazile su parne) */
            int ox[AGA_SPR_EVENTS], oe[AGA_SPR_EVENTS];
            int no = 0;
            {
                uint64_t da2[AGA_SPR_EVENTS], db2[AGA_SPR_EVENTS];
                int nc2 = spr_line_copies(a, pair * 2 + 1, t_after, ox, da2, db2);
                for (int k = 0; k < nc2; k++) {
                    int limit = (k + 1 < nc2) ? ox[k + 1] : linelen;
                    if (limit > linelen) limit = linelen;
                    int end = ox[k] + (w << sh);
                    if (end > limit) end = limit;
                    if (end < ox[k]) end = ox[k];
                    oe[k] = end;
                }
                no = nc2;
            }
            for (int set = 0; set < 2; set++) {
                const int *sx = set ? ox : cx, *se = set ? oe : ex;
                int ns = set ? no : nc;
                for (int k = 0; k < ns; k++)
                    for (int px = sx[k] < 0 ? 0 : sx[k]; px < se[k]; px++) {
                        int e = ve[px], o = vo[px];
                        if (e | o) line[px] = (uint8_t)(e | (o << 2) | 0x30 | (pair << 6));
                    }
            }
            for (int set = 0; set < 2; set++) {
                const int *sx = set ? ox : cx, *se = set ? oe : ex;
                int ns = set ? no : nc;
                uint8_t *v = a->spv[set ? pair * 2 + 1 : pair * 2];
                for (int k = 0; k < ns; k++) {
                    int xs = sx[k] < 0 ? 0 : sx[k];
                    if (se[k] > xs) memset(&v[xs], 0, (size_t)(se[k] - xs));
                }
            }
        }
    }
    if (lo < a->sprline_lo) a->sprline_lo = lo;
    if (hi > a->sprline_hi) a->sprline_hi = hi;
#ifdef AGA_SPR_REPLAY_CHECK
    {
        static uint8_t direct[sizeof a->sprline];
        if (!spr_buffers_clear(a)) aga_spr_replay_dirty++;
        memcpy(direct, line, sizeof direct);
        memset(line, 0, sizeof a->sprline);
        spr_replay_buffers(a, 0);
        aga_spr_replay_checked++;
        if (memcmp(direct, line, sizeof direct)) aga_spr_replay_mismatch++;
        if (!spr_buffers_clear(a)) aga_spr_replay_dirty++;
    }
#endif
}

/* Iscrtaj sve aktivirane spriteove za trenutačni redak u a->sprline (shres
   pikseli, ishodište DIW koordinata). Kodiranje: bitovi 0-3 boja, bit 4
   spojen, bit 5 neparna osnova, bitovi 6-7 par, 0 = prozirno. S clx,
   a->sprclx dobiva i po jedan bit po spriteu: sudari trebaju svaki sprite,
   i one koje drugi sprite skriva. */
void spr_build_line(aga_t *a, int clx)
{
    uint8_t *line = a->sprline;
    int linelen = (int)sizeof a->sprline;
    /* čisti samo ono što je prošla izgradnja upisala: ostalo je već nula, a
   čišćenje 2 KB po retku bilo je većina troška ove funkcije */
    if (a->sprline_hi > a->sprline_lo) memset(line + a->sprline_lo, 0, (size_t)(a->sprline_hi - a->sprline_lo));
    a->sprline_lo = linelen; a->sprline_hi = 0;
    a->sprline_valid = 1;

    if ((a->spr_touched || a->nsprres_ev) && !a->sprev_full) { spr_replay_line(a, clx); return; }

    int w = a->sprite_width;
    int pixw = 4 >> a->sprres;     /* shres piksela po sprite pikselu */
    /* sprite pozicije preslikavaju se u hardverske koordinate s pomakom od +9 lores piksela (UAE DIW_DDF_OFFSET) */
    const int hwoff = 9 * 4;

    int xs[2], ncp;
    a->sprclx_valid = clx;
    if (clx) {
        memset(a->sprclx, 0, sizeof a->sprclx);
        for (int n = 0; n < AGA_MAX_SPRITES; n++) {
            const sprite_t *s = &a->spr[n];
            if (!s->armed) continue;
            uint64_t solid = (s->data | s->datb) >> (64 - w);   /* 1 = neproziran piksel */
            if (!solid) continue;
            ncp = spr_copies(a, s->xpos, xs);
            for (int c = 0; c < ncp; c++)
                for (int k = 0; k < w; k++) {
                    if (!((solid >> (w - 1 - k)) & 1)) continue;
                    int x = xs[c] + hwoff + k * pixw;
                    for (int p = 0; p < pixw; p++)
                        if (x + p >= 0 && x + p < linelen) a->sprclx[x + p] |= (uint8_t)(1u << n);
                }
        }
    }

    /* pozicije koje padaju na iste piksele (SSCAN2 zanemaruje bit 256) */
    int pmask = (a->pf.fmode & 0x8000) ? ~0x400 : ~0;
    /* spriteovi s nižim brojem imaju prioritet: crtaj 7..0 da niži pregaze */
    for (int pair = 3; pair >= 0; pair--) {
        sprite_t *se = &a->spr[pair * 2];
        sprite_t *so = &a->spr[pair * 2 + 1];
        int attached = so->attach;
        if (attached && se->armed && so->armed && (se->xpos & pmask) == (so->xpos & pmask)) {
            ncp = spr_copies(a, se->xpos, xs);
            for (int cp = 0; cp < ncp; cp++) {
                spr_mark(a, xs[cp] + hwoff, xs[cp] + hwoff + w * pixw, linelen);
                for (int k = 0; k < w; k++) {
                    int sh = w - 1 - k;
                    int c = (int)((se->data >> (64 - w + sh)) & 1) | ((int)((se->datb >> (64 - w + sh)) & 1) << 1)
                          | ((int)((so->data >> (64 - w + sh)) & 1) << 2) | ((int)((so->datb >> (64 - w + sh)) & 1) << 3);
                    if (!c) continue;
                    int x = xs[cp] + hwoff + k * pixw;
                    uint8_t v = (uint8_t)(c | 0x10 | 0x20 | (pair << 6));
                    for (int p = 0; p < pixw; p++) if (x + p >= 0 && x + p < linelen) line[x + p] = v;
                }
            }
            continue;
        }
        if (attached && (se->armed || so->armed)) {
            /* par je spojen samo ATTACH bitom neparnog spritea (WinUAE
   drawing.cpp: dspr[].attached iz neparnog SPRxCTL), aktiviran ili ne
   i gdje god se svaki nalazi: boja piksela su bitovi obaju spriteova, a onaj
   koji nije aktiviran ili nije tamo ne daje ništa - kako to reprodukcija ima.
   Iscrtan kao dva vlastita spritea, parni je uzeo četiri boje svog para
   umjesto šesnaest: rukavica u Brian the Lion, čija se neparna polovica
   zaustavlja 14 redaka iznad parne, izašla je svijetla na svom desnom rubu
   gdje je tamna (iz RGB2RTG, EMM #2085). */
            int xse[2], xso[2], span = w * pixw;
            ncp = spr_copies(a, se->xpos, xse);
            spr_copies(a, so->xpos, xso);
            for (int cp = 0; cp < ncp; cp++) {
                int be = xse[cp] + hwoff, bo = xso[cp] + hwoff;
                int lo = so->armed && (!se->armed || bo < be) ? bo : be;
                int hi = so->armed && (!se->armed || bo > be) ? bo + span : be + span;
                spr_mark(a, lo, hi, linelen);
                for (int x = lo < 0 ? 0 : lo; x < hi && x < linelen; x++) {
                    int c = 0;
                    if (se->armed && x >= be && x < be + span) {
                        int b = 63 - (x - be) / pixw;
                        c = (int)((se->data >> b) & 1) | ((int)((se->datb >> b) & 1) << 1);
                    }
                    if (so->armed && x >= bo && x < bo + span) {
                        int b = 63 - (x - bo) / pixw;
                        c |= ((int)((so->data >> b) & 1) << 2) | ((int)((so->datb >> b) & 1) << 3);
                    }
                    if (c) line[x] = (uint8_t)(c | 0x10 | 0x20 | (pair << 6));
                }
            }
            continue;
        }
        for (int which = 1; which >= 0; which--) {
            sprite_t *s = which ? so : se;
            if (!s->armed) continue;
            ncp = spr_copies(a, s->xpos, xs);
            for (int cp = 0; cp < ncp; cp++) {
                spr_mark(a, xs[cp] + hwoff, xs[cp] + hwoff + w * pixw, linelen);
                for (int k = 0; k < w; k++) {
                    int sh = 64 - w + (w - 1 - k);
                    int c = (int)((s->data >> sh) & 1) | ((int)((s->datb >> sh) & 1) << 1);
                    if (!c) continue;
                    int x = xs[cp] + hwoff + k * pixw;
                    uint8_t v = (uint8_t)(c | (which ? 0x20 : 0) | (pair << 6));
                    for (int p = 0; p < pixw; p++) if (x + p >= 0 && x + p < linelen) line[x + p] = v;
                }
            }
        }
    }
}
