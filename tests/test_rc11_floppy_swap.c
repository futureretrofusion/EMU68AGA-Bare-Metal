/* RC11 staged ADF-change / empty window / index callback regression.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../machine/core/src/internal.h"
static unsigned indexes, index_by_drive[4];
static void index_hook(void *user,int drive)
{
    (void)user;
    ++indexes;
    if(drive>=0 && drive<4)++index_by_drive[drive];
}
void aga_logf(aga_t *a,const char *fmt,...) {(void)a;(void)fmt;}
void aga_intreq_set(aga_t *a,uint16_t bits) {(void)a;(void)bits;}
int aga_dma_enabled(const aga_t *a,uint16_t mask) {(void)a;(void)mask;return 0;}
int main(void) {
    aga_t *a=calloc(1,sizeof(aga_t));
    assert(a);
    a->dsk=calloc(1,sizeof(disk_t));
    assert(a->dsk);
    a->hooks.disk_index=index_hook;
#ifdef FRF_VIRTUAL_AGA
    /* RC12: an EMPTY DF1 is already physically connected. Kickstart
     * probes /RDY drive ID before any ADF has been inserted. */
    assert(aga_disk_active(a));
    dsk_reset(a);
    aga_disk_cia_prb(a,0xef); /* DF1 selected, motor off */
    assert(a->dsk->selected==1);
    uint8_t id_bits=0xffu;
    assert(aga_disk_cia_pra(a,&id_bits));
    assert((id_bits & 0x20u)==0u); /* 3.5" DD drive ID */
    assert((id_bits & 0x04u)==0u); /* /CHNG: no media */
    assert((id_bits & 0x08u)==0u); /* /WPROT: empty */
    assert(!aga_disk_inserted(a,1)); /* drive exists without disk */
    aga_disk_cia_prb(a,0xff); /* reset drive selection */
#endif
    /* Only a few nonzero blocks needed for media identity. */
    uint8_t *disk1=calloc(1,901120),*disk2=calloc(1,901120);
    assert(disk1 && disk2);
    disk1[32]=0x11; disk2[32]=0x22;
    assert(aga_disk_insert(a,0,disk1,901120,1)==0);
    assert(aga_disk_inserted(a,0));
    assert(aga_disk_swap(a,0,disk2,901120,1)==0);
    assert(!aga_disk_inserted(a,0)); /* an observable eject */
    assert(a->dsk->drv[0].pending_image);
    uint32_t lines=a->dsk->drv[0].swap_lines;
    assert(lines>5000 && lines<12000);
    for(uint32_t i=1;i<lines;i++)dsk_line_tick(a);
    assert(!aga_disk_inserted(a,0));
    dsk_line_tick(a);
    assert(aga_disk_inserted(a,0));
    assert(!a->dsk->drv[0].pending_image);
    assert(aga_disk_image(a,0,0,0)[32]==0x22);
    assert(indexes>=2); /* removal + insertion */
    /* Motor and INDEX must keep running in DF0 without active DMA. */
    aga_disk_cia_prb(a,0x77); /* MTR=0, SEL0=0: DF0 running */
    unsigned before=indexes;
    for(int i=0;i<3500;i++)dsk_line_tick(a);
    assert(indexes>before);
    /* DF1 can be selected while DF0 remains spinning. Both rotate and
     * the selected drive is used for Paula DMA input. */
    assert(aga_disk_insert(a,1,disk1,901120,1)==0);
#ifdef FRF_VIRTUAL_AGA
    /* Media inserted later must be available to the already-present DF1. */
    dsk_reset(a);
    aga_disk_cia_prb(a,0x6f); /* select DF1, motor starts */
    uint8_t ready=0xffu;
    assert(a->dsk->selected==1 && a->dsk->drv[1].motor);
    assert(aga_disk_cia_pra(a,&ready));
    assert((ready & 0x20u)==0); /* /RDY low on spinning inserted DF1 */
    dsk_reset(a); /* restore for old two-motor rotation regression */
    aga_disk_cia_prb(a,0x77); /* DF0 motor on */
#endif
    aga_disk_cia_prb(a,0xff); /* deselect, DF0 keeps spinning */
    aga_disk_cia_prb(a,0x6f); /* MTR=0 SEL1=0, latch DF1 motor */
    assert(a->dsk->selected==1);
    assert(a->dsk->drv[0].motor && a->dsk->drv[1].motor);
    before=indexes;
    unsigned df0_idx=index_by_drive[0],df1_idx=index_by_drive[1];
    for(int i=0;i<3500;i++)dsk_line_tick(a);
    assert(indexes>before);
    assert(index_by_drive[1]>df1_idx);
    assert(index_by_drive[0]==df0_idx); /* spinning but unselected */
    /* Eject while another replacement is pending prevents ghost reinsertion. */
    assert(aga_disk_swap(a,0,disk1,901120,1)==0);
    aga_disk_eject(a,0);
    for(int i=0;i<10000;i++)dsk_line_tick(a);
    assert(!aga_disk_inserted(a,0));
    /* Dirty image must not be overwritten (no persistent writeback yet). */
    assert(aga_disk_insert(a,0,disk2,901120,0)==0);
    a->dsk->drv[0].dirty=1;
    assert(aga_disk_swap(a,0,disk1,901120,1)==-4);
    assert(aga_disk_image(a,0,0,0)[32]==0x22);
    dsk_destroy(a);free(a);free(disk1);free(disk2);
    puts("PASS: ADF swap empty interval, INDEX, no ghost media, dirty protection");
    return 0;
}
