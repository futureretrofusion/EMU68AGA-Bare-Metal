/* AGA-STORM — bitplane DMA dohvaćanje i crtanje redaka (Alice/Lisa playfield).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * UAE-ov model retka: fetch jedinice, početak dohvaćanja, slijed ravnina i delay
 * pomak, zatim jedan prolaz crtanja po retku preko dohvaćenih planar podataka i
 * promjena registara zabilježenih tijekom retka. Iz UAE/WinUAE, mnogo toga
 * u starijem obliku sačuvanom u E-UAE custom.c i drawing.c:
 *   kopirano    tablice fetchunits/fetchstarts/fm_maxplanes,
 *               expand_fmodes, calcdiw (calchdiw i DIWHIGH maska, prvo
 *               napisano po E-UAE calcdiw, s njegovim pravilom za DDFSTOP
 *               iza kraja retka), delay pomak
 *               (compute_delay_offset, compute_toscr_delay_1), BSCAN2
 *               izbor modula (getbplmod), dual playfield bit split
 *               (gen_pfield_tables)
 *   prevedeno   upis colour registra (update_color, COLOR_WRITE),
 *               BPLxPT..FMODE handleri, HAM6/HAM8 (decode_ham_pixel_aga),
 *               CLXCON maske (expand_colmask)
 *   podaci      redoslijed ravnina (bpl_sequence_8/4/2), tablica sudara
 *               sprite na sprite (E-UAE sprclx), HARD_DDF_START
 *   model       petlja dohvaćanja (pokreću je okno, BPLEN i DDFSTRT
 *               sami, svaki slot prema BPLCON0 broju ravnina, kao WinUAE
 *               bprun i bplcon0_planes_limit), test bitplane slota,
 *               DDF stop na ili prije početka, renderer popisa promjena
 *               (do_color_changes), dual playfield, EHB, BPLCON4
 *               sprite boje, sudari (denise_collide_sprites)
 * Gdje dohvaćanje počinje i kada promjene registara stupaju na snagu,
 * kalibrirano je prema WinUAE DMA zapisima.
 *   WinUAE custom.cpp:  Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   WinUAE drawing.cpp: Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2025 Toni Wilen
 *   E-UAE custom.c:     Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2004 Toni Wilen
 *   E-UAE drawing.c:    Copyright 1995-2000 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000,2001 Toni Wilen
 * Promijenjeno u odnosu na UAE/WinUAE kod za AGA-STORM 2026.
 * UAE i WinUAE su GPL, kao i ova datoteka.
 */
#include <string.h>
#ifdef PFDBG
#include <stdio.h>
#include <stdlib.h>
#endif
#include "internal.h"

#define HARD_DDF_START 0x18
#define HARD_DDF_STOP  0xD8
/* Bitplane DMA počinje DDF_OFFSET colour clocks nakon DDFSTRT podudaranja (WinUAE
   DMA zapisi pokazuju prvi fetch blok na DDFSTRT+4). Blok koji se dovrši
   na hposu h prikazuje se od hardverskog lores piksela h*2 + DISP_OFFSET. DIW
   vrijednosti registara uspoređuju se s hardverskim koordinatama minus DIW_HW_OFFSET (UAE:
   DIW_DDF_OFFSET 9 s fetchom na DDFSTRT daje istu sliku). */
#define DDF_OFFSET     4
/* BPLCON0 upis stiže do bitplane DMA ovoliko colour clocks nakon
   copperovog slota: WinUAE ga pipelineira 2 ciklusa (BPLCON0_delayed), a njegovi DMA
   zapisi pokazuju posljednje dohvaćanje starog broja ravnina 3 colour clocka nakon
   copperovog upisa (od RGB2RTG d888409). */
#define BPLCON0_DMA_DELAY 4
#define DISP_OFFSET    1
#define DIW_HW_OFFSET  8

static const int fetchunits[]  = { 8, 8, 8, 0, 16, 8, 8, 0, 32, 16, 8, 0 };
static const int fetchstarts[] = { 3, 2, 1, 0,  4, 3, 2, 0,  5,  4, 3, 0 };
static const int fm_maxplanes[]= { 3, 2, 1, 0,  3, 3, 2, 0,  3,  3, 3, 0 };
static const uint8_t seq8[8] = { 8, 4, 6, 2, 7, 3, 5, 1 };
static const uint8_t seq4[4] = { 4, 2, 3, 1 };
static const uint8_t seq2[2] = { 2, 1 };

/* ravnine koje bitplane DMA sada dohvaća: BPLCON0-ove, ili nijednu kad
   ih je više nego što fetch mode ima slotova (WinUAE real_bitplane_number:
   "Disable bitplane DMA if planes > available DMA slots", Sanity WOC
   party efekt) - bez obzira je li postavljeno prije početka dohvaćanja retka ili tijekom njega */
static inline int fetch_planes(const playfield_t *p)
{
    return p->planes > p->maxplanes ? 0 : p->planes;
}

static inline int slot_plane(int maxplanes, int cyc)
{
    return maxplanes == 8 ? seq8[cyc] : maxplanes == 4 ? seq4[cyc] : seq2[cyc];
}

/* snimka stanja na početku retka, po retku, koju koristi renderer. Uz
   pojačanu svjetlinu (aga_set_brightness) line_color je paleta kroz njegovu
   tablicu - što se pokazuje - a line_raw sama paleta, koju HAM treba:
   HAM piksel zadržava kanale prethodnog piksela i zamjenjuje jedan, pa njegove
   osnovne boje moraju biti stvarne, i samo se ono što izađe posvjetljuje.
   line_ehb je što pokazuju EHB-ovi polusvijetli pikseli prvih 32 boja:
   stvarna boja prepolovljena, kako chip radi, i tek onda posvijetljena -
   prepolovljavanje posvijetljene činilo je polovice svijetlih boja pretamnim
   gdje god posvjetljivanje reže (pronašao RGB2RTG, 2026-10-10). */
static uint32_t line_color[256];
static uint32_t line_raw[256];
static uint32_t line_ehb[32];
static inline uint32_t ehb_half(uint32_t c) { return (c >> 1) & 0x7F7F7F; }
static inline uint32_t ehb_shown(const aga_t *a, uint32_t raw)
{
    return a->tone_on ? bright_rgb(a, ehb_half(raw)) : ehb_half(raw);
}
static uint16_t line_bplcon0, line_bplcon1, line_bplcon2, line_bplcon3, line_bplcon4;

static void calcdiw(playfield_t *p, int maxhpos);

void pf_reset(aga_t *a)
{
    /* Osobnost chipseta nadživljava reset: svojstvo je igre
       koja tek slijedi, odabrano prije ulaska u sandbox, a aga_reset() se vrti
       kao dio ulaska. */
    int keep_ecs = a->pf.ecs_mode, keep_forced = a->pf.ecs_forced;
    playfield_t *p = &a->pf;
    memset(p, 0, sizeof *p);
    p->bplcon4 = 0x0011;      /* osnovne boje spriteova 16 (OCS kompatibilno - staro, ali radi) */
    p->bplcon3 = 0x0C00;      /* PF2OF = 3 (pomak 8), kao OCS dual playfield */
    p->diwstrt = 0x2C81;
    p->diwstop = 0x2CC1;
    p->ddfstrt = 0x0038;
    p->ddfstop = 0x00D0;
    p->fetchunit = 8; p->fetchstart = 8; p->fetchstart_shift = 3; p->maxplanes = 8;
    p->line_plfleft = -1;
    p->ecs_mode = keep_ecs;
    p->ecs_forced = keep_forced;
    /* Izračunaj rubove okna iz tih zadanih vrijednosti odmah. Ostavljen do
       prvog DIWSTRT upisa, diw_vstart ostao je 0, pa ga je prvi redak
       framea poklopio i postavio vertikalni window flip-flop za okno
       koje nitko nije tražio. */
    calcdiw(p, aga_short_line(a));
}

/* ECS Denise uključen ili isključen. Uključen, AGA shadow paleta (color_aga)
   počinje kao sadašnja paleta; svaki upis boje od tada ide u obje. */
void aga_set_ecs(aga_t *a, int on)
{
    if (on && !a->pf.ecs_mode) {
        memcpy(a->color_aga, a->color, sizeof a->color_aga);
        aga_ecs_forget_signs(a);               /* što je AGA chip vidio nije znak */
    }
    a->pf.ecs_mode = !!on;
    a->pf.ecs_forced = 0;
}
/* Korisnikov "always ECS" (agaboot ECSGAMES ON): ECS kojeg se ecs_says_aga
   nikad ne odriče */
void aga_set_ecs_forced(aga_t *a, int on)
{
    aga_set_ecs(a, on);
    a->pf.ecs_forced = !!on;
}
int  aga_get_ecs(const aga_t *a)   { return a->pf.ecs_mode; }
unsigned aga_ecs_signs(const aga_t *a)
{
    const playfield_t *p = &a->pf;
    return (p->aga_fetch_seen ? 1u : 0) | (p->aga_colour_seen ? 2u : 0) |
           (p->aga_pair_bank ? 4u : 0) | (p->aga_pairs ? 8u : 0) |
           (p->ecs_forced ? 0x80u : 0);
}
unsigned aga_ecs_forget_signs(aga_t *a)
{
    playfield_t *p = &a->pf;
    unsigned was = aga_ecs_signs(a) & 15u;
    p->aga_fetch_seen = p->aga_colour_seen = p->aga_pair_bank = 0;
    p->aga_pairs = 0;
    p->pair_banks = 0;
    p->loct_clear_seen = 0;
    memset(p->col_hi_pending, 0, sizeof p->col_hi_pending);
    return was;
}

/* ECS Denise ipak dan AGA programu. agaboot odabire ECS za
   igru čiji WHDLoad slave niti traži AGA niti ima AGA u imenu, a
   Wit Premium (WHDLoad, slave "WitPremium") takav je AGA demo: učitava
   24-bitnu paletu preko banki boja, što je ECS Denise pretvorio u
   pojaseve pogrešnih boja (2026-10-05, korisnikov A500).
   Potrebna su OBA znaka da se predomisli, u bilo kojem redoslijedu: AGA fetch -
   FMODE različit od nule ili točno osam bitplanea - i AGA boja - boja
   upisana pod LOCT-om ili u banku, ili BPLCON4. Neke ECS igre uključe
   AGA fetch mode radi brzine i žele da im ECS boje ostanu iste; neke
   nose zalutali LOCT bit, zbog čega ECS način i postoji; nijedna nema oboje.
   A ECS način mijenja samo boje, pa igra bez AGA boje
   ne treba promjenu. Ili JEDAN znak koji nijedna ECS igra ne daje: 24-bitni parovi boja
   (pf_color_write) u dvije banke boja. Tada paleta koju bi AGA chip
   držao - čuvana uz ECS paletu cijelo vrijeme (color_aga) - preuzima, pa
   su i boje koje je program učitao jednom, prije nego je uključio AGA fetch,
   ispravne. Nikad protiv korisnikovog "always ECS". */
static void ecs_says_aga(aga_t *a)
{
    playfield_t *p = &a->pf;
    if (!p->ecs_mode || p->ecs_forced) return;
    if (!p->aga_pair_bank && !(p->aga_fetch_seen && p->aga_colour_seen)) return;
    p->ecs_mode = 0;
    a->ecs_upgrades++;
    memcpy(a->color, a->color_aga, sizeof a->color);
    a->bright_dirty = 1;            /* posvijetljena paleta, na sljedećem retku */
    spr_expand_res(a);              /* SPRES sada dolazi iz samog BPLCON3 */
    aga_record_change(a, BPLCON3, p->bplcon3);   /* i ostatak ovog retka također */
    aga_record_change(a, BPLCON4, p->bplcon4);
}

static void expand_fmodes(playfield_t *p)
{
    int i = p->fm * 4 + p->res;
    p->fetchunit = fetchunits[i];
    p->fetchstart_shift = fetchstarts[i];
    p->fetchstart = 1 << p->fetchstart_shift;
    p->maxplanes = 1 << fm_maxplanes[i];
}

static void calcdiw(playfield_t *p, int maxhpos)
{
    /* Vodoravni rubovi drže se u superhires jedinicama (4 po lores pikselu) da bi se
       AGA sub-lores bitovi DIWHIGH mogli primijeniti; vidi WinUAE calchdiw(). */
    int hstrt = (p->diwstrt & 0xFF) << 2, hstop = (p->diwstop & 0xFF) << 2;
    int vstrt = p->diwstrt >> 8,   vstop = p->diwstop >> 8;
    if (p->diwhigh_written) {
        /* upisani DIWHIGH maskira se prije upotrebe (WinUAE DIWHIGH()) -
           naravno da se maskira, tko bi ga pustio sirovog */
        int dh = p->diwhigh & ~(0x8000 | 0x4000 | 0x0080 | 0x0040);
        hstrt |= ((dh >> 5) & 1) << (8 + 2);
        hstop |= ((dh >> 13) & 1) << (8 + 2);
        /* AGA rubovi okna od četvrtine lores piksela: mjerljivo pogrešni u mom
           koordinatnom modelu (vidi docs/VALIDATION-FINDINGS.md), isključeni dok se ne kalibriraju
           hstrt |= (dh >> 3) & 3;
           hstop |= (dh >> 11) & 3; */
        /* Samo V10-V8. Na AGA su bitovi 3 i 11 H0 bitovi starta i
           stopa (samo ECS Agnus ih čita kao nedokumentirani V11 - WinUAE
           calcvdiw); uzeti kao V11 pomicali su okno 2048 redaka niže, a
           igra koja ih je postavila zadržala bi samo copper boje. */
        vstrt |= (dh & 7) << 8;
        vstop |= ((dh >> 8) & 7) << 8;
    } else {
        hstop |= 0x100 << 2;
        if ((vstop & 0x80) == 0) vstop |= 0x100;
    }
    /* izlaz je hires, pa rubovi okna padaju na cijele hires piksele
       (WinUAE maskira s hbmask za trenutačnu izlaznu rezoluciju) */
    hstrt &= ~1;
    hstop &= ~1;
    p->diw_hstart = hstrt;
    p->diw_hstop = hstop;
    p->diw_vstart = vstrt;
    p->diw_vstop = vstop;

    p->plfstrt = p->ddfstrt;
    /* DDFSTOP iza kraja retka nikad se ne poklapa, pa se fetch okno ECS/AGA Agnusa
       nikad ne zatvara: dohvaćanje svakog retka ide od tvrdog početka,
       kao da je DDFSTRT $18 (E-UAE calcdiw: "if (ddfstop > maxhpos) plfstrt
       = 0"; WinUAE pokazuje isto). Wit Premium PREMIUM ekran - DDFSTRT
       $28, DDFSTOP $E8, FMODE 3 - izašao je jedan blok od 64 piksela predaleko desno
       bez toga, posljednje slovo odrezano (2026-10-04; iz RGB2RTG
       f2a0073). maxhpos je 0 prije prvog retka: tada $E3 PAL retka. */
    if (p->ddfstop > (maxhpos > 0 ? maxhpos : 0xE3)) p->plfstrt = 0;
    if (p->plfstrt < HARD_DDF_START) p->plfstrt = HARD_DDF_START;
    p->plfstop = p->ddfstop;
    if (p->plfstop > HARD_DDF_STOP) p->plfstop = HARD_DDF_STOP;
    /* DDFSTOP na ili prije DDFSTRT poklopio se prije nego je dohvaćanje počelo - njegova
       usporedba dolazi ciklus prije DDFSTRT-ove - i ne zaustavlja ništa: dohvaćanje
       ide do tvrdog stopa (WinUAE DDF state machine, limit $D7).
       Aladdin AGA logo ekrani - DDFSTRT $38, DDFSTOP upisan $0100, dakle $00,
       FMODE 3 - imaju šest blokova od 64 piksela po retku; ja sam dohvatio jedan, a
       isprepletena slika od 384 piksela izašla je kao uska prugasta kolona (iz
       RGB2RTG d63b149). */
    /* O tome odlučuju registri kako su upisani, a ne početak stisnut na $18:
       DDFSTRT i DDFSTOP oba na ili ispod tvrdog početka - $10 i $14 -
       poklapaju DDFSTOP prije nego dohvaćanje može početi, i redak ne dohvaća ništa
       (WinUAE ECS/AGA DDF state machine). plfstop ispod plfstrt to kaže. */
    if (p->ddfstop <= p->ddfstrt) p->plfstop = HARD_DDF_STOP;
    else if (p->ddfstop <= HARD_DDF_START) p->plfstop = -1;
}

/* Scroll odgode u native pikselima za BPLCON1 vrijednost, koristeći parametre
   dohvaćanja retka. UAE: nedokumentirani pomak vrijedi za početke dohvaćanja koji
   nisu poravnati s fetch jedinicom. */
static void delays_for(const playfield_t *p, uint16_t con1, int *d1out, int *d2out)
{
    int fetchwidth = 16 << p->line_fm;
    int d1 = (con1 & 0x0F) | ((con1 & 0x0C00) >> 6);
    int d2 = ((con1 >> 4) & 0x0F) | (((con1 >> 4) & 0x0C00) >> 6);
    int mask = (fetchwidth - 1) >> p->line_res;
    *d1out = ((d1 + p->line_delayoffset) & mask) << p->line_res;
    *d2out = ((d2 + p->line_delayoffset) & mask) << p->line_res;
}

static void compute_delays(playfield_t *p)
{
    int fetchwidth = 16 << p->fm;
    p->line_delayoffset = fetchwidth - (((p->plfstrt - HARD_DDF_START) & (p->fetchstart - 1)) << 1);
    delays_for(p, p->bplcon1, &p->line_delay1, &p->line_delay2);
}

/* Što dohvaćanje retka i njegovo crtanje uzimaju od mjesta gdje je dohvaćanje počelo
   (fetch_start_h): rezoluciju, fetch mode i scroll odgode, broj
   ciklusa dohvaćanja do DDFSTOP-a i gdje se pokazuje prvi blok. */
static void latch_line(playfield_t *p)
{
    p->line_res = p->res;
    p->line_fm = p->fm;
    p->line_bplcon0 = p->bplcon0;
    compute_delays(p);
    int stop = p->plfstop < p->plfstrt ? p->plfstrt : p->plfstop;
    int len = stop - p->plfstrt;
    len = (len + p->fetchunit - 1) & ~(p->fetchunit - 1);
    p->fetch_total = len + p->fetchunit;
    /* prvi blok postaje vidljiv kad je ravnina 1 (dohvaćena posljednja u
       bloku, na ciklusu maxplanes-1) dohvaćena - redoslijed nije slučajan,
       pitajte ravninu 1 */
    p->line_plfleft = p->fetch_start_h + p->maxplanes;
}

/* An ECS game drives an ECS Denise, which has no LOCT bit, no colour bank and
   no sprite colour offset: a COLORxx write is a whole 12-bit colour and sprites
   take colours 16-31. My chipset is AGA and obeys all three, so an ECS game
   playing with BPLCON3 bit 9 set has its palette written into the low nibbles
   and comes out at a sixteenth brightness - the "dark ECS games", whose LOCT
   came from graphics.library in AA mode after WHDLoad's SetChipRev (agaboot
   stubs that now). In ECS mode I present those registers as their reset
   values instead, so a COLORxx write is one whole 12-bit colour, there is no
   colour bank, and sprites take colours 16-31 - an ECS Denise. BPLCON3's
   bits an ECS Denise does have (BRDRBLNK, BRDNTRAN, ZDCLKEN, EXTBLKEN) are
   kept, and BPLCON2's RDRAM, which it does not have, is ignored.

   The mode is agaboot's choice for the game (sent to $DFF1F2 at sandbox
   entry: $5E00 AGA, $5E01 ECS, $5E03 the user's "always ECS"), because
   nothing in a WHDLoad slave reliably says which chipset a game draws for;
   ecs_says_aga takes it back when the game shows it is AGA after all.
   Nothing changes for AGA games: with this off the renderer is
   byte-identical, which tools/regress.py checks. */
#define BPLCON3_RESET 0x0C00
#define BPLCON4_RESET 0x0011

#define BPLCON3_ECS_BITS 0x0035     /* BRDRBLNK, BRDNTRAN, ZDCLKEN, EXTBLKEN */
uint16_t pf_con3(const playfield_t *p) { return p->ecs_mode ? (uint16_t)(BPLCON3_RESET | (p->bplcon3 & BPLCON3_ECS_BITS)) : p->bplcon3; }
uint16_t pf_con4(const playfield_t *p) { return p->ecs_mode ? BPLCON4_RESET : p->bplcon4; }

/* Colour clocks od reseta, dovoljno približno (redak se računa kao 228): kada su
   došla dva upisa 24-bitnog para, za okna niže. */
static inline uint32_t pf_now(const aga_t *a) { return a->line_no * 228u + (uint32_t)a->hpos; }
#define PAIR_CCK      200u          /* banka 0: niski upis unutar ovoga od visokog */
#define PAIR_CCK_BANK 912u          /* u banci: četiri retka, za loader koji upisuje svakoj
                                       banci visoke nibble prije niskih */

void pf_color_write(aga_t *a, int num, uint16_t v, int hpos)
{
    playfield_t *p = &a->pf;
    int rdram = (p->bplcon2 & BPLCON2_RDRAM) != 0;
    if (rdram && !p->ecs_mode) return;          /* AGA Lisa umjesto toga čita */
    if (p->ecs_mode && !rdram) {                /* (a shadow AGA paleta se ostavlja na miru) */
        /* isti upis kako ga AGA Lisa prima, u shadow paletu
           koju ecs_says_aga predaje: banka i LOCT koje BPLCON3 stvarno ima */
        uint16_t raw = p->bplcon3;
        int ai = ((raw >> 13) & 7) * 32 + num;
        uint32_t ar = (v >> 8) & 15, ag = (v >> 4) & 15, ab = v & 15, ac = a->color_aga[ai];
        uint32_t xr = ac >> 16, xg = (ac >> 8) & 0xFF, xb = ac & 0xFF;
        if (raw & BPLCON3_LOCT) { xr = (xr & 0xF0) | ar; xg = (xg & 0xF0) | ag; xb = (xb & 0xF0) | ab; }
        else                    { xr = ar | (ar << 4); xg = ag | (ag << 4); xb = ab | (ab << 4); }
        a->color_aga[ai] = (xr << 16) | (xg << 8) | xb;
        /* 24-bitni par boja - slijed kojim AGA softver upisuje 24-bitnu
            boju: BPLCON3 upisan s čistim LOCT-om, visoki nibble registra,
            BPLCON3 s postavljenim LOCT-om, njegovi niski nibble, druga
            vrijednost, sve blizu zajedno (PAIR_CCK; 32 boje banke stanu u
            PAIR_CCK_BANK). Nijedan ECS program nema razloga za to: zalutali
            LOCT ostaje postavljen, Chaos Engine preklop upisuje istu vrijednost
            dvaput (uzeto na bilo koji način, ista boja), a zalutali LOCT panela
            redak ispod gradijenta predaleko je od gradijentovog upisa (pronašao
            review, 2026-10-05). Wit Premium (WHDLoad, bez AGA zastavice) crta
            svoja crvena neba tako u svakom retku s FMODE 0 i jednom ili dvije
            ravnine, pa su dva znaka došla tek pri njegovom brisanju minutu
            kasnije, nakon što je svako nebo nacrtano kao dugine trake
            (2026-10-05; WinUAE snimka, korisnikov A500). U banci 0 par se
            uzima na AGA način i ništa se drugo ne mijenja. Parovi u dvije banke,
            jedna od njih nije banka 0, paleta su učitana preko banki - znak
            koji ecs_says_aga treba sam; jedna banka sama mogli bi biti smeće
            bankovnih bitova iz tablice (pronašao review). */
        int pair = 0;
        uint32_t now = pf_now(a), win = (raw & 0xE000) ? PAIR_CCK_BANK : PAIR_CCK;
        if (raw & BPLCON3_LOCT) {
            pair = p->col_hi_pending[ai] && (v & 0x0FFF) != p->col_hi_val[ai] &&
                   now - p->col_hi_when[ai] <= win && !p->ecs_forced;
            p->col_hi_pending[ai] = 0;
        } else {
            p->col_hi_pending[ai] = p->loct_clear_seen && now - p->loct_clear_when <= win;
            p->col_hi_val[ai] = (uint16_t)(v & 0x0FFF);
            p->col_hi_when[ai] = now;
        }
        if (pair) {
            p->pair_banks = (uint8_t)(p->pair_banks | (1u << ((raw >> 13) & 7)));
            if ((p->pair_banks & 0xFE) && (p->pair_banks & (p->pair_banks - 1))) p->aga_pair_bank = 1;
        }
        if (raw & (BPLCON3_LOCT | 0xE000)) { p->aga_colour_seen = 1; ecs_says_aga(a); }
        if (!p->ecs_mode) {                     /* upravo predano: AGA paleta ima ovaj upis */
            aga_record_change(a, LC_COLOR + ai, a->color_aga[ai]);
            return;
        }
        if (pair && !(raw & 0xE000)) {          /* banka 0: ai je num, ECS indeks */
            uint32_t val = a->color_aga[ai];
            p->aga_pairs++;
            if (val == a->color[num]) return;
            a->color[num] = val;
            if (a->tone_on) a->color_b[num] = bright_rgb(a, val);
            aga_record_change(a, LC_COLOR + num, val);
            return;
        }
    }
    uint16_t con3 = pf_con3(p);
    int idx = ((con3 >> 13) & 7) * 32 + num;
    uint32_t r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    uint32_t cur = a->color[idx];
    uint32_t cr = cur >> 16, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
    if (con3 & BPLCON3_LOCT) {
        cr = (cr & 0xF0) | r; cg = (cg & 0xF0) | g; cb = (cb & 0xF0) | b;
    } else {
        cr = r | (r << 4); cg = g | (g << 4); cb = b | (b << 4);
    }
    uint32_t val = (cr << 16) | (cg << 8) | cb;
    if (val == cur) return;
    a->color[idx] = val;
    if (a->tone_on) a->color_b[idx] = bright_rgb(a, val);
    aga_record_change(a, LC_COLOR + idx, val);      /* puni indeks 0..255 */
    (void)hpos;
}

void pf_write(aga_t *a, uint32_t reg, uint16_t v, int hpos)
{
    playfield_t *p = &a->pf;
    if (reg >= BPL1PTH && reg < BPL1PTH + 32) {
        int n = (reg - BPL1PTH) >> 2;
        pf_sync(a, hpos);
        if ((reg & 2) == 0) p->bplpt[n] = (p->bplpt[n] & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16);
        else                p->bplpt[n] = (p->bplpt[n] & 0xFFFF0000u) | (v & 0xFFFE);
        return;
    }
    if (reg >= BPL1DAT && reg < BPL1DAT + 16) {
        p->bpldat[(reg - BPL1DAT) >> 1] = v;
        if (reg == BPL1DAT) p->line_bpl1dat = 1;
        return;
    }
    switch (reg) {
    case BPLCON0:
        if (p->bplcon0 == v) return;
        /* The bitplane DMA fetches the planes BPLCON0 allows at each slot, so a
           change mid-line counts from BPLCON0_DMA_DELAY on (WinUAE: the plane
           against bplcon0_planes_limit at every fetch). The count was taken
           once, where the line's fetch began. Zool (AGA) ends its status bar
           with BPLCON0 $0200 at colour clock $3E of line 59 and sets the
           playfield's pointers right after: WinUAE fetches up to $41 and
           stops, I fetched the rest of the line with four planes, those
           damn pointers ran a line ahead and the whole playfield came from the
           wrong rows (from RGB2RTG d888409). line_planes stays the most the
           line had, for drawing: the blocks a plane missed hold zeros
           (cleared each line). A line that began with no planes - or more
           than the fetch mode allows - has its fetch running all the same
           and fetches from here on; one that has fetched nothing yet takes
           the new resolution and fetch mode for drawing as well. */
        pf_sync(a, hpos + BPLCON0_DMA_DELAY);
        if ((v & 0x7010) == 0x0010) {            /* sam BPU3: točno osam bitplanea */
            p->aga_fetch_seen = 1;
            ecs_says_aga(a);
        }
        p->bplcon0 = v;
        p->res = GET_RES(v);
        p->planes = GET_PLANES(v);
        expand_fmodes(p);
        if (p->fetch_active) {
            if (p->line_planes == 0) latch_line(p);
            if (fetch_planes(p) > p->line_planes) p->line_planes = fetch_planes(p);
        }
        spr_expand_res(a);
        aga_record_change(a, BPLCON0, v);
        break;
    case BPLCON1:
        if (p->bplcon1 == v) return;
        pf_sync(a, hpos);
        p->bplcon1 = v;
        if (p->fetch_active) compute_delays(p);   /* stupa na snagu za sljedeće blokove */
        aga_record_change(a, BPLCON1, v);
        break;
    case BPLCON2:
        if (p->bplcon2 == v) return;
        p->bplcon2 = v;
        aga_record_change(a, BPLCON2, v);
        break;
    /* crtanje retka prima promjenu kao što bi i ECS Denise: njegov
       početak koristi pf_con3/pf_con4, pa mora i promjena usred retka */
    case BPLCON3:
        if (p->ecs_mode && !(v & BPLCON3_LOCT)) {  /* čak i prepisan kako je bio: početak 24-bitnog para */
            p->loct_clear_seen = 1;
            p->loct_clear_when = pf_now(a);
        }
        if (p->bplcon3 == v) return;
        p->bplcon3 = v;
        spr_expand_res(a);
        aga_record_change(a, BPLCON3, pf_con3(p));
        break;
    case BPLCON4:
        if (p->bplcon4 == v) return;
        if (p->ecs_mode && (v & 0xFF00)) {       /* XOR boje bitplanea: AGA boje */
            p->aga_colour_seen = 1;
            ecs_says_aga(a);
        }
        p->bplcon4 = v;
        aga_record_change(a, BPLCON4, pf_con4(p));
        break;
    case BPL1MOD:
        pf_sync(a, hpos);
        p->bpl1mod = (int16_t)(v & 0xFFFE);
        break;
    case BPL2MOD:
        pf_sync(a, hpos);
        p->bpl2mod = (int16_t)(v & 0xFFFE);
        break;
    case DIWSTRT:
        p->diwstrt = v; p->diwhigh_written = 0; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWSTRT, v);
        break;
    case DIWSTOP:
        p->diwstop = v; p->diwhigh_written = 0; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWSTOP, v);
        break;
    case DIWHIGH:
        p->diwhigh = v; p->diwhigh_written = 1; calcdiw(p, aga_short_line(a));
        aga_record_change(a, DIWHIGH, v);
        break;
    case DDFSTRT:
        pf_sync(a, hpos);
        p->ddfstrt = v & 0xFE; calcdiw(p, aga_short_line(a));
        break;
    case DDFSTOP:
        pf_sync(a, hpos);
        p->ddfstop = v & 0xFE; calcdiw(p, aga_short_line(a));
        break;
    case FMODE:
        pf_sync(a, hpos);
        if (v & 0xC00F) {                        /* širina fetcha/spritea, BSCAN2, SSCAN2 */
            p->aga_fetch_seen = 1;
            ecs_says_aga(a);
        }
        p->fmode = v;
        p->fm = (v & 3) == 3 ? 2 : (v & 3) ? 1 : 0;
        expand_fmodes(p);
        spr_expand_res(a);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
void pf_vsync(aga_t *a)
{
    a->pf.vdiw = 0;
    a->pf.vdiw_ff = 0;
}

void pf_start_line(aga_t *a)
{
    playfield_t *p = &a->pf;
    /* Vertikalni display window flip-flop i blanking koji ga skriva.
     *
     * To su dvije različite stvari i bile su jedna. Vertikalni blanking
     * inhibira dohvaćanje, ali ne dira flip-flop: igra čiji DIWSTRT
     * započinje okno UNUTAR blanka - Worms: The
     * Director's Cut postavlja $0081, redak 0 - ima okno otvoreno od
     * trenutka kad blank završi. Čišćenje flip-flopa ondje umjesto toga gubilo
     * je jedini redak koji ga ikad postavlja, i okno je zatim ostalo zatvoreno
     * do kraja framea, i svakog framea nakon toga: osam
     * bitplanea, DMA uključen, pokazivači u pravu grafiku, a ni jedna riječ
     * nije dohvaćena. Crn zaslon zauvijek. */
    if (a->vpos == p->diw_vstop) p->vdiw_ff = 0;
    if (a->vpos == p->diw_vstart) p->vdiw_ff = 1;
    p->vdiw = p->vdiw_ff && a->vpos >= a->vblank_end;

    p->fetch_active = 0;
    p->fetch_done = 0;
    p->mod_added = 0;
    p->mod_planes = 0;
    memset(p->slot_used, 0, sizeof p->slot_used);
    p->fetch_cycle = 0;
    p->fetch_last_hpos = 0;
    p->line_plfleft = -1;
    p->line_blocks = 0;
    p->line_planes = 0;
    p->line_bpl1dat = 0;
    p->fetch_start_h = -1;
    /* samo blokovi koje je posljednji redak upisao: ostali su već nula (svaki
       redak čisti ono što je koristio, a reset je očistio sve). 8 KB po retku
       prolazilo je kroz cache za nekoliko stotina bajtova koliko redak dohvaća. */
    if (p->blocks_dirty > 0) {
        if (p->blocks_dirty > AGA_MAX_BLOCKS) p->blocks_dirty = AGA_MAX_BLOCKS;
        memset(p->blockdata, 0, (size_t)p->blocks_dirty * sizeof p->blockdata[0]);
        p->blocks_dirty = 0;
    }

    if (a->tone_on) {
        if (a->bright_dirty) {                   /* nova razina, ili boje postavljene oko pf_color_write */
            for (int i = 0; i < 256; i++) a->color_b[i] = bright_rgb(a, a->color[i]);
            a->bright_dirty = 0;
        }
        memcpy(line_color, a->color_b, sizeof line_color);
        memcpy(line_raw, a->color, sizeof line_raw);
    } else {
        memcpy(line_color, a->color, sizeof line_color);
    }
    for (int i = 0; i < 32; i++) line_ehb[i] = ehb_shown(a, a->color[i]);
    line_bplcon0 = p->bplcon0; line_bplcon1 = p->bplcon1; line_bplcon2 = p->bplcon2;
    line_bplcon3 = pf_con3(p); line_bplcon4 = pf_con4(p);
}

/* OR `nbits` bitova `data` (MSB prvi, data poravnat desno) u ravninu `plane` na bit-poziciju `pos`. */
static inline void put_bits(playfield_t *p, int plane, int pos, uint64_t data, int nbits)
{
    if (pos < 0 || pos + nbits > AGA_PLANE_WORDS * 64) return;
    int w = pos >> 6, b = pos & 63;
    /* postavi polje od nbits tako da njegov MSB padne na bit-poziciju `pos` računajući od MSB-a riječi w */
    int shift = 64 - b - nbits;   /* lijevi pomak unutar riječi w */
    if (shift >= 0) {
        p->bits[plane][w] |= data << shift;
    } else {
        p->bits[plane][w] |= data >> (-shift);
        if (w + 1 < AGA_PLANE_WORDS) p->bits[plane][w + 1] |= data << (64 + shift);
    }
}

static void fetch_plane(aga_t *a, int plane, int block, int hpos)
{
    playfield_t *p = &a->pf;
    uint32_t addr = p->bplpt[plane];
    uint64_t data;
    aga_trace_at(a, hpos, AGA_DMA_BITPLANE, (uint16_t)(BPL1DAT + 2 * plane), addr);
    switch (p->line_fm) {
    case 0:  data = chip_rd16(a, addr); p->bplpt[plane] += 2; break;
    case 1:  data = chip_rd32(a, addr); p->bplpt[plane] += 4; break;
    default: data = chip_rd64(a, addr); p->bplpt[plane] += 8; break;
    }
    if (data) a->pf_data_nonzero = 1;   /* diagnostika: je li ikad išta napunilo bitplanee */
    if (block < AGA_MAX_BLOCKS) {
        p->blockdata[block][plane] = data;
        if (block >= p->blocks_dirty) p->blocks_dirty = block + 1;
    }
}

/* Blok je dovršen kad prođe njegov posljednji slot (ravnina 1): scroll odgoda
   na snazi u tom trenutku vrijedi za cijeli blok (UAE beginning_of_plane_block). */
static void complete_block(playfield_t *p, int block)
{
    if (block < AGA_MAX_BLOCKS) {
        p->blockdelay1[block] = (int16_t)p->line_delay1;
        p->blockdelay2[block] = (int16_t)p->line_delay2;
    }
}

static void build_bits(playfield_t *p)
{
    int W = 16 << p->line_fm;
    int nb = p->line_blocks < AGA_MAX_BLOCKS ? p->line_blocks : AGA_MAX_BLOCKS;
    memset(p->bits, 0, sizeof p->bits);
    for (int b = 0; b < nb; b++)
        for (int pl = 0; pl < p->line_planes; pl++)
#ifdef SCROLL_MODEL_BLOCK
            put_bits(p, pl, b * W + ((pl & 1) ? p->blockdelay2[b] : p->blockdelay1[b]), p->blockdata[b][pl], W);
#else
            put_bits(p, pl, b * W, p->blockdata[b][pl], W);
#endif
}

static void add_modulos(aga_t *a)
{
    playfield_t *p = &a->pf;
#ifdef PFDBG
    if (getenv("MODDBG") && a->vpos >= 0x40 && a->vpos <= 0x46)
        fprintf(stderr, "v$%03x add_modulos at h=%d: mod1=%d mod2=%d bpl1=$%06x bpl2=$%06x\n",
                a->vpos, p->fetch_last_hpos, p->bpl1mod, p->bpl2mod, p->bplpt[0], p->bplpt[1]);
#endif
    if (p->fmode & 0x4000) {
        /* BSCAN2, AGA bitplane scan-doubling: svaka ravnina uzima ISTI
           modulo na retku, BPL1MOD i BPL2MOD izmjenjuju se redak po redak prema
           parnosti DIWSTRT početnog retka (WinUAE getbplmod). Uz BPL1MOD
           postavljen na minus jedan redak, svaki redak dohvaća se dvaput - 1x2 chunky
           ekran iz pola memorije i pola konverzije. Synergy
           (Sector 7) vrti tako cijeli svoj intro; uzimanje neparne/parne
           podjele ravnina umjesto toga pokazivalo je svaki izvorni redak jednom, pa je slika
           izašla pola visine s vrijednošću sljedećeg međuspremnika memorije pod
           sobom. */
        int mod = (((p->diwstrt >> 8) ^ (a->vpos ^ 1)) & 1) ? p->bpl1mod : p->bpl2mod;
        for (int i = 0; i < AGA_MAX_PLANES; i++)
            if (p->mod_planes & (1u << i)) p->bplpt[i] += mod;
        return;
    }
    /* samo ravnine dohvaćene u posljednjem bloku retka: WinUAE dodaje
       modul ravnine s njezinim vlastitim dohvaćanjem ondje, pa ravnina koju je BPLCON0
       usred retka isključio prije njezinog slota ne uzima ništa, a ona uključena nakon svog
       slota također ne uzima ništa */
    for (int i = 0; i < AGA_MAX_PLANES; i++)
        if (p->mod_planes & (1u << i)) p->bplpt[i] += (i & 1) ? p->bpl2mod : p->bpl1mod;
}

/* Dodaj ih jednom po retku, kad je posljednje bitplane dohvaćanje retka gotovo i
   s vrijednostima na snazi u tom trenutku - ne nakon završnih ciklusa
   posljednjeg fetch bloka. Dijaloški okvir Reshoot Proxima 3 pušta copper
   odmah nakon posljednjeg dohvaćanja (završava na colour clocku 195, copper
   upisuje na 200): novi pokazivači za ravnine 1, 3, 5 i 7, i novi modulosi.
   Dodavanje modulosa poslije primijenilo je nove na svježe upisane
   pokazivače, pa je svaki redak ispod čitao s pogrešne adrese - šumni
   portret, udvostručeni tekst i kolona šuma niz zaslon. */
static void add_modulos_once(aga_t *a)
{
    if (a->pf.mod_added) return;
    a->pf.mod_added = 1;
    add_modulos(a);
}

/* Jedan ciklus dohvaćanja retka: ravnina kojoj njegov slot pripada, ako BPLCON0
   dopušta DMA-i da ga uzme, i dovršetak bloka na njegovom posljednjem slotu (ravnina
   1). in_line je 0 za cikluse dovršene na kraju retka, koji
   stvarno padaju u prve clockove sljedećeg retka: oni nisu slotovi
   ovog retka za copper. */
static void fetch_step(aga_t *a, int h, int in_line)
{
    playfield_t *p = &a->pf;
    int cyc = p->fetch_cycle & (p->fetchstart - 1);
    int block = p->fetch_cycle >> p->fetchstart_shift;
    if (cyc < p->maxplanes) {
        int plane = slot_plane(p->maxplanes, cyc);
        int last = ((block + 1) << p->fetchstart_shift) >= p->fetch_total;
        if (plane <= fetch_planes(p)) {
            fetch_plane(a, plane - 1, block, h);
            if (in_line && h >= 0 && h < 256) p->slot_used[h >> 6] |= 1ull << (h & 63);
            if (last) p->mod_planes |= 1u << (plane - 1);
        }
        if (cyc == p->maxplanes - 1) {
            complete_block(p, block);
            if (last) add_modulos_once(a);
        }
    }
    p->fetch_cycle++;
}

/* Dohvaćanje retka je gotovo: na posljednjem bloku DDFSTOP-a ili na kraju
   retka. Dohvaćanje može nastaviti u prve cikluse sljedećeg retka (hardver
   nastavlja DMA slotove); ta dohvaćanja samo pomiču
   pokazivače, podaci padaju u horizontalni blanking, pa se preostali slotovi
   posljednjeg bloka dovršavaju ovdje odmah. */
static void end_fetch(aga_t *a, int h)
{
    playfield_t *p = &a->pf;
    while (p->fetch_cycle < p->fetch_total) fetch_step(a, h, 0);
    p->fetch_active = 0;
    p->fetch_done = 1;
    p->line_blocks = (p->fetch_cycle + p->fetchstart - 1) >> p->fetchstart_shift;
    add_modulos_once(a);
}

void pf_sync(aga_t *a, int hpos)
{
    playfield_t *p = &a->pf;
    if (hpos > a->maxhpos) hpos = a->maxhpos;
    int h = p->fetch_last_hpos;
    if (h >= hpos) return;

    for (; h < hpos; h++) {
        if (!p->fetch_active && !p->fetch_done && h == p->plfstrt + DDF_OFFSET) {
            /* što je od četiriju stvari koje treba redak s dohvaćanjem
               nedostajalo - uvijek ista četiri, jebemu */
            a->dbg_fetch[0]++;
            if (!p->vdiw) a->dbg_fetch[1]++;
            else if (!aga_dma_enabled(a, DMAF_BPLEN)) a->dbg_fetch[2]++;
            else if (p->planes <= 0) a->dbg_fetch[3]++;
            else if (p->planes > p->maxplanes) a->dbg_fetch[4]++;
            else a->dbg_fetch[5]++;
            /* Fetch sekvencer pokreću okno, BPLEN i DDFSTRT
               sami; broj ravnina u BPLCON0 odlučuje samo, slot po slot, koje
               ravnine dohvaća (WinUAE: bprun bez člana ravnine, zatim
               bplcon0_planes_limit pri svakom dohvaćanju). Redak koji počinje bez
               ravnina i dobije ih uključene nekoliko clockova kasnije - copper
               MOVE u BPLCON0 točno prije početka dohvaćanja ili točno nakon njega
               - dohvaća ostatak retka; pokrenut samo s ravninama, izgubio bi
               cijeli redak. */
            if (p->vdiw && aga_dma_enabled(a, DMAF_BPLEN)) {
                /* DDFSTOP se poklopio prije nego je dohvaćanje moglo početi (calcdiw) */
                if (p->plfstop < p->plfstrt) { p->fetch_done = 1; continue; }
                p->fetch_active = 1;
                p->fetch_cycle = 0;
                p->fetch_start_h = h;
                p->line_planes = fetch_planes(p);
                latch_line(p);
            }
        }
        if (p->fetch_active) {
            fetch_step(a, h, 1);
            if (p->fetch_cycle >= p->fetch_total || h + 1 >= a->maxhpos)
                end_fetch(a, h);
        }
    }
    p->fetch_last_hpos = hpos;
}

/* Je li colour clock hpos slot koji je bitplane DMA uzeo? Ono što je dohvaćanje
   učinilo ondje, a ne što BPLCON0 kaže sada: BPLCON0 upis stiže do dohvaćanja
   BPLCON0_DMA_DELAY clockova kasnije, a ti su slotovi već otišli
   starom broju ravnina. */
int pf_is_bpl_slot(aga_t *a, int hpos)
{
    playfield_t *p = &a->pf;
    pf_sync(a, hpos + 1);
    if (hpos < 0 || hpos >= 256) return 0;
    return (int)((p->slot_used[hpos >> 6] >> (hpos & 63)) & 1);
}

/* Koliko nakon upisa na colour clocku hpos se pokazuje promjena registra, u
   hires pikselima. BPLCON1 scroll vidi se 3 lores piksela kasnije; boja se
   vidi na samom upisu, gdje je ta odgoda bila 2 hires od prvog commita jezgre
   ("small pipeline delay", nikad kalibrirano) i stavljala svaku
   boju 1 lores piksel desno od WinUAE - pojas neba Shadow of the Beast i
   Castlevania rezultate (iz RGB2RTG, EMM #2061: 10 render promjena, svaka
   bliža WinUAE, nijedna gora). */
static inline int change_delay(uint32_t reg)
{
    if (reg == BPLCON1) return 6;
    if (reg >= LC_COLOR) return 0;
    return 2;
}

/* Traje li bitplane dohvaćanje retka na colour clocku hpos - cijelo
   okno od prvog ciklusa dohvaćanja do posljednjeg, uključujući slobodne cikluse? WinUAE
   bprun: sprite DMA ne dobiva slot dok je postavljen (spr_dma_slot). Isto
   sustizanje kao pf_is_bpl_slot. */
int pf_bpl_running(aga_t *a, int hpos)
{
    playfield_t *p = &a->pf;
    pf_sync(a, hpos + 1);
    return p->fetch_start_h >= 0 && hpos >= p->fetch_start_h && hpos < p->fetch_start_h + p->fetch_total;
}

/* -------------------------------------------------------------------------- */
/* planar -> chunky za cijeli redak, neparne i parne ravnine odvojeno (obje drže
   svoje izvorne bit-pozicije: neparne ravnine u bitovima 0,2,4,6, parne u 1,3,5,7) */
/* Osam piksela odjednom: bajt svake ravnine razastire se u osam bajtova
   tablicom (bit 7 bajta, krajnji lijevi piksel, pada u prvi bajt)
   i pomiče do bita svoje ravnine. Sedam pretraga tablice po osam piksela
   gdje je bilo 56 koraka pomaka i maskiranja; isti bajtovi izlaze.
   Jezgra se gradi BIG-endian (-mbig-endian): tablica se gradi preko
   polja bajtova, pa "bajt i = piksel i" vrijedi u memoriji na oba reda bajtova, a
   pomaci nikad ne prenose izvan bajta (0/1 pomaknuti najviše 7 bitova). Prva
   verzija gradila ju je numerički, samo little-endian: na Pi svaka grupa od 8 piksela
   izašla je zrcaljena - Banshee nazubljen i pomaknut, dok je PC,
   little-endian, crtao svaki regresijski frame byte-identical. */
static void planar_to_chunky(playfield_t *p, int nbits)
{
    static uint64_t spread[256];
    static int spread_ready;
    if (!spread_ready) {
        for (int b = 0; b < 256; b++) {
            uint8_t px[8];
            for (int i = 0; i < 8; i++) px[i] = (b & (0x80 >> i)) ? 1 : 0;
            memcpy(&spread[b], px, 8);
        }
        spread_ready = 1;
    }
    int planes = p->line_planes;
    int words = (nbits + 63) >> 6;
    for (int w = 0; w < words; w++) {
        for (int k = 0; k < 8; k++) {
            int sh = 56 - 8 * k;                       /* pikseli 8k..8k+7 riječi */
            uint64_t vo = 0, ve = 0;
            for (int pl = 0; pl < planes; pl += 2) vo |= spread[(p->bits[pl][w] >> sh) & 0xFF] << pl;
            for (int pl = 1; pl < planes; pl += 2) ve |= spread[(p->bits[pl][w] >> sh) & 0xFF] << pl;
            memcpy(&p->idx_odd[w * 64 + k * 8], &vo, 8);   /* bajt i = piksel 8k+i, bilo koji red bajtova */
            memcpy(&p->idx_even[w * 64 + k * 8], &ve, 8);
        }
    }
}

/* Srednja vrijednost dvaju piksela, kanal po kanal. Super-hires stavlja dva piksela u
   svaki izlazni (hires) piksel, a zadržavanje samo prvog - WinUAE slika
   s isključenim "Filtered low resolution" - baca pola svakog ditheringa:
   Virtual Karting 2 miješa četiri boje unutar jednog lores piksela da ih TV
   stopi, a njegovo tlo izašlo je kao trake dviju od njih. */
static inline uint32_t mix2(uint32_t a, uint32_t b)
{
    return (a & b & 0xFFFFFF) + (((a ^ b) & 0xFEFEFE) >> 1);
}

/* Boja jednog playfield piksela iz njegove sirove bitplane vrijednosti - dual playfield,
   EHB ili obično, s BPLAM - i ono što sprite prioritet treba: iste
   odluke kao petlje render_segment, za super-hires parove. */
static uint8_t dpf1_tab[256], dpf2_tab[256];
static int dpf_tab_ready;
static void dpf_tab_init(void)
{
    for (unsigned v = 0; v < 256; v++) {
        dpf1_tab[v] = (uint8_t)((v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8));
        dpf2_tab[v] = (uint8_t)(((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8));
    }
    dpf_tab_ready = 1;
}
static inline uint32_t pf_pixel(unsigned v, int dpf, int ehb, int pf2pri, int pf2of, unsigned xorv,
                                int *pfv, int *front_pf)
{
    *front_pf = 2;
    if (dpf) {
        unsigned v1 = dpf1_tab[v & 255], v2 = dpf2_tab[v & 255], idx;
        if (pf2pri) {
            if (v2)      { idx = v2 + pf2of; *pfv = (int)v2; }
            else if (v1) { idx = v1;         *pfv = (int)v1; *front_pf = 1; }
            else         { idx = 0;          *pfv = 0; }
        } else {
            if (v1)      { idx = v1;         *pfv = (int)v1; *front_pf = 1; }
            else if (v2) { idx = v2 + pf2of; *pfv = (int)v2; }
            else         { idx = 0;          *pfv = 0; }
        }
        return line_color[(idx ^ xorv) & 255];
    }
    *pfv = (int)v;
    if (ehb) return (v & 0x20) ? line_ehb[(v & 31) ^ (xorv & 31)] : line_color[(v ^ xorv) & 255];
    return line_color[(v ^ xorv) & 255];
}
/* sprite piksel na super-hires poziciji sx preko boje c, ako pobjeđuje */
static inline uint32_t spr_pixel(uint32_t c, const uint8_t *spr, int sprlen, int sx, int pfv, int front_pf,
                                 int pf1p, int pf2p, int base_even, int base_odd)
{
    uint8_t sv = (sx >= 0 && sx < sprlen) ? spr[sx] : 0;
    if (!sv) return c;
    int pair = sv >> 6;
    int visible = (pfv == 0) ? 1 : (front_pf == 1 ? pair < pf1p : pair < pf2p);
    if (!visible) return c;
    int cc = sv & 15;
    int idx = (sv & 0x10) ? (base_odd + cc) : (((sv & 0x20) ? base_odd : base_even) + pair * 4 + cc);
    return line_color[idx & 255];
}

/* Sprite redak piksel sv preko boje c, ako je njegov par sprijeda (pair <
   thr: prag brzog puta). Sprite redak je u super-hires pikselima
   a izlaz u hires, pa lores ili hires playfield uzima dva sprite
   piksela na svaki izlazni piksel. Lores i hires spriteovi uvijek pune oba;
   super-hires (BPLCON3 SPRES = 3) mogu se razlikovati, a uzimanje samo prvog
   bacalo je pola ditheringa: menu pointer Super Stardust AGA, super-hires
   šahovnica (jedan redak 1010..., sljedeći 0101...), izašla je kao pune linije i
   praznine. Oba se crtaju i miješaju, kao što se miješaju parovi super-hires playfielda
   (iz RGB2RTG 04538c4, EMM #2157). */
static inline uint32_t spr_thr(uint32_t c, uint8_t sv, int thr, int base_even, int base_odd)
{
    if (!sv || (sv >> 6) >= thr) return c;
    int cc = sv & 15;
    int idx = (sv & 0x10) ? (base_odd + cc) : (((sv & 0x20) ? base_odd : base_even) + (sv >> 6) * 4 + cc);
    return line_color[idx & 255];
}

/* Sirova vrijednost playfield piksela, dekodirana jednom po postavci umjesto na
   svakom pikselu - bitovi 0-7 indeks boje (BPLAM primijenjen), bit 8 EHB-ova pola
   svjetline, bitovi 9-11 sprite parovi pokazani ispred njega (pair < ovoga).
   Iste odluke kao opća petlja render_segment; gradi se ponovno samo kad to
   BPLCON2/3/4 retka ili način promijene. Od RGB2RTG 0.75-dev18 (istog
   autora): grane dual playfielda na svakom pikselu bile su trećina
   pixel petlje Jim Powera na Pi 3. */
static uint16_t pfx_tab[256];
static int pfx_key = -1;
static const uint16_t *pfx_table(int dpf, int ehb, int pf2pri, int pf2of, unsigned xorv, int pf1p, int pf2p)
{
    int key = dpf | ehb << 1 | pf2pri << 2 | pf2of << 3 | (int)xorv << 11 | pf1p << 19 | pf2p << 22;
    if (key == pfx_key)
        return pfx_tab;
    if (!dpf_tab_ready) dpf_tab_init();
    int t1 = pf1p < 4 ? pf1p : 4, t2 = pf2p < 4 ? pf2p : 4;
    for (unsigned v = 0; v < 256; v++) {
        unsigned idx, half = 0;
        int thr;
        if (dpf) {
            unsigned v1 = dpf1_tab[v], v2 = dpf2_tab[v];
            if (pf2pri) {
                if (v2)      { idx = v2 + pf2of; thr = t2; }
                else if (v1) { idx = v1;         thr = t1; }
                else         { idx = 0;          thr = 4; }
            } else {
                if (v1)      { idx = v1;         thr = t1; }
                else if (v2) { idx = v2 + pf2of; thr = t2; }
                else         { idx = 0;          thr = 4; }
            }
            idx = (idx ^ xorv) & 255;
        } else {
            thr = v ? t2 : 4;
            if (ehb && (v & 0x20)) { idx = (v & 31) ^ (xorv & 31); half = 1; }
            else                     idx = (v ^ xorv) & 255;
        }
        pfx_tab[v] = (uint16_t)(idx | half << 8 | (unsigned)thr << 9);
    }
    pfx_key = key;
    return pfx_tab;
}

typedef struct segstate {
    uint16_t con0, con1, con2, con3, con4;
    int diw_hstart, diw_hstop;
    uint32_t ham_last;
} segstate_t;

static void apply_change(aga_t *a, segstate_t *s, const linechange_t *c)
{
    if (c->reg >= LC_COLOR && c->reg < LC_COLOR + 256) {
        int i = c->reg - LC_COLOR;
        if (a->tone_on) { line_raw[i] = c->value; line_color[i] = bright_rgb(a, c->value); }
        else line_color[i] = c->value;
        if (i < 32) line_ehb[i] = ehb_shown(a, c->value);
        return;
    }
    switch (c->reg) {
    case BPLCON0: s->con0 = (uint16_t)c->value; break;
    case BPLCON1: s->con1 = (uint16_t)c->value; break;
    case BPLCON2: s->con2 = (uint16_t)c->value; break;
    case BPLCON3: s->con3 = (uint16_t)c->value; break;
    case BPLCON4: s->con4 = (uint16_t)c->value; break;
    case DIWSTRT: case DIWSTOP: case DIWHIGH:
        s->diw_hstart = a->pf.diw_hstart; s->diw_hstop = a->pf.diw_hstop;   /* aproksimacija: koristi konačno */
        break;
    default: break;
    }
}

static const int pf2of_table[8] = { 0, 2, 4, 8, 16, 32, 64, 128 };

/* Detekcija sudara, po UAE (E-UAE do_sprite_collisions i
   do_playfield_collisions; WinUAE denise_collide_sprites, s CLXCON
   maskama kao u expand_colmask): računa se na pikselima kako se crtaju,
   unutar prikaznog okna, pa vidi isti scroll i okno kao
   slika. Sprite grupe: grupa g je sprite 2g, plus sprite 2g+1 ako to CLXCON
   omogućuje. */

/* grupe prisutne na pikselu (bit g) -> sprite-na-sprite bitovi 9..14 CLXDAT-a
   (E-UAE sprclx tablica) */
static const uint16_t clx_sprspr[16] = {
    0, 0, 0, 0x0200, 0, 0x0400, 0x1000, 0x1600,
    0, 0x0800, 0x2000, 0x2A00, 0x4000, 0x4C00, 0x7000, 0x7E00
};

static void render_segment(aga_t *a, segstate_t *s, uint32_t *out, int hx0, int hx1, int have_data, int x0, int nbits, int clx)
{
    playfield_t *p = &a->pf;
    int res = p->line_res;
    int planes = p->line_planes;
    uint16_t con0 = s->con0;
    int ecsena = con0 & BPLCON0_ECSENA;
    int ham = (con0 & BPLCON0_HAM) && (planes == 6 || planes == 8);
    int dpf = (con0 & BPLCON0_DPF) != 0;
    int ehb = (planes == 6) && !ham && !dpf && !(s->con2 & BPLCON2_KILLEHB);
    int brdrblnk = ecsena && (s->con3 & BPLCON3_BRDRBLNK);
    int brdsprt = ecsena && (s->con3 & BPLCON3_BRDSPRT);
    int pf1p = s->con2 & 7, pf2p = (s->con2 >> 3) & 7;
    int pf2pri = s->con2 & BPLCON2_PF2PRI;
    int pf2of = pf2of_table[(s->con3 >> 10) & 7];
    unsigned xorv = (s->con4 >> 8) & 0xFF;
    /* BPLCON4 osnovne boje spriteova, kako ih WinUAE primjenjuje (drawing.cpp):
       visoki nibble osnova je za piksel niže numeriranog (parnog) spritea
       para, niski nibble za više numerirani (neparni) i za
       spojeni par. Uz reset vrijednost $11 oba su 16, zbog čega samo
       igre koje programiraju BPLCON4 (Turrican 2: $65) razlikuju njih dvoje. */
    int base_even = ((s->con4 >> 4) & 15) << 4, base_odd = (s->con4 & 15) << 4;
    int vdiw = p->vdiw;
    const uint8_t *spr = a->sprline_valid ? a->sprline : NULL;
    /* Denise pokazuje spriteove na retku tek kad je BPL1DAT na njemu upisan
       - od bitplane DMA, ili od CPU-a ili coppera - i ponovno ih skriva
       na početku sljedećeg retka; ECS/AGA border spriteovi (BRDSPRT) pokazuju ih sve
       jednako (WinUAE drawing.cpp: hstart_new, bpl1dat_enable_sprites). Redak
       bez bitplane podataka border je cijelim putem, i unutar prikaznog
       okna. Lupus Reditus (AGA, Revision 2025) isključuje svoje ravnine za
       posljednja dva retka svog okna dok njegovih osam spriteova od 64 piksela, s
       VSTOP 4 pod VSTART 80, nastavljaju preko svojih podataka u kontrolne
       riječi sljedećeg spritea: te sam riječi crtao kao red točkica u četiri grupe
       pod slikom, što WinUAE ne crta i što reporterova Amiga
       očito ne pokazuje (RGB2RTG 0.76 izvještaj, EMM #2162). Prije
       prvog BPL1DAT retka koji
       ima podatke WinUAE ih također skriva; taj dio ovdje nije modeliran. */
    if (!have_data && !p->line_bpl1dat && !brdsprt) spr = NULL;
    int sprlen = (int)sizeof a->sprline;
    int d1 = 0, d2 = 0;
#ifndef SCROLL_MODEL_BLOCK
    if (have_data) delays_for(p, s->con1, &d1, &d2);
#endif
    /* sudari: omogućene ravnine, vrijednost koju moraju poklopiti, spriteovi koji sudjeluju */
    const uint8_t *sprm = (clx && a->sprclx_valid) ? a->sprclx : NULL;
    unsigned c_ena = 0, c_match = 0, c_emask = 0;
    uint16_t cx = a->clxdat & 1;                      /* bit 0 već nađen ovaj frame: preskoči tu pretragu */
    if (clx) {
        c_ena   = ((a->clxcon >> 6) & 0x3F) | (a->clxcon2 & 0xC0);
        c_match = ((a->clxcon & 0x3F) | ((a->clxcon2 & 3) << 6)) & c_ena;
        c_emask = 0x55 | ((a->clxcon >> 11) & 0x02) | ((a->clxcon >> 10) & 0x08)   /* ENSP1, ENSP3 */
                       | ((a->clxcon >> 9) & 0x20) | ((a->clxcon >> 8) & 0x80);    /* ENSP5, ENSP7 */
    }

    /* ---- brzi put: bez sudara, bez HAM-a --------------------------------
       Isti pikseli kao opća petlja ispod, s odlukama po pikselu
       izdignutim van: prikazno okno je raspon hx umjesto testa po
       pikselu, lores piksel dekodira se jednom za dva izlazna piksela koja
       pokriva, a sirova vrijednost piksela dekodira se jednim pogledom u tablicu
       (pfx_table, 1.114). Teške razine Reshoot Proxima 3 prekoračile su 20 ms
       frame na CM4 s ovom petljom na 40% vremena chipset jezgre.
       Byte-identical općoj petlji; hostovi regresijski frameovi su
       čuvar. */
    if (!clx && !ham) {
        const int L2 = AGA_OUT_LEFT_LORES * 2;
        int w0 = hx1, w1 = hx1;                       /* pikseli okna: [w0, w1) */
        if (vdiw) {
            int A = s->diw_hstart + DIW_HW_OFFSET * 4, B = s->diw_hstop + DIW_HW_OFFSET * 4;
            w0 = (A + 1) / 2 - L2;                    /* 2*hires >= A  <=>  hires >= ceil(A/2) */
            w1 = (B + 1) / 2 - L2;                    /* 2*hires <  B  <=>  hires <  ceil(B/2) */
            if (w0 < hx0) w0 = hx0;
            if (w0 > hx1) w0 = hx1;
            if (w1 > hx1) w1 = hx1;
            if (w1 < w0) w1 = w0;
        }
        uint32_t border = brdrblnk ? 0 : line_color[0];
        for (int side = 0; side < 2; side++) {       /* border lijevo i desno od okna */
            int b0 = side ? w1 : hx0, b1 = side ? hx1 : w0;
            for (int hx = b0; hx < b1; hx++) {
                uint32_t col = border;
                if (spr && brdsprt) {
                    int sx = (L2 + hx) << 1;
                    uint8_t sv = (sx < sprlen) ? spr[sx] : 0;
                    uint8_t sv2 = (sx + 1 < sprlen) ? spr[sx + 1] : 0;
                    col = spr_thr(border, sv, 4, base_even, base_odd);      /* u borderu se pokazuje svaki par */
                    if (sv2 != sv)
                        col = mix2(col, spr_thr(border, sv2, 4, base_even, base_odd));
                }
                out[hx] = col;
            }
        }
        if (res == RES_SHRES && !dpf_tab_ready) dpf_tab_init();
        const uint16_t *pfx = pfx_table(dpf, ehb, pf2pri != 0, pf2of, xorv, pf1p, pf2p);
        /* Sprite piksel leži samo gdje je sprite redak bio upisan
           (sprline_lo..sprline_hi), pa se okno lijevo i desno od toga
           crta bez sprite testa i bez čuvanja prioriteta (RGB2RTG 0.75-dev13:
           na Pi 3 test na svakom izlaznom pikselu bio je pola Jim Power
           pixel petlje). Gdje je okno odrezano ne mijenja ni jedan piksel: lores piksel
           se dekodira isto iz bilo kojeg od svoja dva izlazna piksela. */
        int s0 = w1, s1 = w1;
        if (res == RES_SHRES) { s0 = w0; s1 = w1; }   /* super-hires parovi drže svoj vlastiti test */
        else if (spr && a->sprline_hi > a->sprline_lo) {
            s0 = (a->sprline_lo + 1) / 2 - L2;        /* 2*(L2+hx) >= lo  <=>  L2+hx >= ceil(lo/2) */
            s1 = (a->sprline_hi + 1) / 2 - L2;
            if (s0 < w0) s0 = w0;
            if (s0 > w1) s0 = w1;
            if (s1 > w1) s1 = w1;
            if (s1 < s0) s1 = s0;
        }
        for (int part = 0; part < 3; part++) {
            int p0 = part == 0 ? w0 : part == 1 ? s0 : s1;
            int p1 = part == 0 ? s0 : part == 1 ? s1 : w1;
            if (part != 1) {                          /* nema spritea ovdje */
                for (int hx = p0; hx < p1; ) {
                    int hires = L2 + hx;
                    unsigned v = 0;
                    if (have_data) {
                        int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : (hires << 1);
                        int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                        if ((unsigned)i1 < (unsigned)nbits) v = p->idx_odd[i1];
                        if ((unsigned)i2 < (unsigned)nbits) v |= p->idx_even[i2];
                    }
                    unsigned e = pfx[v & 255];
                    uint32_t col = (e & 0x100) ? line_ehb[e & 31] : line_color[e & 255];
                    out[hx++] = col;
                    if (res == 0 && !(hires & 1) && hx < p1) out[hx++] = col;
                }
                continue;
            }
            for (int hx = p0; hx < p1; ) {
                int hires = L2 + hx;
                if (res == RES_SHRES) {
                    /* dva super-hires piksela na svaki izlazni piksel: oba dekodirana u
                       cijelosti, svaki sa svojim sprite pikselom, i izmiješana (mix2) */
                    uint32_t c2[2];
                    for (int sub = 0; sub < 2; sub++) {
                        int sx = (hires << 1) + sub;
                        unsigned vs = 0;
                        if (have_data) {
                            int i1 = sx - x0 - d1, i2 = sx - x0 - d2;
                            if ((unsigned)i1 < (unsigned)nbits) vs = p->idx_odd[i1];
                            if ((unsigned)i2 < (unsigned)nbits) vs |= p->idx_even[i2];
                        }
                        int pv, fr;
                        uint32_t c = pf_pixel(vs, dpf, ehb, pf2pri, pf2of, xorv, &pv, &fr);
                        if (spr) c = spr_pixel(c, spr, sprlen, sx, pv, fr, pf1p, pf2p, base_even, base_odd);
                        c2[sub] = c;
                    }
                    out[hx++] = mix2(c2[0], c2[1]);
                    continue;
                }
                unsigned v = 0;
                if (have_data) {
                    int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : (hires << 1);
                    int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                    if ((unsigned)i1 < (unsigned)nbits) v = p->idx_odd[i1];
                    if ((unsigned)i2 < (unsigned)nbits) v |= p->idx_even[i2];
                }
                unsigned e = pfx[v & 255];
                uint32_t col = (e & 0x100) ? line_ehb[e & 31] : line_color[e & 255];
                int thr = (int)(e >> 9);
                /* lores piksel pokriva dva izlazna piksela: dekodiraj jednom, prekrij dvaput */
                int n = (res == 0 && !(hires & 1) && hx + 1 < p1) ? 2 : 1;
                for (int k = 0; k < n; k++, hx++) {
                    uint32_t c = col;
                    if (spr) {
                        int sx = (hires + k) << 1;
                        uint8_t sv = (sx < sprlen) ? spr[sx] : 0;
                        uint8_t sv2 = (sx + 1 < sprlen) ? spr[sx + 1] : 0;
                        c = spr_thr(col, sv, thr, base_even, base_odd);
                        if (sv2 != sv)                    /* super-hires spriteovi: par izmiješan */
                            c = mix2(c, spr_thr(col, sv2, thr, base_even, base_odd));
                    }
                    out[hx] = c;
                }
            }
        }
        return;
    }

    for (int hx = hx0; hx < hx1; hx++) {
        int hires = AGA_OUT_LEFT_LORES * 2 + hx;      /* hires hardverska koordinata (temeljena na hpos*4) */
        /* super-hires: dva piksela na svaki izlazni piksel, dekodirana jedan za
           drugim (HAM boja prenosi se iz prvog u drugi)
           i izmiješana (mix2); pretraga sudara gleda prvi, kao
           oduvijek. Lores i hires imaju jedan prolaz, kao prije. */
        int subs = (res == RES_SHRES) ? 2 : 1;
        uint32_t c2[2];
        for (int sub = 0; sub < subs; sub++) {
            /* DIW vrijednosti registara pomaknute su za DIW_DDF_OFFSET-1 od
               hardverskih koordinata (UAE) - jer zašto bi koordinate bile
               jednostavne, fuck */
            int sx = (hires << 1) + sub;                  /* superhires hardverska koordinata */
            int in_window = vdiw && sx >= s->diw_hstart + DIW_HW_OFFSET * 4
                                 && sx <  s->diw_hstop  + DIW_HW_OFFSET * 4;
            uint32_t col;
            int pfv = 0;             /* vrijednost playfield piksela za sprite prioritet */
            int front_pf = 2;        /* koji je playfield sprijeda za prioritet (1 ili 2) */
            unsigned v = 0;          /* sirova bitplane vrijednost, bit n = ravnina n+1 */
            int fetched = 0;         /* piksel leži u dohvaćenim bitplane podacima */

            if (in_window) {
                if (have_data) {
                    int nx = (res == 0) ? (hires >> 1) : (res == 1) ? hires : sx;
                    int i1 = nx - x0 - d1, i2 = nx - x0 - d2;
                    if (i1 >= 0 && i1 < nbits) { v = p->idx_odd[i1]; fetched = 1; }
                    if (i2 >= 0 && i2 < nbits) { v |= p->idx_even[i2]; fetched = 1; }
                }
                if (dpf) {
                    unsigned v1 = (v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8);
                    unsigned v2 = ((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8);
                    /* Indeks boje gradi se prvi - vrijednost pobjedničkog playfielda,
                        plus PF2OF kad je to playfield 2 - a BPLAM (visoki bajt BPLCON4)
                        XOR-a se u cijeli 8-bitni rezultat nakon toga, točno kako
                        WinUAE radi (decode_pixel_aga, CMODE_DUALPF). Vrijedi za oba
                        playfielda i za pozadinu, pa je uz postavljen BPLAM pozadina
                        boja BPLAM, a ne boja 0. XOR-anje samo niskih četiri bita u
                        playfield 1 ostavilo je dijalog Reshoot Proxima 3 da crta iz
                        boja 0-15 gdje je igra stavila paletu na 160 (BPLAM $A0
                        za retke koje okvir pokriva): šumni portret, kolona
                        boje niz zaslon i pogrešne boje tla. */
                    unsigned idx;
                    if (pf2pri) {
                        if (v2)      { idx = v2 + pf2of; pfv = v2; front_pf = 2; }
                        else if (v1) { idx = v1;         pfv = v1; front_pf = 1; }
                        else           idx = 0;
                    } else {
                        if (v1)      { idx = v1;         pfv = v1; front_pf = 1; }
                        else if (v2) { idx = v2 + pf2of; pfv = v2; front_pf = 2; }
                        else           idx = 0;
                    }
                    col = line_color[(idx ^ xorv) & 255];
    #ifdef PFDBG
                    {
                        static int v0 = -2, x0dbg = 0, x1dbg = 0;
                        if (v0 == -2) { const char *e = getenv("PFDBG"); v0 = -1; if (e) sscanf(e, "%i,%i,%i", &v0, &x0dbg, &x1dbg); }
                        if (a->vpos == v0 && hx >= x0dbg && hx <= x1dbg)
                            fprintf(stderr, "x%3d v=%02x v1=%x v2=%x idx=%3d col=%06x con1=$%04x d1=%d d2=%d\n",
                                    hx, v, v1, v2, (idx ^ xorv) & 255, col, s->con1, d1, d2);
                    }
    #endif
                } else if (ham) {
                    pfv = v;
                    const uint32_t *hb = a->tone_on ? line_raw : line_color;   /* stvarne boje */
                    if (planes == 8) {
                        switch (v & 3) {
                        case 0: s->ham_last = hb[(v >> 2) ^ (xorv >> 2)]; break;
                        case 1: s->ham_last = (s->ham_last & 0xFFFF03) | (v & 0xFC); break;
                        case 2: s->ham_last = (s->ham_last & 0x03FFFF) | ((v & 0xFC) << 16); break;
                        default: s->ham_last = (s->ham_last & 0xFF03FF) | ((v & 0xFC) << 8); break;
                        }
                    } else {
                        unsigned n = v & 15;
                        unsigned n8 = n | (n << 4);
                        switch (v & 0x30) {
                        case 0x00: s->ham_last = hb[n ^ (xorv & 15)]; break;
                        case 0x10: s->ham_last = (s->ham_last & 0xFFFF00) | n8; break;
                        case 0x20: s->ham_last = (s->ham_last & 0x00FFFF) | (n8 << 16); break;
                        default:   s->ham_last = (s->ham_last & 0xFF00FF) | (n8 << 8); break;
                        }
                    }
                    col = a->tone_on ? bright_rgb(a, s->ham_last) : s->ham_last;
                } else if (ehb) {
                    pfv = v;
                    if (v & 0x20) col = line_ehb[(v & 31) ^ (xorv & 31)];
                    else          col = line_color[v ^ xorv];
                } else {
                    pfv = v;
                    col = line_color[(v ^ xorv) & 255];
                }
            } else {
                col = brdrblnk ? 0 : line_color[0];
            }

            /* spriteovi */
            if (spr && (in_window || brdsprt)) {
                uint32_t under = col;
                int pv = in_window ? pfv : 0;
                col = spr_pixel(under, spr, sprlen, sx, pv, front_pf, pf1p, pf2p, base_even, base_odd);
                /* jedno dekodiranje po izlaznom pikselu (nije super-hires): i drugi piksel
                   super-hires spriteova, izmiješan (spr_thr) */
                if (subs == 1 && sx >= 0 && sx + 1 < sprlen && spr[sx + 1] != spr[sx])
                    col = mix2(col, spr_pixel(under, spr, sprlen, sx + 1, pv, front_pf, pf1p, pf2p, base_even, base_odd));
            }

            /* kad je bit 0 nađen, treba gledati samo piksele na kojima je sprite */
            unsigned m = (clx && sprm && sub == 0) ? (sprm[sx] & c_emask) : 0;
            if (clx && sub == 0 && in_window && (m || !(cx & 1))) {
                unsigned g = 0;                           /* sprite grupe s pikselom ovdje */
                if (m) {
                    m = (m | (m >> 1)) & 0x55;
                    g = (m & 1) | ((m >> 1) & 2) | ((m >> 2) & 4) | ((m >> 3) & 8);
                    cx |= clx_sprspr[g];
                }
                if (fetched) {
                    /* onemogućena ravnina uvijek se poklapa, pa nijedna omogućena ravnina = uvijek */
                    unsigned d = (v ^ c_match) & c_ena;
                    unsigned even_ok = !(d & 0xAA), odd_ok = !(d & 0x55);
                    if (even_ok && odd_ok) cx |= 0x0001;  /* parne na neparne bitplanee - da, čita se kako piše */
                    if (!dpf) odd_ok &= even_ok;          /* jedan playfield: neparni bitovi trebaju sve ravnine (WinUAE) */
                    if (odd_ok)  cx |= (uint16_t)(g << 1);
                    if (even_ok) cx |= (uint16_t)(g << 5);
                }
            }
            c2[sub] = col;
        }
        out[hx] = (subs == 2) ? mix2(c2[0], c2[1]) : c2[0];
    }
    a->clxdat |= cx;
}

void pf_finish_line(aga_t *a)
{
    playfield_t *p = &a->pf;
    /* Dohvaćanje koje još traje ovdje promašilo je kraj retka: VPOSW na dugom
       NTSC retku učinio ga je kratkim nakon što je dohvaćanje već prošlo
       novi posljednji clock. Njegov posljednji blok i modulosi duguju se
       svejedno - dugovi se plaćaju. */
    if (p->fetch_active) end_fetch(a, a->maxhpos - 1);
    int y = a->vpos - AGA_OUT_TOP;
    if (!a->cfg.render || y < 0 || y >= AGA_OUT_H) return;

    uint32_t *out = a->linebuf;
    int have_data = p->line_blocks > 0 && p->line_planes > 0;
    if (have_data && p->vdiw) {
        a->display_lines++;
        /* okno u hires pikselima framea, kako ga računa brzi put
           ispod, prije ikakvog odrezivanja na frame (Size Auto) */
        int wa = (p->diw_hstart + DIW_HW_OFFSET * 4 + 1) / 2 - AGA_OUT_LEFT_LORES * 2;
        int wb = (p->diw_hstop + DIW_HW_OFFSET * 4 + 1) / 2 - AGA_OUT_LEFT_LORES * 2;
        if (wa < a->win_x0) a->win_x0 = wa;
        if (wb > a->win_x1) a->win_x1 = wb;
        if (y < a->win_y0) a->win_y0 = y;
        if (y > a->win_y1) a->win_y1 = y;
    }
    /* preskočeni frame ne crta ništa - osim ako su se sudari uključili tijekom
       njega, jer CLXDAT računa renderer ispod */
    if (a->frame_skip && !(a->cfg.collisions || a->clx_frames > 0)) return;
    int nbits = 0, x0 = 0;
    if (have_data) {
        int W = 16 << p->line_fm;
        nbits = p->line_blocks * W + 64;
        if (nbits > AGA_PLANE_WORDS * 64) nbits = AGA_PLANE_WORDS * 64;
        build_bits(p);
        planar_to_chunky(p, nbits);
        x0 = (p->line_plfleft * 2 + DISP_OFFSET) << p->line_res;
    }

    int clx = a->cfg.collisions || a->clx_frames > 0;
    int any_sprite = 0;
    for (int i = 0; i < AGA_MAX_SPRITES; i++) if (a->spr[i].armed) { any_sprite = 1; break; }
    /* sprite upisan tijekom retka možda se pokazao i od tada razoružao */
    if (any_sprite || a->spr_touched) spr_build_line(a, clx); else a->sprline_valid = a->sprclx_valid = 0;

    segstate_t s;
    s.con0 = line_bplcon0; s.con1 = line_bplcon1; s.con2 = line_bplcon2;
    s.con3 = line_bplcon3; s.con4 = line_bplcon4;
    s.diw_hstart = p->diw_hstart; s.diw_hstop = p->diw_hstop;
    s.ham_last = a->tone_on ? line_raw[0] : line_color[0];

    int hx = 0;
    for (int i = 0; i < a->nchanges; i++) {
        const linechange_t *c = &a->changes[i];
        /* promjena stupa na snagu na lores pikselu 2*hpos plus vlastita odgoda
           registra (change_delay iznad) */
        int chx = (int)c->hpos * 4 + change_delay(c->reg) - AGA_OUT_LEFT_LORES * 2;
        if (chx > AGA_OUT_W) chx = AGA_OUT_W;
        if (chx > hx) {
            render_segment(a, &s, out, hx, chx, have_data, x0, nbits, clx);
            hx = chx;
        }
        apply_change(a, &s, c);
    }
    if (hx < AGA_OUT_W) render_segment(a, &s, out, hx, AGA_OUT_W, have_data, x0, nbits, clx);
    /* završeni redak, u jednom prolazu: vanjska stranica (uncached HVS memorija)
       dobiva alpha bajt koji alpha način ravnine treba, a vlastiti frameovi
       jezgre ostaju byte-identical s onim što host testovi uspoređuju */
    uint32_t *row;
    if (a->weave_now)      /* tkan: dvije stranice kao jedan frame od 572 retka (aga_set_weave) */
        row = a->fb[0] + (2 * y + a->weave_row) * AGA_OUT_W;
    else
        row = a->fb[a->fb_cur] + y * AGA_OUT_W;
    for (int twice = a->weave_now && a->weave_fill; ; twice = 0) {
        if (a->fb_external) { for (int i = 0; i < AGA_OUT_W; i++) row[i] = out[i] | 0xFF000000u; }
        else memcpy(row, out, sizeof a->linebuf);
        if (!twice) break;
        /* prvi tkani frame: i redak između, ne stara slika */
        row = a->fb[0] + (2 * y + (a->weave_row ^ 1)) * AGA_OUT_W;
    }
    /* the line, still in my cache, for whoever gives a damn (the RGB port) */
    if (a->hooks.line_done && !a->weave_now) a->hooks.line_done(a->hooks.user, y, a->fb_cur, out);
}
