/* FRF standalone DD floppy drive-ID protocol regression.
 * Test hardware probe: motor off, sample /RDY on each of 32 SEL edges.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../machine/core/src/internal.h"
void aga_logf(aga_t *a,const char *fmt,...){(void)a;(void)fmt;}
void aga_intreq_set(aga_t *a,uint16_t bits){(void)a;(void)bits;}
int aga_dma_enabled(const aga_t *a,uint16_t mask){(void)a;(void)mask;return 0;}

static uint32_t probe_id(aga_t *a,int drive)
{
    uint8_t sel=(uint8_t)(0x08u<<drive);
    uint8_t prb=0xf8u;
    aga_disk_cia_prb(a,prb);         /* motor off, deselect */
    prb &= (uint8_t)0x7fu;        /* motor on */
    aga_disk_cia_prb(a,prb);
    prb &= (uint8_t)~sel;         /* select, latch on */
    aga_disk_cia_prb(a,prb);
    prb |= sel;                   /* deselect */
    aga_disk_cia_prb(a,prb);
    prb |= 0x80u;                 /* motor off */
    aga_disk_cia_prb(a,prb);
    prb &= (uint8_t)~sel;         /* select, latch off */
    aga_disk_cia_prb(a,prb);
    prb |= sel;                   /* deselect */
    aga_disk_cia_prb(a,prb);
    uint32_t result=0;
    for(int n=0;n<32;n++) {
        prb &= (uint8_t)~sel; /* select */
        aga_disk_cia_prb(a,prb);
        uint8_t pra=0x3cu;    /* no virtual drive: no /RDY */
        (void)aga_disk_cia_pra(a,&pra);
        result=(result<<1u)|((pra>>5u)&1u);
        prb |= sel;
        aga_disk_cia_prb(a,prb);
    }
    return result;
}
int main(void) {
    aga_t *a=calloc(1,sizeof(*a));
    assert(a);
    a->dsk=calloc(1,sizeof(disk_t));
    assert(a->dsk);
    dsk_reset(a);
    assert(probe_id(a,0)==0u);
    assert(probe_id(a,1)==0u);
    assert(probe_id(a,2)==0xffffffffu);
    assert(probe_id(a,3)==0xffffffffu);
    uint8_t *adf=calloc(1,901120);
    assert(adf);
    assert(aga_disk_insert(a,1,adf,901120,1)==0);
    assert(probe_id(a,1)==0u);
    dsk_reset(a);
    uint8_t prb=0x6fu; /* select DF1, motor on */
    aga_disk_cia_prb(a,prb);
    assert(a->dsk->selected==1);
    uint8_t bits=0xff;
    assert(aga_disk_cia_pra(a,&bits));
    assert((bits & 0x20u)==0); /* ready for inserted DD */
    aga_disk_eject(a,1);
    dsk_reset(a);
    aga_disk_cia_prb(a,prb);
    assert(aga_disk_cia_pra(a,&bits));
    assert((bits & 0x20u)!=0); /* not ready if spinning and empty */
    assert(probe_id(a,1)==0u); /* drive is still physically present */
    puts("PASS: DF0/DF1 ID 00000000, DF2/DF3 absent FFFFFFFF, DF1 insert/eject readiness");
    free(adf);
    dsk_destroy(a);
    free(a);
    return 0;
}
