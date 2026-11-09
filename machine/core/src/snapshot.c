/* AGA-STORM — učitavanje izvana snimljenog stanja chipseta (snimke profila WinUAE).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "internal.h"

void aga_set_color(aga_t *a, int index, uint32_t rgb)
{
    a->color[index & 255] = rgb & 0xFFFFFF;
    a->color_aga[index & 255] = rgb & 0xFFFFFF;   /* također AGA sjena ECS načina */
    a->bright_dirty = 1;            /* posvijetljena paleta, na sljedećem retku */
}

static int is_strobe_or_readonly(uint32_t reg)
{
    if (reg < 0x020) return 1;                       /* blok samo za čitanje - tu se ne piše */
    switch (reg) {
    case COPJMP1: case COPJMP2: case COPINS: case BLTSIZE: case BLTSIZH:
    case STREQU: case STRVBL: case STRHOR: case STRLONG:
    case DSKPTH: case DSKPTL: case DSKLEN: case DSKDAT: case DSKSYNC:
    case SERDAT: case SERPER: case POTGO: case JOYTEST: case REFPTR:
    case VPOSW: case VHPOSW: case HHPOSW:
    case DENISEID: case NOOP:
        return 1;
    default:
        return 0;
    }
}

void aga_load_registers(aga_t *a, const uint16_t *regs)
{
    /* redoslijed je važan: FMODE/BPLCON3 prije boja i spriteova; SET/CLR
       registri apsolutni. Ne dirajte redoslijed. Stvarno, jebote. */
    a->hpos = 0; a->vpos = 0;
    /* redak 0 kratak, kao nakon reset-a (NTSC: faza dugih/kratkih redaka,
       dakako) */
    a->maxhpos = aga_short_line(a);
    a->lol = 0;
    /* BLTBDAT below is shifted into B's hold through the B shifter's previous
       word: that starts at 0, as when a blit starts, not with whatever the
       core held before */
    a->blt.bold = 0;
    aga_custom_wput(a, FMODE, regs[FMODE >> 1]);
    aga_custom_wput(a, BPLCON0, regs[BPLCON0 >> 1]);
    aga_custom_wput(a, BPLCON3, regs[BPLCON3 >> 1]);
    for (uint32_t reg = 0x020; reg < 0x200; reg += 2) {
        if (is_strobe_or_readonly(reg)) continue;
        if (reg >= COLOR00 && reg < COLOR00 + 64) continue;     /* paleta dolazi iz agaColors */
        if (reg == DMACON || reg == INTENA || reg == INTREQ || reg == ADKCON) continue;
        if (reg >= SPR0POS && reg < SPR0POS + 64 && (reg & 6) == 4) continue; /* SPRxDATA: aktivira sprite; preskoči */
        aga_custom_wput(a, reg, regs[reg >> 1]);
    }
    /* sprite DATA/DATB upisani nakon CTL-a da bi se aktivirali kao u snimci */
    for (int n = 0; n < 8; n++) {
        aga_custom_wput(a, SPR0POS + n * 8 + 6, regs[(SPR0POS + n * 8 + 6) >> 1]);
        if (regs[(SPR0POS + n * 8 + 4) >> 1] || regs[(SPR0POS + n * 8 + 6) >> 1])
            aga_custom_wput(a, SPR0POS + n * 8 + 4, regs[(SPR0POS + n * 8 + 4) >> 1]);
    }
    a->dmacon = regs[DMACON >> 1] & 0x07FF;
    a->intena = regs[INTENA >> 1] & 0x7FFF;
    a->intreq = regs[INTREQ >> 1] & 0x7FFF;
    a->adkcon = regs[ADKCON >> 1] & 0x7FFF;
    aud_dmacon_changed(a, 0, a->dmacon);
    aga_update_ipl(a);
    a->cop.pc = a->cop.cop1lc;
    a->cop.state = COP_read1;
    a->cop.delay = 2;            /* kao nakon vertikalnog blanka: prvo čitanje u ciklusu 6 */
    a->nchanges = 0;
    pf_start_line(a);
    cop_start_line(a);
}
