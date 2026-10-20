/* SPDX-License-Identifier: GPL-2.0-only
 * Fake DWC2 register backend: independent channel, RX FIFO, control phases.
 */
#include "../src/frf_usb/native/frf_dwc2_pio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { HCCHAR=0x500, HCTSIZ=0x510, HCINT=0x508,
       GINTSTS=0x014, GRXSTSP=0x020, GRSTCTL=0x010,
       GNPTXSTS=0x02c,FIFO=0x1000 };
struct mock {
 uint32_t regs[0x1100/4],fifo;
 unsigned phase, fifo_reads, setup_words,rxpending, bytes;
 uint8_t device_desc[8];
};
static uint32_t readreg(void *context,uint32_t off) {
 struct mock *m=context;
 if(off==GRSTCTL) return 0;
 if(off==GNPTXSTS) return 4096;
 if(off==GINTSTS) return m->rxpending ? 16u:0;
 if(off==GRXSTSP) {m->rxpending=0;return (8u<<4)|(2u<<17);}
 if(off==FIFO) {
   uint32_t v=0;
   for(unsigned j=0;j<4;++j) v|=(uint32_t)m->device_desc[4*m->fifo_reads+j]<<(j*8);
   m->fifo_reads++;
   return v;
 }
 if(off==HCINT) return m->rxpending ? 0u : m->regs[off/4];
 return m->regs[off/4];
}
static void writereg(void *context,uint32_t off,uint32_t val) {
 struct mock *m=context;
 if(off==HCINT){m->regs[HCCHAR/4] &= ~(1u<<31);m->regs[off/4]=0;return;}
 if(off==FIFO){m->fifo=val;m->setup_words++;return;}
 if(off==HCCHAR){
   m->regs[off/4]=val;
   if(val&(1u<<31)){
     m->phase++;
     m->regs[HCINT/4]=1;
     if(m->phase==2){ m->rxpending=1;m->fifo_reads=0; }
   }
   return;
 }
 m->regs[off/4]=val;
}
static void delay(void *ctx,unsigned us) {(void)ctx;(void)us;}
int main(void){
 struct mock m={0};
 const uint8_t dev[8]={18,1,0,2,0,0,0,64};
 memcpy(m.device_desc,dev,sizeof(dev));
 struct frf_dwc2_ops ops={readreg,writereg,delay,&m};
 assert(frf_dwc2_pio_init(&ops)==0);
 struct frf_dwc2_pio_stats st={0};
 uint8_t got[8]={0};
 int rc=frf_dwc2_pio_get_descriptor(&ops,0,8,1,0,got,8,&st);
 assert(rc==8);
 assert(!memcmp(got,dev,8));
 assert(m.phase==3);
 assert(m.setup_words==2);
 assert(st.attempts==3 && st.completed==3 && st.rx_bytes==8);
 struct frf_dwc2_packet invalid={0};
 assert(frf_dwc2_pio_packet(&ops,&invalid,&st)==-1);
 printf("DWC2 PIO root descriptor mock PASS; attempts=%u RX=%u bytes\n",
        st.attempts,st.rx_bytes);
 return 0;
}
