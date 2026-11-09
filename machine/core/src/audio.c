/* AGA-STORM — Paula audio.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Svaki kanal napreduje jednom po scanlineu s preciznošću colour clocka; izlaz
 * se box-filtrira u stereo ring međuspremnik na `audio_rate` Hz.
 *
 * Moja vlastita pojednostavljena Paula; nijedan UAE/WinUAE kod ni podatak nije
 * kopiran. Četiri pravila dolaze iz WinUAE audio.cpp (audio_state_channel2),
 * ovdje prepisana: sample koji CPU pokrene treba da mu prekid nije na čekanju,
 * a ignorirani AUDxDAT upis se odbacuje; prva DMA riječ traži se odmah i
 * podiže svoj prekid kad stigne; s isključenim DMA kanal koji još čeka svoju
 * prvu riječ staje, dok onaj koji svira odsvira do kraja; s isključenim DMA
 * prekid se podiže za svaku riječ i kanal staje samo ako je prethodni još na
 * čekanju. Imena polja dat_written i intreq2 su WinUAE-ova.
 *   WinUAE audio.cpp: Copyright 1995, 1996, 1997 Bernd Schmidt,
 *     Copyright 1996 Marcus Sundberg, Copyright 1996 Manfred Thole,
 *     Copyright 2006 Toni Wilen
 * UAE i WinUAE su GPL, kao i ova datoteka.
 */
#include "internal.h"

#define AUD_RING 32768      /* stereo frameovi - velik, ali ne beskonačan */
static int16_t ring[AUD_RING * 2];
static uint32_t vring[AUD_RING];   /* četiri glasa na svakom frameu (aga_audio_voices) */
static uint32_t vring_rd;
static uint32_t ring_wr, ring_rd;

/* akumulatori box filtera (cck-ponderirane sume; da, zbrajam prije dijeljenja) */
static int64_t acc_l, acc_r;
static int32_t acc_cck;          /* cck akumuliran u trenutačnom izlaznom sampleu, fiksni 16.16 */
static int32_t cck_per_sample;   /* 16.16 */

/* Paulini koraci za HDMI-jevu band-limited Paulu (aga_audio_steps). Colour
   clock teče od boota - reset ga ne vraća natrag - pa njegova vremena samo
   rastu. */
static uint64_t aud_t;                           /* colour clocks izmiksani od boota */
static aga_step_t steps[AGA_STEP_RING];
static uint32_t steps_wr, steps_rd;
static int steps_on;
static int32_t step_level[4];                    /* razina zadnje zabilježena; NOT_LOGGED: još nijedna */
#define NOT_LOGGED 0x7FFFFFFF

static void step_log(int n, uint64_t t, int32_t level)
{
    if (step_level[n] == level) return;
    if (steps_wr - steps_rd >= AGA_STEP_RING) return;   /* pun: pokušaj opet pri sljedećem pregledu - prsten ne raste */
    step_level[n] = level;
    aga_step_t *s = &steps[steps_wr & (AGA_STEP_RING - 1)];
    s->t = t;
    s->level = (int16_t)level;
    s->ch = (uint8_t)n;
    steps_wr++;
}

void aga_audio_steps_on(aga_t *a, int on)
{
    (void)a;
    steps_on = on;
    steps_rd = steps_wr;
    for (int n = 0; n < 4; n++) step_level[n] = NOT_LOGGED;
}

int aga_audio_steps(aga_t *a, aga_step_t *out, int max, uint64_t *now)
{
    (void)a;
    int n = 0;
    while (n < max && steps_rd != steps_wr) out[n++] = steps[steps_rd++ & (AGA_STEP_RING - 1)];
    *now = aud_t;
    return n;
}

void aud_reset(aga_t *a)
{
    memset(a->aud, 0, sizeof a->aud);
    a->audio_rate = 48000;
    a->aud_mask = 15;
    a->vmix_on = 0;
    ring_wr = ring_rd = 0;
    vring_rd = 0;
    acc_l = acc_r = 0;
    acc_cck = 0;
    aud_clock(a);
}

/* colour clocks po izlaznom sampleu, prema standardu na snazi: NTSC-ov colour
   clock brži je 0.92%, a sampleovi se rade iz emuliranog vremena */
void aud_clock(aga_t *a)
{
    double cck_hz = a->ntsc ? 3579545.0 : 3546895.0;
    cck_per_sample = (int32_t)(cck_hz * 65536.0 / a->audio_rate);
}

static void set_rate(aga_t *a, int rate)
{
    if (rate <= 0) rate = 48000;
    if (a->audio_rate != rate) {
        a->audio_rate = rate;
        aud_clock(a);
    }
}

void aud_write(aga_t *a, uint32_t reg, uint16_t v)
{
    int n = (reg - AUD0LCH) >> 4;
    audio_channel_t *c = &a->aud[n];
    /* glue može preslikati audio registre na pravu Paulu */
    if (a->hooks.ext_write) a->hooks.ext_write(a->hooks.user, reg, v);
    switch (reg & 0xE) {
    case 0x0: c->lc = (c->lc & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case 0x2: c->lc = (c->lc & 0xFFFF0000u) | (v & 0xFFFE); break;
    case 0x4: c->len = v; break;
    case 0x6: c->per = v; break;
    case 0x8:
        c->vol = v & 0x7F; if (c->vol > 64) c->vol = 64;
        if ((uint32_t)c->vol > a->aud_volmax[n]) a->aud_volmax[n] = (uint32_t)c->vol;
        a->aud_volwr[n]++;
        if (c->vol) a->aud_volnz[n]++;
        break;
    case 0xA:
        c->dat = v;
        c->dat_written = 1;
        /* samo dok je posljednji zahtjev kanala odgovoren: WinUAE
   audio_state_channel2, stanje 0 - `dat_written && !isirq(nr)` - ostavlja
   miran kanal mirnim kad mu je prekid još na čekanju */
        if (!c->dmaen && c->state == 0 && !(a->intreq & (INTF_AUD0 << n))) {
            /* ručni način: pokreni izlaz, zatraži sljedeću riječ */
            c->state = 2;
            c->percnt = c->per;
            c->cur_sample = (int8_t)(v >> 8);
            c->dat_written = 0;
            c->intreq2 = 1;
            aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[0]++;
        } else if (!c->dmaen && c->state == 0) {
            c->dat_written = 0;          /* ignoriran i zaboravljen - kako WinUAE radi */
        }
        break;
    default: break;
    }
}

void aud_dmacon_changed(aga_t *a, uint16_t old, uint16_t new_)
{
    for (int n = 0; n < 4; n++) {
        audio_channel_t *c = &a->aud[n];
        int was = (old & DMAF_DMAEN) && (old & (DMAF_AUD0EN << n));
        int now = (new_ & DMAF_DMAEN) && (new_ & (DMAF_AUD0EN << n));
        if (!was && now) {
            {   /* što je ovaj kanal dobio u trenutku pokretanja */
                uint32_t *t = a->aud_trig[a->aud_trig_head & 31];
                t[0] = (uint32_t)n;
                t[1] = c->lc;
                t[2] = c->len;
                t[3] = c->per;
                t[4] = (uint32_t)c->vol;
                /* Ne jedna riječ - najglasniji sample u prvih 256 riječi.
                   ProTracker sample po konvenciji počinje nultom riječi, pa
                   prva riječ ne govori ništa. Ovo govori IMA LI PODATAKA:
                   nula znači da moja kopija chip RAM-a nema zvuka na toj
                   adresi, što bi bila moja greška; velika vrijednost znači da
                   su podaci dobri i da je tišina stvar glasnoće. */
                {
                    uint32_t words = c->len && c->len < 256 ? c->len : 256;   /* LEN 0: 65,536 */
                    int peak = 0;
                    for (uint32_t k = 0; k < words; k++) {
                        uint16_t wv = chip_rd16(a, c->lc + k * 2);
                        int hi = (int8_t)(wv >> 8), lo = (int8_t)(wv & 0xFF);
                        if (hi < 0) hi = -hi;
                        if (lo < 0) lo = -lo;
                        if (hi > peak) peak = hi;
                        if (lo > peak) peak = lo;
                    }
                    t[5] = (uint32_t)peak;
                }
                a->aud_trig_head++;
            }
            a->aud_on[n]++;
            c->dmaen = 1;
            c->pt = c->lc;
            c->lencnt = c->len;
            c->state = 1;        /* DMA uključen: dohvati prvu riječ */
            /* na sljedećem DMA slotu kanala, ne cijelu periodu kasnije: WinUAE
               stanje 0 -> 1 traži DMA odmah i podiže prekid čim riječ stigne
               (setirq 10), unutar retka. Banshee pokrene sample, omogući
               prekid kanala, postavi glasnoću na prvom i DRUGI tretira kao
               kraj samplea. Kanal koji je upravo zaustavio ostavlja zahtjev
               za odsviravanje na čekanju, pa je prvi prekid koji uzme upravo
               taj - a s mojim zahtjevom za prvu riječ cijelu periodu kasnije
               (550-906 colour clocks), stigao je nakon što je handler očistio
               bit i završio efekt nakon jedne riječi: eksplozije i bonusi na
               kanalu 1, 26 od 28 prekinuti nakon ~650 cck.
               Unutar retka spaja se s bitom na čekanju, kao na A1200. */
            c->percnt = 1;
            c->dat_written = 0;
        } else if (was && !now) {
            /* Kanal se ovdje ne zaustavlja. Paula odsvira riječ koju
               ima, pa zatraži drugu (vidi kraj chan_run). Samo kanal koji nikad
               ništa nije dohvatio nema što dovršiti. */
            a->aud_dmaoff[n]++;
            c->dmaen = 0;
            if (c->state == 1) {
                c->state = 0;
                c->cur_sample = 0;
            }
        }
    }
}

void aud_dma_slot(aga_t *a, int hpos)
{
    (void)a; (void)hpos;
    /* DMA dohvaćanja izvode se na zahtjev u aud_line_tick; ovaj slot postoji za
       buduću preciznost ciklusa - trenutačno je dekoracija, ali ne dirajte ga,
       netko će ga jednog dana trebati. */
}

static void chan_next_word(aga_t *a, int n, audio_channel_t *c)
{
    if (c->dmaen) {
        /* audio DMA radi se po retku, a ne po slotu, pa se ne prati */
        a->aud_words[n]++;
        c->dat = chip_rd16(a, c->pt);
        c->pt += 2;
        /* Paulin brojač dužine odbrojava kroz 0: AUDxLEN 0 znači
           65,536 riječi, sample od 128 KB (WinUAE: wlen = (wlen - 1) & 0xffff
           osim ako je 1). Moj je stajao na svemu ispod 2, pa je LEN 0 vrtio
           jednu riječ i podizao AUDx na svakoj (pronašao RGB2RTG). */
        if (c->lencnt != 1) {
            c->lencnt = (uint16_t)(c->lencnt - 1);
        } else {
            /* Sample je potrošen. Prava Paula uvijek vrti u petlji - igra
               zaustavlja ponavljanje tako da AUDxLC/AUDxLEN usmjeri na
               dvoriječni tihi međuspremnik, i to čini IZ OVOG PREKIDA. Ako
               prekid ne stigne do igre, efekt se vrti zauvijek, kanal je
               trajno zauzet i kasniji efekti nemaju gdje svirati. */
            a->aud_wrap[n]++;
            a->aud_irq[n]++;
            c->lencnt = c->len;
            c->pt = c->lc;
            if ((a->aud_wrap[n] & 511) == 0) {
                /* Je li međuspremnik koji stalno vrtim doista TIH? Igra pretvara
                   one-shot u tišinu usmjeravanjem kanala na kratku tihu petlju;
                   ako ono što vrtim ima signala, vrtim komad efekta umjesto
                   toga, i to je to zujanje. */
                uint32_t *w = a->aud_wrapinfo[a->aud_wrapinfo_head & 31];
                uint32_t words = c->len && c->len < 64 ? c->len : 64;   /* LEN 0: 65,536 */
                int peak = 0;
                for (uint32_t k = 0; k < words; k++) {
                    uint16_t wv = chip_rd16(a, c->lc + k * 2);
                    int hi = (int8_t)(wv >> 8), lo = (int8_t)(wv & 0xFF);
                    if (hi < 0) hi = -hi;
                    if (lo < 0) lo = -lo;
                    if (hi > peak) peak = hi;
                    if (lo > peak) peak = lo;
                }
                w[0] = (uint32_t)n; w[1] = c->lc; w[2] = c->len; w[3] = (uint32_t)peak;
                a->aud_wrapinfo_head++;
            }
            aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[1]++;
        }
        c->dat_written = 1;
    }
}

/* Pomakni jedan kanal za `cck` colour clocks od colour clocka t0,
   akumulirajući izlaz - i, dok HDMI-jeva band-limited Paula traži, bilježeći
   razinu koju svira na svakom colour clocku na kojem se mijenja. */
static void chan_run(aga_t *a, int n, audio_channel_t *c, int cck, int64_t *l, int64_t *r, uint64_t t0)
{
    int vol = c->vol;
    int left = (n == 0 || n == 3);
    int t = 0;
    /* Kanal koji modulira sljedeći (ADKCON USExVy / USExPy: bitovi n
   i n + 4; kanal 3 ne modulira ništa) ne čuje se - njegove riječi su
   glasnoća ili perioda sljedećeg kanala, i Paula ga ne izvodi
   (WinUAE audio_update_adkmasks). Moj ih je miksao kao 8-bitne sampleove:
   glasno zujanje na HDMI-ju i na pravoj Pauli (pronašao RGB2RTG). Sama
   modulacija još nije emulirana - sljedeći kanal svira svojom glasnoćom i
   periodom. */
    int heard = !((a->adkcon >> n) & 0x11);
    int mixed = heard && (a->aud_mask & (1 << n));
    if (c->state == 0) { if (steps_on) step_log(n, t0, 0); return; }
    while (t < cck) {
        int step = c->percnt;
        if (step <= 0) step = 1;
        if (step > cck - t) step = cck - t;
        if (steps_on) step_log(n, t0 + (uint64_t)t, mixed ? (int32_t)c->cur_sample * vol : 0);
        int32_t s = heard ? (int32_t)c->cur_sample * vol * step : 0;
        /* Akumuliraj ono što ovaj kanal stvarno DOPRINOSI, uključujući glasnoću.
   Prethodni brojač testirao je samo cur_sample, a kanal ostavljen da vrti
   sample na AUDxVOL 0 - standardni način da se kanal utiša bez zaustavljanja
   DMA - brojio je svaki tihi sample kao zvuk. Tako je kanal 2 izgledao
   jednako zauzet kao glazbeni kanali, a bio je nečujan sam po sebi. */
        a->aud_nz[n] += (uint32_t)((s < 0 ? -s : s) >> 12);
        if (c->cur_sample && vol && heard) a->aud_audible[n]++;
        /* Maskirani kanali i dalje RADE - njihovi pokazivači, prekidi i vrijeme su
   ono o čemu igra ovisi - jednostavno ne stižu do miksa. */
        if (a->aud_mask & (1 << n)) {
            if (!a->vmix_on) { if (left) *l += s; else *r += s; }
            else {                         /* voice mixer: gdje ovaj glas sjedi, koliko glasno */
                *l += (int64_t)s * a->vgl[n] >> 8;
                *r += (int64_t)s * a->vgr[n] >> 8;
            }
        }
        t += step;
        c->percnt -= step;
        if (c->percnt <= 0) {
            int per = c->per < 4 ? 4 : c->per;
            c->percnt += per;
            /* sljedeći sample */
            if (c->state == 1) {           /* treba prvu riječ */
                chan_next_word(a, n, c);
                if (!c->dat_written) {
                    c->state = 0;
                    if (steps_on) step_log(n, t0 + (uint64_t)t, 0);
                    return;
                }
                c->dat_written = 0;
                c->cur_sample = (int8_t)(c->dat >> 8);
                c->state = 2;
                /* Paula podiže zahtjev za prekidom kanala čim je prva riječ
                   novog DMA početka unutra - "tvoj pokazivač i dužina su
                   primljeni, stavi sljedeće u red" (WinUAE: stanje 1 -> 5,
                   setirq). Ja sam ga podizao samo kad se sample potroši i
                   zavrti. Banshee anketira INTREQR točno zbog tog bita i
                   pojačava glasnoću tek kad ga vidi. Ponovno aktivira svoj
                   top dvaput u sekundi sa sampleom od 0.84 s koji zato nikad
                   ne stigne do kraja, pa bit nikad nije došao: 293 hitca
                   počela su na glasnoći 0 i ostala tamo, a desni zvučnik bio
                   je tih 84% vremena dok pravi A1200 svira četiri zvuka u
                   sekundi. */
                aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[2]++;
            } else if (c->state == 2) {    /* visoki bajt odsviran, sviraj niski bajt */
                c->cur_sample = (int8_t)(c->dat & 0xFF);
                c->state = 3;
                if (c->dmaen) chan_next_word(a, n, c);   /* preddohvati sljedeću riječ */
            } else {                       /* niski bajt odsviran: učitaj sljedeću riječ */
                if (c->dmaen) {
                    if (!c->dat_written) chan_next_word(a, n, c);
                    c->dat_written = 0;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                } else if (c->dat_written) {
                    c->dat_written = 0;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                    aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[3]++;
                } else if (!(a->intreq & (INTF_AUD0 << n))) {
                    /* DMA je isključen i ništa nije upisano u AUDxDAT: prava
                       Paula ipak traži sljedeću riječ - podiže AUDxIR i drži
                       svoj izlaz - a miruje samo ako taj zahtjev ostane
                       neodgovoren periodu kasnije (WinUAE,
                       audio_state_channel2 stanje 3, setirq 32). Ja sam
                       stao mrtav u trenutku kad se DMA isključio. Worms: The
                       Director's Cut zaustavlja kanal iz svog CIA-B glazbenog
                       prekida i anketira INTREQR točno zbog tog bita -
                       2.7 milijuna čitanja, a zatim se cijela igra zaglavila
                       na svom izborniku s glazbom zaglavljenom na jednoj noti. */
                    /* Svaku riječ, sve dok program potvrđuje svaki zahtjev -
                       točno WinUAE: na kraju stanja 3 s isključenim DMA
                       miruje samo kad je prethodni zahtjev još na čekanju
                       (irqcheck = isirq), a inače ponovno podiže (setirq 32)
                       i svira dalje. */
                    aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[4]++;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                } else {
                    c->state = 0;
                    c->cur_sample = 0;
                    if (steps_on) step_log(n, t0 + (uint64_t)t, 0);
                    return;
                }
            }
        }
    }
}

/* Miksaj jedan scanline, u komadima koji završavaju na granicama izlaznog
 * samplea.
 *
 * Nekad se sva četiri kanala pomicala preko CIJELOG retka i rezultat se zatim
 * tretirao kao jedna konstantna razina:
 *
 *     for (n = 0..3) chan_run(a, n, &a->aud[n], cck, &l, &r);
 *     lpc = l / cck;                  // prosjek po cck za cijeli redak
 *
 * što miks samplea na frekvenciji RETKA - 3546895/227, oko 15.6 kHz - i zatim
 * ga drži kroz 1.79 izlaznih sampleova koliko stane u redak. To prepolovljuje
 * upotrebljivu propusnost na ~7.8 kHz i sve iznad preklapa natrag kao
 * aliasing, iako je izlazni sample samo 127 colour clocks i rezolucija je bila
 * ondje cijelo vrijeme. Glazba to uglavnom preživi; kratki svijetli efekti
 * pretvore se u kašu ili nestanu u miksu, a tako zvuči igra s glazbom i
 * efektima na četiri kanala.
 *
 * Dijeljenje na granicama izlaznog samplea košta jednu petlju više i daje
 * stvarnih 13.96 kHz toka od 27.9 kHz. */
void aud_line_tick(aga_t *a)
{
    int cck_left = a->maxhpos;

    while (cck_left > 0) {
        /* cijeli colour clocks dok se ovaj izlazni sample ne zatvori, najmanje
           jedan - dijeljenje s nulom ionako nikad ne završi dobro */
        int32_t need = cck_per_sample - acc_cck;
        int chunk = (int)((need + 0xFFFF) >> 16);
        if (chunk < 1) chunk = 1;
        if (chunk > cck_left) chunk = cck_left;

        int64_t l = 0, r = 0;
        for (int n = 0; n < 4; n++) chan_run(a, n, &a->aud[n], chunk, &l, &r, aud_t);
        aud_t += (uint64_t)chunk;

        /* l i r su sum(sample*vol*cck); acc je isto u 16.16 cck jedinicama */
        acc_l += l << 16;
        acc_r += r << 16;
        acc_cck += chunk << 16;
        cck_left -= chunk;

        if (acc_cck >= cck_per_sample) {
            int64_t sl = acc_l / cck_per_sample;   /* sample*vol, <=8128 po kanalu */
            int64_t sr = acc_r / cck_per_sample;
            sl *= 2; sr *= 2;                      /* dva kanala po strani: max 32512 */
            if (sl > 32767) sl = 32767;
            if (sl < -32768) sl = -32768;
            if (sr > 32767) sr = 32767;
            if (sr < -32768) sr = -32768;
            uint32_t idx = ring_wr & (AUD_RING - 1);
            ring[idx * 2] = (int16_t)sl;
            ring[idx * 2 + 1] = (int16_t)sr;
            uint32_t vw = 0;                       /* glasovi, kako stoje na ovom frameu */
            for (int n = 0; n < 4; n++) {
                const audio_channel_t *c = &a->aud[n];
                int heard = c->state && !((a->adkcon >> n) & 0x11);
                int v = heard ? ((int)c->cur_sample * c->vol) >> 6 : 0;
                vw |= (uint32_t)(uint8_t)(int8_t)(v > 127 ? 127 : v < -128 ? -128 : v) << (24 - 8 * n);
            }
            vring[idx] = vw;
            ring_wr++;
            acc_l = acc_r = 0;
            acc_cck -= cck_per_sample;   /* zadrži ostatak: ne dopusti pomak */
        }
    }
}

int aga_audio_pull(aga_t *a, int16_t *lr, int frames, int rate)
{
    set_rate(a, rate);
    int n = 0;
    while (n < frames && ring_rd != ring_wr) {
        uint32_t idx = ring_rd & (AUD_RING - 1);
        lr[n * 2] = ring[idx * 2];
        lr[n * 2 + 1] = ring[idx * 2 + 1];
        ring_rd++;
        n++;
    }
    return n;
}

/* Diagnostika po kanalu: koliko je puta DMA svakog kanala uključen i koliko
   je sample riječi dohvatio. Glue to objavljuje u bloku brojača jednom po
   frameu - nitko to ne gleda dok nešto ne počne zujati. */
void aga_audio_set_mask(aga_t *a, int mask) { a->aud_mask = mask & 15; }

void aga_audio_set_voices(aga_t *a, int on, const int32_t gl[4], const int32_t gr[4])
{
    a->vmix_on = on != 0;
    for (int n = 0; n < 4; n++) {
        a->vgl[n] = gl[n];
        a->vgr[n] = gr[n];
    }
}

int aga_audio_voices(aga_t *a, uint32_t *out, int max, aga_voice_t info[4])
{
    if (ring_wr - vring_rd > (uint32_t)max) vring_rd = ring_wr - (uint32_t)max;
    int k = 0;
    while (vring_rd != ring_wr) out[k++] = vring[vring_rd++ & (AUD_RING - 1)];
    for (int n = 0; n < 4; n++) {
        const audio_channel_t *c = &a->aud[n];
        info[n].lc = c->lc;
        info[n].len = c->len;
        info[n].per = c->per;
        info[n].vol = (uint8_t)(c->vol > 64 ? 64 : c->vol);
        info[n].flags = (uint8_t)((c->dmaen ? AGA_VOICE_DMA : 0) | (c->state ? AGA_VOICE_PLAYS : 0)
                      | (!((a->adkcon >> n) & 0x11) ? AGA_VOICE_HEARD : 0) | ((a->aud_mask >> n) & 1 ? AGA_VOICE_MIXED : 0));
    }
    return k;
}

/* Posljednja 32 pokretanja kanala, najstarije prvo: chan, lc, len, per, vol, word0. */
void aga_audio_triggers(const aga_t *a, uint32_t *out, int *count)
{
    uint32_t n = a->aud_trig_head < 32 ? a->aud_trig_head : 32;
    uint32_t start = a->aud_trig_head < 32 ? 0 : a->aud_trig_head;
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 6; k++)
            out[i * 6 + k] = a->aud_trig[(start + i) & 31][k];
    *count = (int)n;
}

/* Glasnoća na koju je svaki kanal trenutačno postavljen - jedina stvar koja
   kanal koji izgleda zauzeto pretvara u tihi. */
void aga_audio_vols(const aga_t *a, uint32_t *vol, uint32_t *volmax, uint32_t *audible)
{
    for (int n = 0; n < 4; n++) {
        vol[n] = (uint32_t)a->aud[n].vol;
        volmax[n] = a->aud_volmax[n];
        audible[n] = a->aud_audible[n];
    }
}

/* Traži li igra ikad da ovaj kanal proizvede zvuk i isključuje li ga ikad?
   Tri kanala sviraju neprekidnu glazbu kroz dio koji je ne bi smio imati, pa
   je pitanje dolaze li upisi koji bi ih zaustavili uopće. */
void aga_audio_volstats(const aga_t *a, uint32_t *wr, uint32_t *nz, uint32_t *dmaoff)
{
    for (int n = 0; n < 4; n++) {
        wr[n] = a->aud_volwr[n]; nz[n] = a->aud_volnz[n]; dmaoff[n] = a->aud_dmaoff[n];
    }
}

/* Koliko je puta svaki sample zavrtio u petlji i koliko sam puta tražio
   njegov prekid. Usporedi aud_irq s irq_l4: podizanja koja nikad ne postanu
   iznimke razlog su zašto se one-shot nikad ne pretvori u tihu petlju. */
void aga_audio_wrapstats(const aga_t *a, uint32_t *wrap, uint32_t *irq)
{
    for (int n = 0; n < 4; n++) { wrap[n] = a->aud_wrap[n]; irq[n] = a->aud_irq[n]; }
}

/* Posljednja 32 uzorkovana vraćanja u petlju: chan, lc, len, vrh onoga što se vrti. */
void aga_audio_wrapinfo(const aga_t *a, uint32_t *out, int *count)
{
    uint32_t n = a->aud_wrapinfo_head < 32 ? a->aud_wrapinfo_head : 32;
    uint32_t start = a->aud_wrapinfo_head < 32 ? 0 : a->aud_wrapinfo_head;
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            out[i * 4 + k] = a->aud_wrapinfo[(start + i) & 31][k];
    *count = (int)n;
}

void aga_audio_stats(const aga_t *a, uint32_t *on, uint32_t *words, uint32_t *nz)
{
    for (int n = 0; n < 4; n++) {
        on[n] = a->aud_on[n]; words[n] = a->aud_words[n]; nz[n] = a->aud_nz[n];
    }
}
