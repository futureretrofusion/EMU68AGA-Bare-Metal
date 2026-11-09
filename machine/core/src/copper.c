/* AGA-STORM — Copper koprocesor.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Model: jedan pristup memoriji na dva colour clocka. MOVE = 2 pristupa, WAIT i
 * SKIP = 2 pristupa plus 2-ciklusna odgoda usporedbe. Usporedbe zraka koriste
 * položaj *sljedećeg* copper ciklusa, kao hardver. Ciklus koji je već zauzeo
 * bitplane DMA nije dostupan copperu.
 *
 * The classic UAE copper - update_copper and its states, as kept in E-UAE's
 * custom.c - translated to C: the states and fields, reading, WAIT, SKIP,
 * the blitter wait and the idle test. The WAIT/SKIP compare values and the
 * beam compare are as in WinUAE custom.cpp (coppercomp). The cycle timing,
 * and what a COPJMP written by the copper does, were measured from WinUAE's
 * DMA records; a restart or a CPU COPJMP takes its pointer at the first
 * fetch, as WinUAE's copper does (getstrobecopip), in my own code.
 *   E-UAE custom.c:    Copyright 1995-2002 Bernd Schmidt,
 *                      Copyright 1995 Alessandro Bissacco,
 *                      Copyright 2000-2004 Toni Wilen
 *   WinUAE custom.cpp: Copyright 1995-2002 Bernd Schmidt,
 *                      Copyright 1995 Alessandro Bissacco,
 *                      Copyright 2000-2025 Toni Wilen
 * Promijenjeno u odnosu na UAE/WinUAE kod za AGA-STORM 2026.
 * UAE i WinUAE su GPL, kao i ova datoteka.
 */
#include "internal.h"

void cop_reset(aga_t *a)
{
    memset(&a->cop, 0, sizeof a->cop);
    a->cop.state = COP_stop;
}

void cop_write_lc(aga_t *a, int which, int high, uint16_t v)
{
    uint32_t *lc = (which == 1) ? &a->cop.cop1lc : &a->cop.cop2lc;
    if (high) *lc = (*lc & 0x0000FFFF) | ((uint32_t)(v & 0x01FF) << 16);
    else      *lc = (*lc & 0xFFFF0000) | (v & 0xFFFE);
}

void cop_strobe(aga_t *a, int which)
{
    copper_t *c = &a->cop;
    c->ignore_next = 0;
    if (c->in_move) {
        /* Upisao ga je sam copper: hardver je već dohvatio sljedeću instrukcijsku
           riječ, pa se izvrši točno još jedna instrukcija iz stare liste prije
           nego skok stupi na snagu (WinUAE zapisi: Castlevania redak 52 i
           Turrican 2 redak 194 pokazuju COPJMP2, zatim još jednu instrukciju
           pročitanu iz stare liste, pa novu listu). Izvršavanje te instrukcije
           kvari svaku listu koja nakon skoka završava WAIT terminatorom, pa se
           dohvaća i odbacuje. */
        c->strobe = which;
        c->ignore_next = 1;       /* ta se instrukcija dohvaća, ali ne izvršava */
        return;
    }
    /* Upisao ga je CPU: stupa na snagu odmah, nakon jednog ciklusa mirovanja.
       Sam pokazivač čita se na prvom dohvaćanju (c->reload), kao u WinUAE,
       što je važno kad je copper DMA isključen - vidi cop_vsync. */
    c->pc = (which == 1) ? c->cop1lc : c->cop2lc;
    c->reload = which;
    c->state = COP_strobe_delay;
    c->strobe = 0;
}

void cop_vsync(aga_t *a)
{
    /* Vertikalni blank ponovno pokreće copper od COP1LC - bez rasprave. */
    if (a->cop.cop1lc != a->dbg_coprs_last) {   /* diagnostika: novo početno mjesto */
        uint32_t k = a->dbg_coprs_n++ & 7;
        a->dbg_coprs_addr[k] = a->cop.cop1lc;
        a->dbg_coprs_frame[k] = (uint32_t)a->frame;
        a->dbg_coprs_last = a->cop.cop1lc;
    }
    aga_dbg_event(a, 8, (uint16_t)a->cop.cop1lc, 0);
    a->cop.strobe = 0;
    /* Restart je strobe: COP1LC se čita kad copper sljedeći put DOHVATI,
       ne sada (WinUAE COPJMP(1, true) samo postavlja startstrobe; pokazivač
       dolazi iz getstrobecopip() kad se dogodi prvi copper DMA ciklus). S
       uključenim copper DMA to je dva colour clocka kasnije i ništa ne mijenja.
       S isključenim, dohvaćanje čeka da se DMA vrati - a dotad je program
       možda usmjerio COP1LC negdje drugdje. OutRun AGA (na razini sustava)
       isključi sav DMA, upiše BPLCON0 $0211, usmjeri COP1LC na svoju listu i
       ponovno uključi DMA. Zadržavanje pokazivača ovdje sačuvalo je OS-ov
       copinit od prethodnog vertikalnog blanka, a copinitov drugi MOVE
       (BPLCON0 $0200) izvršio se čim se DMA vratio: nijedan bitplane nikad,
       samo copper cesta i nebo. */
    a->cop.pc = a->cop.cop1lc;
    a->cop.reload = 1;
    a->cop.state = COP_read1;
    a->cop.ignore_next = 0;
    a->cop.delay = 2;
}

void cop_start_line(aga_t *a)
{
    /* Prvi ciklus koji redak nudi copperu je 2, ne 0: WinUAE DMA zapisi
       pokazuju copper pristupe na 2,4,..,226 (113 po retku), nikad na 0.
       Početak od 0 davao je copperu jedan pristup više po retku, što se
       očitovalo kao pomak od jedne instrukcijske riječi po retku kad god dugi
       copper niz nije bio prekinut WAIT-om (vidi docs/VALIDATION-FINDINGS.md). */
    a->cop.last_hpos = 1;
}

void cop_blitter_done(aga_t *a)
{
    if (a->cop.state == COP_bltwait) a->cop.state = COP_read1;
}

static int cop_reg_allowed(const aga_t *a, uint32_t reg)
{
    if (a->cop.danger) return 1;              /* AGA + CDANG: sve */
    return reg >= 0x80;
}

static inline int beam_reached(const aga_t *a, int hpos)
{
    int vp = a->vpos & a->cop.vmask;
    int hp = hpos & a->cop.hmask;
    if (vp > a->cop.vcmp) return 1;
    if (vp == a->cop.vcmp && hp >= a->cop.hcmp) return 1;
    return 0;
}

/* Gdje se WAIT na ovom retku budi: prvi parni colour clock od c_hpos
   čiji prethodni ciklus dosegne usporedbu (iza kraja retka ako nijedan
   ne dosegne). Do tada svaki copper slot samo uspoređuje zraku i nalazi je
   prekratkom; cck_step broji te slotove kao mirovanje umjesto da za svaki
   zove cop_sync, kao što čini za WAIT na kasnijem retku. Ništa u međuvremenu
   ne pomiče copper osim strobea ili novog WAIT-a, koji napuštaju COP_wait ili
   zaboravljaju buđenje; to vrijedi samo za ovaj prolaz ovog retka (line_no se
   nikad ne ponavlja, a zraka vraćena prije wake_from ponovno ga nađe).
   RGB2RTG 0.75 (istog autora): Jim Powerov copper čeka mjesto na gotovo
   svakom retku. */
static void cop_wait_wake(aga_t *a, int c_hpos)
{
    copper_t *c = &a->cop;
    int h = c_hpos;
    while (h <= a->maxhpos + 1 && !(h - 2 >= 0 && beam_reached(a, h - 2)))
        h += 2;
    c->wake_ok = 1;
    c->wake_seq = a->line_no;
    c->wake_from = c_hpos;
    c->wake_h = h;
}

static void decode_compare(aga_t *a)
{
    copper_t *c = &a->cop;
    c->vcmp = (c->i1 & (c->i2 | 0x8000)) >> 8;
    c->hcmp = c->i1 & c->i2 & 0xFE;
    c->vmask = ((c->i2 >> 8) & 0x7F) | 0x80;
    c->hmask = c->i2 & 0xFE;
    c->blitwait = (c->i2 & 0x8000) ? 0 : 1;
}

/* Pomakni copper do colour clocka `until` (isključivo).
 *
 * Model vremena (kalibriran prema WinUAE DMA zapisima): copper koristi parne
 * cikluse. Čitanje planirano za ciklus c dogodi se samo ako je ciklus c sam
 * slobodan od bitplane DMA, a WAIT je zadovoljen kad je dosegnut položaj zrake
 * c-2; prvo čitanje tada se dogodi na sljedećem slobodnom parnom ciklusu.
 * Ništa od ovoga nije bilo očito. */
void cop_sync(aga_t *a, int until)
{
    copper_t *c = &a->cop;
    int c_hpos = c->last_hpos;

    if (until > a->maxhpos) until = a->maxhpos;
    if (c_hpos >= until) return;

    if (!aga_dma_enabled(a, DMAF_COPEN) || c->state == COP_stop) {
        c->last_hpos = until;
        return;
    }
    if (c->state == COP_wait && (a->vpos & c->vmask) < c->vcmp) {
        c->last_hpos = until;
        return;
    }
    if (c->state == COP_bltwait) {
        if (a->blt.busy) { c->last_hpos = until; return; }
        c->state = COP_read1;
    }

    c_hpos = (c_hpos + 1) & ~1;
    while (c_hpos < until) {
        int prev = c_hpos - 2;
        if (c->delay > 0) {
            c->delay--;
            c_hpos += 2;
            continue;
        }
        switch (c->state) {
        case COP_strobe_delay:
            c->state = COP_read1;
            break;
        case COP_read1:
            if (pf_is_bpl_slot(a, c_hpos)) break;                 /* slot zauzeo bitplane DMA */
            if (c->reload) {                                      /* pokazivač strobea, pročitan sada */
                c->pc = (c->reload == 1) ? c->cop1lc : c->cop2lc;
                c->reload = 0;
            }
            aga_trace_at(a, c_hpos, AGA_DMA_COPPER, 0x8C, c->pc);
            c->i1 = chip_rd16(a, c->pc);
            c->pc += 2;
            c->state = COP_read2;
            break;
        case COP_read2:
            if (pf_is_bpl_slot(a, c_hpos)) break;
            aga_trace_at(a, c_hpos, AGA_DMA_COPPER,
                         (uint16_t)((c->i1 & 1) ? 0x8C : (c->i1 & 0x1FE)), c->pc);
            c->i2 = chip_rd16(a, c->pc);
            c->pc += 2;
            if (c->ignore_next) {
                c->ignore_next = 0;
                if (c->strobe) {
                    /* odbačena instrukcija nakon COPJMP-a koji je upisao copper */
                    c->pc = (c->strobe == 1) ? c->cop1lc : c->cop2lc;
                    c->strobe = 0;
                    c->reload = 0;
                }
                c->state = COP_read1;
                break;
            }
            if (c->i1 & 1) {
                c->state = (c->i2 & 1) ? COP_skip_in2 : COP_wait_in2;
            } else {
                uint32_t reg = c->i1 & 0x1FE;
                if (cop_reg_allowed(a, reg)) {
                    c->in_move = 1;
                    if (reg == 0x100) {             /* diagnostika: copperovi vlastiti upisi BPLCON0 */
                        a->dbg_cop_bc0_n++;
                        a->dbg_cop_bc0_pc = c->pc - 4;
                        a->dbg_cop_bc0_val = c->i2;
                        a->dbg_cop_bc0_frame = (uint32_t)a->frame;
                        aga_dbg_event(a, 7, c->i2, 0);
                    }
                    if (reg == 0x088 || reg == 0x08A) aga_dbg_event(a, 9, (uint16_t)reg, 0);
                    aga_custom_wput(a, reg, c->i2);
                    c->in_move = 0;
                    c->state = COP_read1;
                } else {
                    c->state = COP_stop;   /* nedopušten pristup: staje do sljedećeg strobea/vblanka, kvragu */
                    c->last_hpos = until;
                    return;
                }
            }
            break;
        case COP_wait_in2:
            decode_compare(a);
            if (c->i1 == 0xFFFF && c->i2 == 0xFFFE) {
                c->state = COP_stop;
                c->last_hpos = until;
                return;
            }
            c->state = COP_wait;
            c->wake_ok = 0;
            if ((a->vpos & c->vmask) < c->vcmp) {
                c->last_hpos = until;
                return;
            }
            /* Usporedba se događa na SLJEDEĆEM copper ciklusu, ne ovome, a
               taj ciklus može biti na sljedećem retku - s brojačem redaka koji
               nosi. Alien Breed: Tower Assault čeka preko retka 255 na uobičajen
               način: WAIT line $FF, zatim WAIT line $20, što znači redak $120
               nakon što brojač prijeđe 255. Usporedba u istom ciklusu, dok
               redak još čita $FF, učinila je "line $20" istinitim odmah ($FF je
               iza $20): modulosi za donju ploču upisani su 32 retka prerano i
               svaki redak odatle čitao je s pogrešne adrese - pojas šuma iznad
               donje ploče. WinUAE zapisi pokazuju da čita te dvije riječi na
               kraju retka $FF, a sljedeću instrukciju tek na retku $120. */
            break;
        case COP_wait:
            /* a ni buđenja u njemu: WinUAE copper ostavlja WAIT u ciklusu koji
               može iskoristiti, pa nakon WAIT h=$40 čita sljedeći MOVE na
               $48, gdje sam čitao na $44 ($40 je prekratak, $44 je
               buđenje). Prečac buđenja (custom.c cck_step, wake_h) ostaje donja
               granica. Shadow of the Beast postavlja COLOR00 iz WAIT-a na svakom
               rubu prikaznog okna u svakom retku, a njegov pojas neba izašao je
               8 lores piksela ulijevo (iz RGB2RTG, EMM #2061: copper slotovi
               koji se razlikuju od WinUAE na ovom korpusu - SOTB 221 prema 1,
               boot 51 prema 1, Castlevania t1/t2 59 prema 1, Reshoot title 269
               prema 133, nijedan gori). */
            if (pf_is_bpl_slot(a, c_hpos)) break;
            if (prev >= 0 && beam_reached(a, prev)) {
                if (c->blitwait && a->blt.busy) {
                    c->state = COP_bltwait;
                    c->last_hpos = until;
                    return;
                }
                c->state = COP_read1;
                c->wake_ok = 0;
                /* WAIT koji je već zadovoljen kad redak počinje budi copper jedan
                   slot kasnije nego usporedba usred retka: WinUAE tada prvo
                   čita u ciklusu 6 (Castlevania retci 52/57, Raptor redak 34,
                   agatest redak 10), dok buđenje usred retka čita na sljedećem
                   slobodnom parnom ciklusu. */
                if (c_hpos <= 2) c->delay = 1;
            } else if (!c->wake_ok || c->wake_seq != a->line_no || c_hpos < c->wake_from)
                cop_wait_wake(a, c_hpos);
            break;
        case COP_skip_in2:
            decode_compare(a);
            if (prev >= 0 && beam_reached(a, prev) && !(c->blitwait && a->blt.busy))
                c->ignore_next = 1;
            c->state = COP_read1;
            break;
        default:
            c->state = COP_stop;
            break;
        }
        c_hpos += 2;
    }
    c->last_hpos = until;
}
