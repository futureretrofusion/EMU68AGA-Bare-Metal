/* SPDX-License-Identifier: GPL-2.0-only
 * Native Synopsys DWC2 slave/PIO USB host channel transport.
 * No USPi code or headers. One channel, one packet, synchronous bounded polls.
 * Supports control transactions to DIRECT root-port devices, no hubs yet.
 */
#include "frf_dwc2_pio.h"
#include <stddef.h>

enum {
 GRSTCTL=0x010, GINTSTS=0x014, GRXSTSP=0x020, GRXFSIZ=0x024,
 GNPTXFSIZ=0x028, GNPTXSTS=0x02c,
 HCFG=0x400, HPTXFSIZ=0x100, HAINTMSK=0x418,
 HCCHAR0=0x500, HCSPLT0=0x504, HCINT0=0x508,
 HCINTMSK0=0x50c, HCTSIZ0=0x510, FIFO0=0x1000
};
#define B(n) (1u<<(n))
static uint32_t rd(const struct frf_dwc2_ops *o,uint32_t r)
{ return o->read(o->ctx,r); }
static void wr(const struct frf_dwc2_ops *o,uint32_t r,uint32_t v)
{ o->write(o->ctx,r,v); }
static int wait_clear(const struct frf_dwc2_ops *o,uint32_t r,uint32_t mask)
{
 for (unsigned i=0;i<10000;i++) {
    if (!(rd(o,r)&mask)) return 0;
    o->delay_us(o->ctx,10);
 }
 return -1;
}
int frf_dwc2_pio_init(const struct frf_dwc2_ops *o)
{
 if (!o||!o->read||!o->write||!o->delay_us) return -1;
 /* Program non-overlapping FIFOs for high-speed host control traffic.
  * 512 + 256 + 512 words fits BCM2837's >=4096 word FIFO.
  * Keep global USB interrupt masking unchanged: this backend polls.
  */
 wr(o,GRXFSIZ,512);
 wr(o,GNPTXFSIZ,(256u<<16)|512u);
 wr(o,HPTXFSIZ,(512u<<16)|768u);
 /* 30/60 MHz host PHY clock select for HS PHY, preserve other bits. */
 wr(o,HCFG,rd(o,HCFG)&~3u);
 wr(o,GRSTCTL,B(4));
 if (wait_clear(o,GRSTCTL,B(4))) return -2;
 wr(o,GRSTCTL,B(5)|(0x10u<<6));
 if (wait_clear(o,GRSTCTL,B(5))) return -3;
 wr(o,HAINTMSK,1u);
 wr(o,HCINTMSK0,0u);
 wr(o,HCINT0,0xffffffffu);
 return 0;
}
/* FIFO words are little-endian protocol data, independent of ARM BE mode.
 * The MMIO callback converts register WORD endian only. */
static void fifo_write(const struct frf_dwc2_ops *o,const uint8_t *bytes,unsigned n)
{
 for(unsigned i=0;i<n;i+=4u) {
    uint32_t word=0;
    for(unsigned j=0;j<4 && i+j<n;j++)
        word|=(uint32_t)bytes[i+j]<<(j*8u);
    wr(o,FIFO0,word);
 }
}
static void fifo_read(const struct frf_dwc2_ops *o,uint8_t *bytes,
                      unsigned valid,unsigned capacity)
{
 for(unsigned i=0;i<valid;i+=4u) {
    uint32_t word=rd(o,FIFO0);
    for(unsigned j=0;j<4 && i+j<valid;j++)
        if(i+j<capacity && bytes) bytes[i+j]=(uint8_t)(word>>(j*8u));
 }
}
int frf_dwc2_pio_packet(const struct frf_dwc2_ops *o,
                       struct frf_dwc2_packet *p,
                       struct frf_dwc2_pio_stats *st)
{
 if(!o||!o->read||!o->write||!o->delay_us||!p||
    p->address>127||p->endpoint>15||p->max_packet==0||
    p->max_packet>64||p->length>p->max_packet||
    (p->length && !p->buffer) || p->type>3) return -1;
 if(st) st->attempts++;
 p->received=0;
 /* A genuine single-packet transfer, MAX 64 bytes. USB2 high-bandwidth
  * interrupts and split transactions are not supported in this stage. */
 wr(o,HCCHAR0,B(30)); /* disable a previously active channel */
 for(unsigned i=0;i<1000u && (rd(o,HCCHAR0)&B(31));++i)
    o->delay_us(o->ctx,10);
 if(rd(o,HCCHAR0)&B(31)) return -2;
 wr(o,HCINTMSK0,0u);
 wr(o,HCINT0,0xffffffffu);
 wr(o,HCSPLT0,0u);
 uint32_t charval=((uint32_t)p->address<<22)|
                  ((uint32_t)p->type<<18)|
                  ((uint32_t)!!p->low_speed<<17)|
                  ((uint32_t)!!p->in<<15)|
                  ((uint32_t)p->endpoint<<11)|p->max_packet;
 uint32_t size=(uint32_t)p->length|(1u<<19)|((uint32_t)p->pid<<29);
 wr(o,HCTSIZ0,size);
 if(!p->in && p->length) {
    /* TX FIFO must have enough words before enabling this channel. */
    unsigned words=(p->length+3u)/4u;
    unsigned i=0;
    while ((rd(o,GNPTXSTS)&0xffffu)<words && i++<10000)
        o->delay_us(o->ctx,10);
    if(i>10000) return -3;
 }
 wr(o,HCCHAR0,charval|B(31));
 if(!p->in && p->length) fifo_write(o,p->buffer,p->length);
 unsigned rxcount=0;
 for(unsigned step=0;step<10000;step++) {
    uint32_t gint=rd(o,GINTSTS);
    if(gint&B(4)) {
        uint32_t status=rd(o,GRXSTSP);
        unsigned channel=status&15u;
        unsigned count=(status>>4)&0x7ffu;
        unsigned kind=(status>>17)&15u;
        if(kind==2u) {
            /* Even unexpected channel data must be consumed. */
            if(channel==0) {
                unsigned remain=p->length>rxcount?p->length-rxcount:0u;
                fifo_read(o,p->buffer? p->buffer+rxcount:NULL,count,remain);
                rxcount+=(count<remain?count:remain);
            } else fifo_read(o,NULL,count,0);
        }
    }
    uint32_t hcint=rd(o,HCINT0);
    if(hcint&B(3)) { /* STALL */
        wr(o,HCINT0,hcint);
        if(st) st->stalls++;
        return -4;
    }
    if(hcint&B(4)) { /* NAK; caller may retry on appropriate schedule */
        wr(o,HCINT0,hcint);
        if(st) st->naks++;
        return -5;
    }
    if(hcint&(B(7)|B(8)|B(9)|B(10))) { /* transfer/data errors */
        wr(o,HCINT0,hcint);
        if(st) st->errors++;
        return -6;
    }
    if(hcint&B(0)) { /* transfer complete */
        wr(o,HCINT0,hcint);
        p->received=(uint16_t)rxcount;
        if(st) { st->completed++;st->rx_bytes+=rxcount; }
        return 0;
    }
    o->delay_us(o->ctx,50);
 }
 if(st) st->timeouts++;
 wr(o,HCCHAR0,rd(o,HCCHAR0)|B(30)|B(31));
 return -7;
}
int frf_dwc2_pio_get_descriptor(const struct frf_dwc2_ops *o,
                                uint8_t addr,uint8_t mps,
                                uint8_t type,uint8_t index,
                                uint8_t *buffer,uint16_t len,
                                struct frf_dwc2_pio_stats *st)
{
 if(!buffer||!len||len>1024||mps==0||mps>64) return -1;
 uint8_t setup[8]={0x80,0x06,index,type,0,0,(uint8_t)len,(uint8_t)(len>>8)};
 struct frf_dwc2_packet p={addr,0,0,0,0,FRF_USB_PID_SETUP,mps,8,0,setup};
 int rc=frf_dwc2_pio_packet(o,&p,st);
 if(rc) return rc;
 uint16_t total=0;uint8_t pid=FRF_USB_PID_DATA1;
 while(total<len) {
    uint16_t n=(len-total<mps)?len-total:mps;
    p=(struct frf_dwc2_packet){addr,0,1,0,0,pid,mps,n,0,buffer+total};
    rc=frf_dwc2_pio_packet(o,&p,st);
    if(rc) return rc;
    total+=p.received;
    if(p.received<n) break;
    pid=(pid==FRF_USB_PID_DATA1)?FRF_USB_PID_DATA0:FRF_USB_PID_DATA1;
 }
 /* Control status OUT ZLP always DATA1. */
 p=(struct frf_dwc2_packet){addr,0,0,0,0,FRF_USB_PID_DATA1,mps,0,0,NULL};
 rc=frf_dwc2_pio_packet(o,&p,st);
 return rc?rc:(int)total;
}
