/*
 * Compile/run TinyUSB DWC2 host on a software model of the Pi3 MMIO
 * window, including delayed AHB/core reset and HPRT power latch.
 *
 * This is a controller-startup regression test, NOT a USB hardware test.
 */
#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include "tusb.h"
#include "host/hcd.h"
#include "common/tusb_fifo.h"
void tu_hwfifo_write(volatile void *fifo,const uint8_t *b,uint16_t n,const tu_hwfifo_access_t *mode) {
 (void)mode;
 for(uint16_t i=0;i<n;i+=4) {uint32_t word=0;
  for(uint16_t j=0;j<4 && i+j<n;j++)word|=((uint32_t)b[i+j])<<(8*j);
  *(volatile uint32_t*)fifo=word;
 }
}
void tu_hwfifo_read(const volatile void *fifo,uint8_t *b,uint16_t n,const tu_hwfifo_access_t *mode) {
 (void)mode;
 for(uint16_t i=0;i<n;i+=4) {uint32_t word=*(const volatile uint32_t*)fifo;
  for(uint16_t j=0;j<4 && i+j<n;j++)b[i+j]=(uint8_t)(word>>(8*j));
 }
}
#define HW_BASE 0xf2980000UL
#define HW_LEN  0x10000u
#define GUSBCFG 0x00cu
#define GRSTCTL 0x010u
#define GINTSTS 0x014u
#define GSNPSID 0x040u
#define GHWCFG2 0x048u
#define GHWCFG3 0x04cu
#define GHWCFG4 0x050u
#define HPRT    0x440u
#define PCGCCTL 0xe00u
#define AHBIDLE 0x80000000u
/* Host-side stand-in for Pi3 GPU bus translation and cache hooks.
   The ARM integration calls mmu_virt2phys() and uses CPU cache operations. */
static volatile unsigned dma_clean_calls,dma_invalidate_calls,dma_translate_calls;
void arm_flush_cache(uintptr_t p,uint32_t n){(void)p;(void)n;dma_clean_calls++;}
void arm_dcache_invalidate(uintptr_t p,uint32_t n){(void)p;(void)n;dma_invalidate_calls++;}
void* tusb_app_virt_to_phys(void *p) {
    if (!p) return NULL;
    dma_translate_calls++;
    return (void *)(uintptr_t)(0xc0100000u|((uintptr_t)p&0x000fffffu));
}
static volatile uint32_t *regs;
static volatile int running=1;
/* 0 healthy, 1 no root hub, 2 stalled core reset,
   3 stalled TX FIFO, 5 root power not latched,
   6 root port enumeration exercises EP0, 7 invalid vendor ID.
   4 is deliberately not tested: this simple memory-array simulation
   cannot model the GINTSTS CMOD read-only + W1C combination. */
static volatile int test_mode=0;
static volatile unsigned core_resets, tx_flushes, rx_flushes, powered;
static volatile unsigned port_reset_asserted, ep0_setup_armed, port_enabled_seen;
static volatile uint32_t phy_cfg_during_reset;
uint32_t tusb_time_millis_api(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u);
}
static void* fake_controller(void *ctx) {
    (void)ctx;
    while(running) {
        uint32_t gr=regs[GRSTCTL/4];
        if((gr&1u) && test_mode!=2){
            phy_cfg_during_reset=regs[GUSBCFG/4];
            regs[GRSTCTL/4]=AHBIDLE; core_resets++;
        }
        if((gr&(1u<<5)) && test_mode!=3){regs[GRSTCTL/4]=AHBIDLE; tx_flushes++;}
        if(gr&(1u<<4)){regs[GRSTCTL/4]=AHBIDLE; rx_flushes++;}
        if(!(gr&AHBIDLE) && !(gr&1u && test_mode==2) && !(gr&(1u<<5) && test_mode==3)) regs[GRSTCTL/4]=AHBIDLE;
        /* GINTSTS is read-only CMOD 1 and W1C other bits. */
        regs[GINTSTS/4]=test_mode==4?0u:1u;
        /* HPRT read-only connection status and sticky power latch. */
        uint32_t port=regs[HPRT/4];
        if(port & (1u<<12)) powered++;
        if(test_mode==6) {
            if(port & (1u<<8)) port_reset_asserted++;
            else if(port_reset_asserted) port_enabled_seen++;
            if(regs[0x500u/4] & (1u<<31)) ep0_setup_armed++;
        }
        uint32_t pwr=test_mode==5?0u:port&(1u<<12);
        uint32_t extra=test_mode==6?
           ((port&(1u<<8))|((port_reset_asserted && !(port&(1u<<8)))?(1u<<2):0u)):0u;
        regs[HPRT/4] = pwr | (test_mode==1?0u:1u) | extra;
        usleep(30);
    }
    return NULL;
}
int main(int argc,char **argv) {
    if(argc>1) test_mode=atoi(argv[1]);
    void *mapped=mmap((void*)HW_BASE,HW_LEN,PROT_READ|PROT_WRITE,
                      MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0);
    if(mapped==MAP_FAILED){perror("mmap");return 2;}
    assert((uintptr_t)mapped==HW_BASE);
    regs=(volatile uint32_t*)mapped;
    regs[GUSBCFG/4]=(1u<<4)|(1u<<3)|(1u<<20); /* begin in ULPI16+external-VBUS */
    regs[GSNPSID/4]=test_mode==7?0u:0x4f54280au;
    regs[GHWCFG2/4]=(2u<<6)|(2u<<3)|(7u<<14); /* ULPI advertised, DMA arch */
    regs[GHWCFG3/4]=4096u<<16;
    regs[GHWCFG4/4]=0u;
    regs[GRSTCTL/4]=AHBIDLE;
    regs[GINTSTS/4]=test_mode==4?0u:1u;
    regs[HPRT/4]=test_mode==1?0u:1u;
    pthread_t worker;
    assert(pthread_create(&worker,NULL,fake_controller,NULL)==0);
    const tusb_rhport_init_t config={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};
    const uint32_t before=tusb_time_millis_api();
    const bool ok=tuh_rhport_init(0,&config);
    uint32_t elapsed=tusb_time_millis_api()-before;
    const uint32_t phy=regs[GUSBCFG/4];
    const uint32_t port=regs[HPRT/4];
    const uint32_t gahbcfg=regs[0x008u/4];
    printf("HCD init=%u elapsed=%ums PHYCFG=%08x HPRT=%08x PCGCCTL=%08x reset=%u tx=%u rx=%u power_samples=%u PHY_BEFORE_RESET=%08x\n",
           ok,elapsed,phy,port,regs[PCGCCTL/4],core_resets,tx_flushes,rx_flushes,powered,phy_cfg_during_reset);
    const bool expected = test_mode==0 || test_mode==1 || test_mode==6;
    const bool worked=(ok && (port & (1u<<12)) &&
             (phy_cfg_during_reset & (3u<<3))==(3u<<3) &&
             (phy & (1u<<20))==0u &&
             (phy & (1u<<4))==0 &&
             (phy & (1u<<3))==0 && core_resets && tx_flushes && rx_flushes);
    const bool as_expected= test_mode==1?(worked && !hcd_port_connect_status(0)):
                           (expected ? worked : !ok);
    if(ok && !(gahbcfg&(1u<<5))) {
        puts("DMA GAHBCFG.ENABLE missing");
        running=0;pthread_join(worker,NULL);return 1;
    }
    printf("MODE %d expected %s: %s\n",test_mode,
           expected?"init success":"init failure",
           as_expected?"PASS":"FAIL");
    if(!as_expected){
        running=0;pthread_join(worker,NULL);return 1;
    }
    if(test_mode==6 && ok) {
        /* Exercise TinyUSB host enumeration after preconnected root attach.
           The fake device cannot return descriptors, but hardware-level
           root reset and EP0 SETUP channel must become visible. */
        hcd_event_device_attach(0,false);
        uint32_t begin=tusb_time_millis_api();
        while((uint32_t)(tusb_time_millis_api()-begin)<1200u) {
            hcd_int_handler(0,false);
            tuh_task_ext(0,false);
            usleep(100);
        }
        const uint32_t dma_first=regs[(0x500u+0x14u)/4];
        printf("FIRST CONTROL DMA bus=%08x translated=%u clean=%u invalidate=%u\n",
               dma_first,dma_translate_calls,dma_clean_calls,dma_invalidate_calls);
        if((dma_first&0xc0000000u)!=0xc0000000u || !dma_translate_calls ||
           !dma_clean_calls) {
            puts("DWC2 channel DMA address/cache: FAIL");
            running=0;pthread_join(worker,NULL);return 1;
        }
        printf("ROOT HUB simulated enumeration reset=%u enabled=%u EP0_SETUP=%u\n",
                port_reset_asserted,port_enabled_seen,ep0_setup_armed);
        if(!port_reset_asserted || !port_enabled_seen || !ep0_setup_armed) {
            running=0;pthread_join(worker,NULL);
            puts("SIMULATED ROOT ENUMERATION: FAIL");
            return 1;
        }
        puts("SIMULATED ROOT ENUMERATION: PASS (device transfer still unmocked)");
    }
    running=0;pthread_join(worker,NULL);
    return 0;
}
