/* SPDX-License-Identifier: GPL-2.0-or-later
 * FRF Pi3A+ ARM SDIO hardware adapter. LIMITED IDENTIFICATION PHASE.
 *
 * Real MMIO CMD0/5/3/7/52; strict 3A+ DT check; bounded hardware polling.
 * Does not transfer firmware, associate with an AP or enable IP networking.
 * Never touches boot SDHOST (0x7E202000). WiFi host = 0x7E300000.
 *
 * Execute only in experimental opted-in firmware; do not concurrently
 * initialize WiFiPi.device, since this must own physical SD1.
 */
#include <stdint.h>
#include <string.h>
#include <devicetree.h>
#include <support.h>
#include <support_rpi.h>
#include "frf_sdio_probe.h"
#include "frf_net_bridge.h"

#define SD1_BASE ((uintptr_t)0xF2300000u)
#define SD1_ARG1 0x08u
#define SD1_CMDTM 0x0cu
#define SD1_RESP0 0x10u
#define SD1_STATUS 0x24u
#define SD1_CONTROL0 0x28u
#define SD1_CONTROL1 0x2cu
#define SD1_IRQ 0x30u
#define SD1_IRQMASK 0x34u
#define SD1_IRQEN 0x38u
#define SD1_SLOTVERSION 0xFCu
#define CMD_INHIBIT (1u<<0)
#define SD1_CMD_DONE (1u<<0)
#define SD1_CMD_ERR  (1u<<15)
#define SD1_CMD_ERRBITS 0xFFFF0000u
#define SD1_RESP48 (2u<<16)
#define SD1_CMD_CRC (1u<<19)
#define SD1_CMD_IDX (1u<<20)
#define SD1_RESET_ALL (1u<<24)
#define SD1_CLK_INT (1u<<0)
#define SD1_CLK_STABLE (1u<<1)
#define SD1_CLK_SD (1u<<2)
#define SD1_TIMEOUT_MS 150u

static uint32_t rd(uint32_t off){return LE32(*(volatile uint32_t*)(SD1_BASE+off));}
static void wr(uint32_t off,uint32_t x){*(volatile uint32_t*)(SD1_BASE+off)=LE32(x);__asm__ volatile("dsb sy" ::: "memory");}
static uint64_t tick(void){uint64_t t;__asm__ volatile("mrs %0,cntvct_el0":"=r"(t));return t;}
static uint64_t freq(void){uint64_t t;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(t));return t;}
static uint64_t deadline(unsigned ms){return tick()+(freq()*(uint64_t)ms)/1000u;}
static int expired(uint64_t end){return (int64_t)(tick()-end)>=0;}
static void pause_ms(void*unused,unsigned ms){
    (void)unused;
    uint64_t end=deadline(ms);
    while(!expired(end))__asm__ volatile("yield");
}
static int power(void*ctx){
    (void)ctx;
    uint32_t ver=rd(SD1_SLOTVERSION);
    if(ver==0 || ver==0xffffffffu){kprintf("[FRFNET] SD1 inaccessible ver=%08x\n",ver);return -1;}
    kprintf("[FRFNET] SD1 Arasan slot=%08x, status=%08x\n",ver,rd(SD1_STATUS));
    wr(SD1_CONTROL1,SD1_RESET_ALL);
    uint64_t end=deadline(250);
    while(rd(SD1_CONTROL1)&SD1_RESET_ALL)
        if(expired(end)){kprintf("[FRFNET] SD1 host reset timeout\n");return -1;}
    /* Polling mode: controller generates status, not ARM IRQs yet. */
    wr(SD1_IRQEN,0);
    wr(SD1_IRQMASK,0xffffffffu);
    wr(SD1_IRQ,0xffffffffu);
    return 0;
}
static int clock_khz(void*ctx,unsigned khz){
    (void)ctx;
    if(khz!=400)return -1;/* only identification stage */
    uint32_t src=get_clock_rate(1u);
    if(src<1000000 || src>500000000u){kprintf("[FRFNET] SD1 clock invalid %u\n",src);return -1;}
    uint32_t desired=khz*1000u;
    uint32_t div=(src+2u*desired-1u)/(2u*desired);
    if(!div)div=1;
    if(div>0x3ffu)return -1;
    uint32_t enc=((div&0xffu)<<8)|((div&0x300u)>>2);
    uint32_t c1=(14u<<16)|SD1_CLK_INT|enc;
    wr(SD1_CONTROL1,c1);
    uint64_t end=deadline(250);
    while(!(rd(SD1_CONTROL1)&SD1_CLK_STABLE))
        if(expired(end)){kprintf("[FRFNET] SD1 clock unstable\n");return -1;}
    wr(SD1_CONTROL1,c1|SD1_CLK_SD);
    pause_ms(0,5);
    kprintf("[FRFNET] SD1 identify clock nominal=%u kHz (source=%u)\n",khz,src);
    return 0;
}
static int command(void*ctx,unsigned n,uint32_t arg,
                   frf_sdio_response type,uint32_t*resp){
    (void)ctx;
    uint64_t end=deadline(SD1_TIMEOUT_MS);
    while(rd(SD1_STATUS)&CMD_INHIBIT)
        if(expired(end))return -1;
    wr(SD1_IRQ,SD1_CMD_DONE|SD1_CMD_ERR|SD1_CMD_ERRBITS);
    uint32_t cmd=n<<24;
    if(type!=FRF_SDIO_RESP_NONE)cmd|=SD1_RESP48;
    if(type==FRF_SDIO_RESP_R1||type==FRF_SDIO_RESP_R5||
       type==FRF_SDIO_RESP_R6)cmd|=SD1_CMD_CRC|SD1_CMD_IDX;
    wr(SD1_ARG1,arg);
    wr(SD1_CMDTM,cmd);
    end=deadline(SD1_TIMEOUT_MS);
    uint32_t s;
    do {
        s=rd(SD1_IRQ);
        if(s&(SD1_CMD_DONE|SD1_CMD_ERR|SD1_CMD_ERRBITS))break;
    }while(!expired(end));
    if(!(s&SD1_CMD_DONE)|| (s&(SD1_CMD_ERR|SD1_CMD_ERRBITS))){
        kprintf("[FRFNET] SD1 CMD%u failure irq=%08x arg=%08x\n",n,s,arg);
        wr(SD1_IRQ,s);
        return -1;
    }
    if(resp)*resp=type==FRF_SDIO_RESP_NONE?0:rd(SD1_RESP0);
    wr(SD1_IRQ,SD1_CMD_DONE);
    return 0;
}
static int pi3a(void){
    of_node_t *root=dt_find_node("/");
    of_property_t *p=root?dt_find_property(root,"compatible"):NULL;
    const char*want="raspberrypi,3-model-a-plus";
    if(!p||!p->op_value)return 0;
    const char*c=p->op_value;
    unsigned at=0;
    while(at<p->op_length){
        unsigned end=at;
        while(end<p->op_length && c[end])++end;
        if(end-at==strlen(want)&&!memcmp(c+at,want,end-at))return 1;
        at=end+1u;
    }
    return 0;
}
void frf_wifi_sd1_identify(void){
    if(!pi3a()){kprintf("[FRFNET] Not Pi 3A+; SD1 writes SKIPPED\n");return;}
    of_node_t *sd1=dt_find_node("/soc/mmcnr@7e300000");
    of_property_t *status=sd1?dt_find_property(sd1,"status"):NULL;
    if(!sd1 || (status && status->op_value &&
       strcmp((char*)status->op_value,"okay")!=0)){
        kprintf("[FRFNET] DT SD1 host unavailable; no writes\n");return;
    }
    const frf_sdio_host h={0,power,clock_khz,command,pause_ms};
    frf_sdio_probe_info info;
    const frf_sdio_status rc=frf_sdio_probe(&h,0x00300000u,&info);
    kprintf("[FRFNET] SD1 identification: %s (status=%u)\n",
           frf_sdio_status_text(rc),(unsigned)rc);
    if(rc==FRF_SDIO_OK)
        kprintf("[FRFNET] SDIO fn=%u RCA=%04x CCCR=%02x ready=%02x. RADIO FIRMWARE NOT LOADED.\n",
                (unsigned)info.function_count,(unsigned)info.rca,
                (unsigned)info.cccr_sdio_version,(unsigned)info.cccr_io_ready);
}
