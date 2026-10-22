/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "A64.h"
#include "config.h"
#include "support.h"
#include "tlsf.h"
#include "mmu.h"
#include "devicetree.h"
#include "M68k.h"
#include "HunkLoader.h"
#include "DuffCopy.h"
#include "EmuLogo.h"
#include "EmuFeatures.h"
#include "RegisterAllocator.h"
#include "version.h"
#include "logo/logo_emu68.h"
#include "logo/logo_ppc.h"
#include "logo/logo_pistorm.h"
#ifdef AGA_PISTORM
#include "logo/logo_aga.h"      /* "AGA-Pistorm for ECS", tools/mklogo.py */
#include "logo/logo_aga_rgb.h"  /* the colour splash, tools/mklogo_rgb.py */
#endif
#ifdef PISTORM_ANY_MODEL
#include "ps_protocol.h"
#endif

void _start();
void _boot();
void move_kernel(intptr_t from, intptr_t to);
extern uint64_t mmu_user_L1[512];
extern uint64_t mmu_user_L2[4*512];

void M68K_StartEmu(void *addr);

void *framebuffer;
uint32_t pitch;
uint32_t fb_width;
uint32_t fb_height;

extern const uint32_t topaz8_charloc[];
extern const uint8_t topaz8_chardata[];

extern void *firmware_file;
extern uint32_t firmware_size;

uint32_t text_x = 0;
uint32_t text_y = 0;
const int modulo = 192;
int purple = 0;
int black = 0;

void put_char(uint8_t c)
{
    if (framebuffer && pitch)
    {
        uint16_t *pos_in_image = (uint16_t*)((uintptr_t)framebuffer + (text_y * 16 + 5)* pitch);
        pos_in_image += 4 + text_x * 8;

        if (c == 10) {
            text_x = 0;
            text_y++;
        }
        else if (c >= 32) {
            uint32_t loc = (topaz8_charloc[c - 32] >> 16) >> 3;
            const uint8_t *data = &topaz8_chardata[loc];

            for (int y = 0; y < 16; y++) {
                const uint8_t byte = *data;

                for (int x=0; x < 8; x++) {
                    if (byte & (0x80 >> x)) {
#ifdef AGA_PISTORM
                        /* logo=black's grey: AGA-PISTORM's splash is always
                           on black (below), so black text would vanish */
                        pos_in_image[x] = LE16(0x630c);
#else
                        if (purple) {
                            pos_in_image[x] = LE16(0xed51);
                        }
                        else if (black) {
                            pos_in_image[x] = LE16(0x630c);
                        }
                        else {
                            pos_in_image[x] = 0;
                        }
#endif
                    }
                }

                if (y & 1)
                    data += modulo;
                pos_in_image += pitch / 2;
            }
            text_x++;
        }
    }
}

static void __putc(void *data, char c)
{
    (void)data;
    put_char(c);
}

struct EmuLogo *rle_decode(uint8_t *logo_rle, uint32_t length)
{
    uint32_t w = 0;
    uint32_t h = 0;

    while (1) {
        w = (w << 7) | (*logo_rle & 0x7f);
        if ((*logo_rle++ & 0x80) == 0)
            break;
    }

    while (1) {
        h = (h << 7) | (*logo_rle & 0x7f);
        if ((*logo_rle++ & 0x80) == 0)
            break;
    }

    struct EmuLogo *logo = (struct EmuLogo *)tlsf_malloc(tlsf, sizeof(struct EmuLogo) + w * h);
    logo->el_Width = w;
    logo->el_Height = h;
    logo->el_Data = (uint8_t *)((uintptr_t)logo + sizeof(struct EmuLogo));

    uint32_t pos = 0;
    uint32_t outpos = 0;
    while(pos < length) {
        int count = 0;
        int diff = 0;
        unsigned char c = logo_rle[pos++];
        if (c & 0x40) diff = 1;
        count = c & 0x3f;
        while (c & 0x80) {
            count <<= 7;
            c = logo_rle[pos++];
            count |= c & 0x7f;
        }
        if (diff) {
            for (int i = 0; i < count; i++) {
                logo->el_Data[outpos] = logo_rle[pos++];
                outpos++;
            }
        } else {
            uint8_t pix = logo_rle[pos++];

            for (int i=0; i < count; i++) {
                logo->el_Data[outpos] = pix;
                outpos++;
            }
        }
    }

    return logo;
}

void draw_logo(struct EmuLogo *logo, uint32_t start_x, uint32_t start_y)
{
    uint32_t pix_cnt = logo->el_Width * logo->el_Height;
    uint32_t x = 0;
    uint8_t *data = logo->el_Data;
    uint16_t *buff = (uint16_t *)((uintptr_t)framebuffer + pitch*start_y);
    buff += start_x;

    while(pix_cnt > 0) {
        uint8_t gray = *data++;
        uint16_t color;

        if (purple)
        {
            gray = 240 - gray;
            int r=-330,g=-343,b=-91;
            r += (gray * 848) >> 8;
            g += (gray * 768) >> 8;
            b += (gray * 341) >> 8;

            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            color = (b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11);
        }
        else if (black)
        {
            int g = ((120 - (int)gray) * 5) / 4;
            if (g < 0)
                g = 0;
            if (g > 255)
                g = 255;

            color = (g >> 3) | ((g >> 2) << 5) | ((g >> 3) << 11);
        }
        else
        {
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }

        pix_cnt--;
        buff[x++] = LE16(color);
        /* If new line, advance the buffer by pitch and reset x counter */
        if (x >= logo->el_Width) {
            buff += pitch / 2;
            x = 0;
        }
    }
}

#ifdef AGA_PISTORM
/* The AGA-PISTORM splash is a picture, not a tinted greyscale line like
   Emu68's own logos: small palette, runs of indices, its own colours -
   `logo=purple` doesn't touch it. The version number is part of the artwork;
   tools/mklogo_rgb.py bakes it in at release time. */
static void draw_logo_rgb(uint32_t start_x, uint32_t start_y, uint32_t scale)
{
    /* each source pixel becomes a scale x scale block: whole-pixel scaling
       keeps the version's pixel letters sharp, and the kernel only has to
       carry the picture at its small size */
    uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + pitch * start_y) + start_x;
    uint32_t x = 0;
    for (uint32_t i = 0; i + 1 < LOGO_AGA_RGB_LEN; i += 2) {
        uint32_t n = logo_aga_rgb[i];
        uint16_t c = LE16(logo_aga_rgb_pal[logo_aga_rgb[i + 1]]);
        while (n--) {
            for (uint32_t dy = 0; dy < scale; dy++) {
                uint16_t *p = row + dy * (pitch / 2) + x * scale;
                for (uint32_t dx = 0; dx < scale; dx++) p[dx] = c;
            }
            if (++x >= LOGO_AGA_RGB_WIDTH) { row += scale * (pitch / 2); x = 0; }
        }
    }
}
#endif

/* EMU68AGA RC6 pre-boot POST overlay.
 * Replaces the non-descriptive milestone squares. This framebuffer is
 * exclusively a boot splash: never paint again after the 68k handoff.
 * CPU0/CPU1 callbacks may race during boot; a nonblocking lock protects
 * rectangle drawing and keeps USB callbacks responsive.
 */
static volatile uint32_t post_flags;
static volatile uint32_t post_lock, post_stop;
static volatile uint32_t post_kb=~0u, post_ms=~0u, post_gp=~0u, post_storage=~0u;
static volatile uint32_t post_rx, post_bad, post_err, post_id, post_len;
static volatile uint32_t post_raw0, post_raw1, post_joy, post_fire, post_events;
static volatile uint32_t post_decoded;

static void post_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    if (!framebuffer || x>=fb_width || y>=fb_height) return;
    if (w>fb_width-x) w=fb_width-x;
    if (h>fb_height-y) h=fb_height-y;
    c=LE16(c);
    for (uint32_t yy=0;yy<h;yy++) {
        uint16_t *p=(uint16_t*)((uintptr_t)framebuffer+pitch*(y+yy))+x;
        for (uint32_t xx=0;xx<w;xx++) p[xx]=c;
    }
}
static void post_char(uint32_t x,uint32_t y,uint8_t c,uint16_t fg)
{
    if (c<32 || c>=127 || x+8>fb_width || y+16>fb_height) return;
    uint32_t loc=(topaz8_charloc[c-32]>>16)>>3;
    const uint8_t *data=&topaz8_chardata[loc];
    uint16_t color=LE16(fg);
    for (unsigned yy=0;yy<16;yy++) {
        uint8_t bits=*data;
        uint16_t *p=(uint16_t*)((uintptr_t)framebuffer+pitch*(y+yy))+x;
        for (unsigned xx=0;xx<8;xx++)
            if (bits&(0x80u>>xx)) p[xx]=color;
        if (yy&1u) data+=192;
    }
}
static void post_text(uint32_t x,uint32_t y,const char *str,uint16_t fg)
{
    while (*str && x+8<=fb_width) {
        post_char(x,y,(uint8_t)*str++,fg);
        x+=8;
    }
}
static void post_dec(uint32_t x,uint32_t y,uint32_t n,unsigned digits,uint16_t col)
{
    char buf[12];
    if (digits>10) digits=10;
    for (unsigned i=0;i<digits;i++) {
        buf[digits-1-i]='0'+(n%10u);
        n/=10u;
    }
    buf[digits]=0;
    post_text(x,y,buf,col);
}
static void post_hex(uint32_t x,uint32_t y,uint32_t n,unsigned digits,uint16_t col)
{
    const char hex[]="0123456789ABCDEF";
    char buf[9];
    if (digits>8) digits=8;
    for (unsigned i=0;i<digits;i++) {
        buf[digits-1-i]=hex[n&15u];
        n>>=4;
    }
    buf[digits]=0;
    post_text(x,y,buf,col);
}
static void post_row(uint32_t x,uint32_t y,const char *name,
                     const char *state,uint16_t color)
{
    post_text(x,y,name,0xbdf7u);
    post_text(x+152u,y,state,color);
}
#ifdef FRF_USB_PASSIVE_TEST
static volatile uint32_t post_dwc2_id;
static volatile uint32_t post_dwc2_rc = ~0u;
static volatile uint32_t post_key_count;
#endif
#ifdef FRF_USB_NATIVE_DWC2
static volatile uint32_t native_usb_stage, native_usb_rc;
static volatile uint32_t native_usb_detail, native_usb_aux;
static volatile uint32_t native_usb_cpu1_seen, native_usb_cpu1_chip;
static volatile uint32_t native_usb_ahb_config;
static volatile uint32_t native_usb_ep0_hcchar,native_usb_ep0_hctsiz;
static volatile uint32_t native_usb_storage_detected,native_usb_pad_detected;
static volatile uint32_t native_usb_storage_blocks;
static volatile uint32_t native_vendor_id,native_vendor_iface,native_vendor_kind;
static volatile uint32_t native_vendor_mounts;
static volatile uint32_t native_dyonder_ok,native_dyonder_ignored;
/* Per USB address snapshot, not last-vendor wins. Bit flags:
   1=whole device mount, 2=HID, 4=MSC, 8=vendor interface. */
#define FRF_USB_ADDR_MAX 16u
struct frf_device_snapshot {
    volatile uint32_t id,iface,flags;
};
static struct frf_device_snapshot native_usb_addr[FRF_USB_ADDR_MAX];
static volatile uint32_t native_usb_addr_events;
static volatile uint32_t native_usb_link[15];
static volatile uint32_t native_usb_probe[7];
static volatile uint32_t native_usb_packet_words[55];
static volatile uint32_t native_r19_map[6];
static volatile uint32_t native_r20_compat[6];
static volatile uint32_t native_r26_get[4];


#ifdef FRF_USB_DIAG_HOLD
static volatile uint32_t native_usb_live[16];
#endif

#endif
static void frf_post_draw(void)
{
    if (__atomic_load_n(&post_stop,__ATOMIC_ACQUIRE)) return;
    if (!framebuffer || pitch<fb_width*2u || fb_width<340u ||
        fb_height<246u) return;
    if (__atomic_exchange_n(&post_lock,1u,__ATOMIC_ACQUIRE)) return;

    const uint32_t w=310u, h=227u, x=fb_width-w-12u, y=12u;
    const uint16_t white=0xe71cu, green=0x27e4u, amber=0xfd40u, cyan=0x3e19u;
    const uint32_t flags=__atomic_load_n(&post_flags,__ATOMIC_ACQUIRE);
    post_rect(x,y,w,h,0x0822u);
    post_rect(x,y,w,2,cyan);
    post_rect(x,y,2,h,cyan);
    post_rect(x+w-2,y,2,h,cyan);
    post_rect(x,y+h-2,w,2,cyan);
    post_text(x+10,y+7,"EMU68AGA  SYSTEM POST",white);
#ifdef FRF_USB_PASSIVE_TEST
    post_text(x+10,y+25,"RC8 / USB INPUT TEST",cyan);
#else
#ifdef FRF_USB_NATIVE_DWC2
#ifdef FRF_USB_TINYUSB
#ifdef FRF_USB_DIAG_HOLD
    post_text(x+10,y+25,"TINYUSB PI3 / PAD R38",cyan);
#else
    post_text(x+10,y+25,"TINYUSB PI3 / PAD R38",cyan);
#endif
#else
    post_text(x+10,y+25,"NATIVE USB V2 / CORE1",cyan);
#endif
#else
    post_text(x+10,y+25,"PI3 BARE METAL / RC8",cyan);
#endif
#endif
    post_rect(x+8,y+44,w-16,1,0x2965u);

    post_row(x+10,y+50,"AGA VIDEO",(flags&(1u<<1))?"READY":"WAIT ",
             (flags&(1u<<1))?green:amber);
    post_row(x+10,y+67,"ROM + MEMORY",(flags&(1u<<2))?"READY":"WAIT ",
             (flags&(1u<<2))?green:amber);
#ifdef FRF_USB_NATIVE_DWC2
    const uint32_t ns=__atomic_load_n(&native_usb_stage,__ATOMIC_ACQUIRE);
    const uint32_t nr=__atomic_load_n(&native_usb_rc,__ATOMIC_ACQUIRE);
    const uint32_t cpu1=__atomic_load_n(&native_usb_cpu1_seen,__ATOMIC_ACQUIRE);
    const char *ns_status =
#ifdef FRF_USB_DIAG_HOLD
          (ns==8u && nr==(uint32_t)-8 &&
           native_usb_live[13]>0u)?"HID NONBOOT":
          (ns==8u && nr==(uint32_t)-8 &&
           native_usb_live[12]>0u)?"DEVICES ONLY":
#endif
          (ns==8u && nr==0u)?"HID READY":
          (ns==8u && nr==(uint32_t)-12)?"NONBOOT HID":
          (ns==8u && nr==(uint32_t)-11)?"DEV NO HID":
          (ns==8u && nr==(uint32_t)-10)?"ENUM XFER FAIL":
          (ns==8u && nr==(uint32_t)-9)?"DMA ADDRESS":
          (ns==8u && nr==(uint32_t)-6)?"NO PORT PWR":
          (ns==8u && nr==(uint32_t)-7)?"NO ROOT HUB":
          (ns==8u && nr!=(uint32_t)0)?"NO USB DEV":
          (ns==7u && nr==0u)?"HID FOUND":
          (ns==4u && nr==(uint32_t)-7)?"NO HUB":
          (ns==4u && nr==(uint32_t)-6)?"NO PORT PWR":
          (ns==3u && nr!=(uint32_t)0)?"CORE FAIL":
          (ns==2u && nr!=(uint32_t)0)?"PROBE FAIL":
          (ns==1u && nr!=(uint32_t)0)?"FW PWR FAIL":
          (nr!=(uint32_t)0)?"USB ERROR":
          (ns>=6u)?"ENUM SCAN":
          (ns>=3u)?"DWC2 DMA":
          (ns>=1u)?"POWER":
          cpu1?"CORE1":"NO CPU1";
    post_row(x+10,y+84,"USB HOST",ns_status,
             (ns>=7u&&nr==0u)?green:amber);
#else
    post_row(x+10,y+84,"USB HOST",(flags&(1u<<9))?"READY":"WAIT ",
             (flags&(1u<<9))?green:amber);
#endif

    post_rect(x+8,y+106,w-16,1,0x2965u);
#ifdef FRF_USB_NATIVE_DWC2
    /* On-screen checkpoints: 1 firmware power, 2 probe, 3 host init,
       4 root port, 5 FIFO, 6 USB enumeration, 7 HID ready. */
    post_text(x+10,y+111,"USB STAGE:",white);
    post_dec(x+105,y+111,native_usb_stage,2,cyan);
    post_text(x+156,y+111,"RC:",white);
    post_hex(x+188,y+111,native_usb_rc,8,native_usb_rc?amber:green);
    post_text(x+10,y+135,"DETAIL:",white);
    post_hex(x+74,y+135,native_usb_detail,8,cyan);
    post_text(x+10,y+159,"AUX:",white);
    post_hex(x+60,y+159,native_usb_aux,8,cyan);
    post_text(x+10,y+183,"CPU1:",white);
    post_dec(x+62,y+183,cpu1,1,cpu1?green:amber);
    post_text(x+102,y+183,"AGA:",white);
    post_dec(x+143,y+183,native_usb_cpu1_chip,1,cyan);
    post_text(x+175,y+183,"AHB:",white);
    post_hex(x+211,y+183,native_usb_ahb_config,8,cyan);
    post_rect(x+8,y+204,w-16,1,0x2965u);
#ifdef FRF_USB_DIAG_HOLD
    post_text(x+10,y+207,"EP0:",cyan);
    post_hex(x+48,y+207,native_usb_ep0_hcchar,8,cyan);
    post_text(x+121,y+207,"SIZ:",cyan);
    post_hex(x+160,y+207,native_usb_ep0_hctsiz,8,cyan);
#else
    post_text(x+10,y+207,"MSC:",cyan);
    post_dec(x+46,y+207,native_usb_storage_detected%100u,2,green);
    post_text(x+90,y+207,"PAD:",cyan);
    post_dec(x+126,y+207,native_usb_pad_detected%100u,2,green);
    post_text(x+171,y+207,"BLKS:",cyan);
    post_hex(x+220,y+207,native_usb_storage_blocks,8,cyan);
#endif
#if defined(FRF_USB_TINYUSB)
    /* Display unsupported USB vendor interfaces too, not merely devices
       successfully claimed by HID. This makes R13 useful even when PAD=0. */
#ifdef FRF_USB_DIAG_HOLD
    const uint32_t vpy=y+360u; /* below existing 354px hardware trace */
#else
    const uint32_t vpy=y;
#endif
    if(x>=330u && fb_height>=vpy+166u) {
        const uint32_t px=x-324u;
        post_rect(px,vpy,310u,164u,0x0822u);
        post_rect(px,vpy,310u,2u,cyan);
        post_rect(px,vpy,2u,164u,cyan);
        post_rect(px+308u,vpy,2u,164u,cyan);
        post_rect(px,vpy+162u,310u,2u,cyan);
        post_text(px+9u,vpy+8u,"R18 2404:0078 USB IN PROBE",white);
        static const char *const lbl[7]={
            "STAGE/ADDR","LAST EP/TYP",
            "OPEN EVENTS","IN SUBMIT OK","IN SUBMIT NO",
            "IN RX OK","IN RX ERR"
        };
        for(unsigned i=0;i<7u;i++) {
            const uint32_t ry=vpy+28u+i*18u;
            post_text(px+10u,ry,lbl[i],white);
            post_hex(px+164u,ry,native_usb_probe[i],8,
                     i==5u?green:cyan);
        }
    }
#endif
#if defined(FRF_USB_TINYUSB)
    /* R15: show EACH successfully discovered USB address separately.
       R14's VENDOR field only recorded the last vendor-class interface;
       it could refer to a completely unrelated device. Show flags and
       interface on separate lines. No guessed devices or cached names. */
    if(fb_height>=548u) {
        const uint32_t px=x, py=y+238u, panelh=282u;
        post_rect(px,py,310u,panelh,0x0822u);
        post_rect(px,py,310u,2u,cyan);
        post_rect(px,py,2u,panelh,cyan);
        post_rect(px+308u,py,2u,panelh,cyan);
        post_rect(px,py+panelh-2u,310u,2u,cyan);
        post_text(px+9u,py+8u,"R18 EACH USB DEVICE / ADDRESS",white);
        post_text(px+9u,py+27u,"FLAGS 01=DEV 02=HID 04=MSC",amber);
        post_text(px+9u,py+45u,"      08=VENDOR INTERFACE",amber);
        unsigned shown=0;
        for(unsigned addr=1;addr<FRF_USB_ADDR_MAX && shown<7u;++addr) {
            const uint32_t id=native_usb_addr[addr].id;
            const uint32_t flags=native_usb_addr[addr].flags;
            const uint32_t iface=native_usb_addr[addr].iface;
            if(!flags) continue;
            const uint32_t ry=py+67u+(shown*29u);
            post_text(px+10u,ry,"D",white);
            post_dec(px+18u,ry,addr,2,white);
            post_hex(px+52u,ry,id,8,cyan);
            post_text(px+144u,ry,"F:",white);
            post_hex(px+169u,ry,flags,2,green);
            post_text(px+10u,ry+13u,"IF:",white);
            post_hex(px+52u,ry+13u,iface,8,cyan);
            ++shown;
        }
        if(!shown) post_text(px+10u,py+70u,"NO USB DEVICE CALLBACKS",amber);
    }
#endif
#if defined(FRF_USB_TINYUSB)
    /* Compact R16 host-to-guest link trace: safe for both normal preboot
       and USB-only live diagnostic. Only CPU1's 3Hz worker updates it. */
#ifdef FRF_USB_DIAG_HOLD
    const uint32_t ipy=y+530u;
#else
    const uint32_t ipy=y+174u;
#endif
    if(x>=330u && fb_height>=ipy+177u) {
        const uint32_t px=x-324u, ph=176u;
        static const char *const input_labels[13]={
            "HID RX","HID DEV/IF","HID LEN/ID","HID RAW 0",
            "VEND RX","VEND LEN/ID","VEND RAW 0",
            "PAD DECODE","DY VALID","JOY/FIRE",
            "JOY WRITES","GUEST JOY RD","GUEST CIA RD"
        };
        static const uint8_t input_idxs[13]={0,1,2,3,5,8,6,9,10,11,12,13,14};
        post_rect(px,ipy,310u,ph,0x0822u);
        post_rect(px,ipy,310u,2u,cyan);
        post_rect(px,ipy,2u,ph,cyan);
        post_rect(px+308u,ipy,2u,ph,cyan);
        post_rect(px,ipy+ph-2u,310u,2u,cyan);
        post_text(px+10u,ipy+6u,"R18 USB -> AMIGA INPUT LINK",white);
        for(unsigned i=0;i<13u;++i) {
            const uint32_t ry=ipy+19u+i*12u;
            post_text(px+10u,ry,input_labels[i],white);
            post_hex(px+181u,ry,native_usb_link[input_idxs[i]],8,
                     i==9u?green:cyan);
        }
    }
#endif
#if defined(FRF_USB_TINYUSB)
    /* Wide 64-byte protocol capture. On 1280x720 this occupies only
       bottom-left of the splash, never hides working USB counters.
       Base is first report with same length+report ID; LIVE updates
       from real USB packets; XOR exposes changed byte positions.
       Source 02 vendor takes priority over ordinary keyboard/mouse HID.
       This is observation only: does not inject guest joystick input. */
    if(fb_width>=980u && fb_height>=704u) {
        const uint32_t px=14u,py=277u,pw=604u,ph=416u;
        post_rect(px,py,pw,ph,0x0822u);
        post_rect(px,py,pw,2u,cyan);
        post_rect(px,py,2u,ph,cyan);
        post_rect(px+pw-2u,py,2u,ph,cyan);
        post_rect(px,py+ph-2u,pw,2u,cyan);
        post_text(px+10u,py+8u,"R38 HID INTERRUPT RECOVERY",white);
        extern uint32_t frf_r31_target_mode(void);
        extern void frf_r33_report_counts(uint32_t[3]);
        uint32_t ids33[3]={0};frf_r33_report_counts(ids33);
        post_text(px+285u,py+8u,"ID1",white);
        post_hex(px+323u,py+8u,ids33[0],4,cyan);
        post_text(px+402u,py+8u,"ID2",white);
        post_hex(px+434u,py+8u,ids33[1],4,green);
        post_text(px+500u,py+8u,"ID3",white);
        post_hex(px+540u,py+8u,ids33[2],4,amber);
        post_text(px+10u,py+29u,"MNT",white);
        extern void frf_r34_claim_stats(uint32_t[3]);
        uint32_t counts34[3]={0};frf_r34_claim_stats(counts34);
        post_hex(px+60u,py+29u,counts34[0],4,cyan);
        post_text(px+157u,py+29u,"CLM",white);
        post_hex(px+203u,py+29u,counts34[1],4,green);
        post_text(px+297u,py+29u,"LATE",white);
        post_hex(px+348u,py+29u,counts34[2],4,amber);
        extern void frf_pad_port_snapshot(uint32_t[4]);
        uint32_t port_r36[4]={0};frf_pad_port_snapshot(port_r36);
        post_text(px+400u,py+29u,"BTN",white);
        post_hex(px+453u,py+29u,port_r36[2],8,green);
        post_text(px+10u,py+47u,"RX:",white);
        post_hex(px+49u,py+47u,native_usb_packet_words[1],8,cyan);
        post_text(px+201u,py+47u,"CHANGED PKTS:",white);
        post_hex(px+327u,py+47u,native_usb_packet_words[2],8,cyan);
        post_text(px+10u,py+65u,"ADDR/EP/LEN/ID:",white);
        post_hex(px+187u,py+65u,native_usb_packet_words[3],8,green);
        post_text(px+345u,py+65u,"MODE:",white);
        post_hex(px+424u,py+65u,frf_r31_target_mode(),4,cyan);
        post_text(px+10u,py+84u,"OFF",white);
        post_text(px+84u,py+84u,"BASE",white);
        post_text(px+259u,py+84u,"LIVE",green);
        post_text(px+435u,py+84u,"XOR",amber);
        for(unsigned i=0;i<16u;i++) {
            const uint32_t yy=py+105u+i*18u;
            post_hex(px+13u,yy,i*4u,2,white);
            post_hex(px+84u,yy,native_usb_packet_words[7u+i*3u],8,cyan);
            post_hex(px+259u,yy,native_usb_packet_words[8u+i*3u],8,green);
            const uint32_t difference=native_usb_packet_words[9u+i*3u];
            post_hex(px+435u,yy,difference,8,difference?amber:cyan);
        }
        post_text(px+10u,py+396u,"R38 PAD RX:",white);
        post_hex(px+143u,py+396u,native_r20_compat[0],4,cyan);
        post_text(px+221u,py+396u,"OK:",white);
        post_hex(px+252u,py+396u,native_r20_compat[1],4,green);
        post_text(px+329u,py+396u,"JOY:",white);
        post_hex(px+367u,py+396u,port_r36[3]&0xffffu,4,green);
        post_text(px+449u,py+396u,"F:",white);
        post_hex(px+475u,py+396u,(port_r36[3]>>16)&1u,2,green);
    }
#endif
#ifdef FRF_USB_DIAG_HOLD
    /* Dedicated side panel: readings from the ACTUAL Pi3 controller,
       not synthetic event counters or presumed USB attachment.
       Only drawn on wide framebuffers, never covering the usual POST. */
    if (x >= 330u && fb_height >= 380u) {
        const uint32_t px=x-324u;
        const uint32_t ph=354u;
        const char *labels[16]={
            "HPRT", "GINTSTS", "GINTMSK", "HAINT",
            "HC0 CHAR", "HC0 SIZE", "HC0 DMA", "HC0 IRQ",
            "HC0 IRQMSK", "HOST TASK", "CHAN IRQ#", "PORT IRQ#",
            "DEV MOUNT", "HID MOUNT", "ENUM OK/ERR", "CTRL LEN"
        };
        post_rect(px,y,310u,ph,0x0822u);
        post_rect(px,y,310u,2u,cyan);
        post_rect(px,y,2u,ph,cyan);
        post_rect(px+308u,y,2u,ph,cyan);
        post_rect(px,y+ph-2u,310u,2u,cyan);
        post_text(px+9u,y+8u,"R23 USB-ONLY LIVE TEST",white);
        post_text(px+9u,y+25u,"NO AMIGA BOOT - PHOTO PANELS",amber);
        for (unsigned i=0;i<16;i++) {
            const uint32_t ry=y+47u+i*18u;
            post_text(px+10u,ry,labels[i],white);
            post_hex(px+167u,ry,native_usb_live[i],8,cyan);
        }
    }
#endif
#else
    post_text(x+10,y+111,"USB KB/MS/GP/MSC:",white);
    uint32_t a=__atomic_load_n(&post_kb,__ATOMIC_RELAXED);
    uint32_t b=__atomic_load_n(&post_ms,__ATOMIC_RELAXED);
    uint32_t c=__atomic_load_n(&post_gp,__ATOMIC_RELAXED);
    uint32_t d=__atomic_load_n(&post_storage,__ATOMIC_RELAXED);
    const uint32_t counts[4]={a,b,c,d};
    for (unsigned i=0;i<4;i++)
        if (counts[i]==~0u)
            post_text(x+169+i*30,y+111,"-",amber);
        else post_dec(x+169+i*30,y+111,counts[i]%10u,1,
                      counts[i]?green:amber);

    post_text(x+10,y+129,"GP RX:",white);
    post_dec(x+66,y+129,post_rx%100000u,5,post_rx?green:amber);
    post_text(x+130,y+129,"BAD:",white);
    post_dec(x+171,y+129,post_bad%10000u,4,post_bad?amber:green);
    post_text(x+215,y+129,"ERR:",white);
    post_dec(x+257,y+129,post_err%1000u,3,post_err?amber:green);

    post_text(x+10,y+147,"PKT ID:",white);
    post_hex(x+74,y+147,post_id,2,cyan);
    post_text(x+106,y+147,"LEN:",white);
    post_dec(x+146,y+147,post_len%100u,2,cyan);
    post_text(x+178,y+147,"DECODE:",white);
    post_dec(x+242,y+147,post_decoded%1000u,3,post_decoded?green:amber);

    post_text(x+10,y+165,"RAW:",white);
    post_hex(x+50,y+165,post_raw0,8,cyan);
    post_text(x+130,y+165,"/",white);
    post_hex(x+146,y+165,post_raw1,8,cyan);

    post_text(x+10,y+183,"JOY1:",white);
    post_hex(x+58,y+183,post_joy,4,green);
    post_text(x+106,y+183,"FIRE:",white);
    post_dec(x+154,y+183,post_fire?1u:0u,1,post_fire?green:amber);
    post_text(x+178,y+183,"EV:",white);
    post_dec(x+210,y+183,post_events%100000u,5,post_events?green:amber);

#ifdef FRF_USB_PASSIVE_TEST
    post_rect(x+8,y+204,w-16,1,0x2965u);
    post_text(x+10,y+207,"DWC:",white);
    uint32_t probe_rc=__atomic_load_n(&post_dwc2_rc,__ATOMIC_RELAXED);
    if (probe_rc==~0u)
        post_text(x+55,y+207,"WAIT",amber);
    else if (probe_rc==0) {
        post_hex(x+55,y+207,__atomic_load_n(&post_dwc2_id,__ATOMIC_RELAXED),8,green);
    } else {
        post_text(x+55,y+207,"ERR",amber);
        post_hex(x+87,y+207,probe_rc,2,amber);
    }
    post_text(x+158,y+207,"KEY:",white);
    post_dec(x+204,y+207,__atomic_load_n(&post_key_count,__ATOMIC_RELAXED)%10000u,4,green);
#else
    post_rect(x+8,y+204,w-16,1,0x2965u);
    if (post_joy || post_fire)
        post_text(x+10,y+207,"PAD INPUT ACTIVE",green);
    else if (post_events)
        post_text(x+10,y+207,"PAD INPUT CHANGE SEEN",green);
    else if (c>0 && c!=~0u && post_rx==0)
        post_text(x+10,y+207,"PAD FOUND - AWAITING USB",amber);
    else if (c==0u)
        post_text(x+10,y+207,"NO GAMEPAD ENUMERATED",amber);
    else if (post_rx>0)
        post_text(x+10,y+207,"USB RX - TEST BUTTONS",amber);
    else
        post_text(x+10,y+207,"TEST PAD DURING BOOT",cyan);
#endif
#endif /* FRF_USB_NATIVE_DWC2 */

    __asm__ volatile("dsb ishst" ::: "memory");
    __atomic_store_n(&post_lock,0u,__ATOMIC_RELEASE);
}
#ifdef FRF_USB_PASSIVE_TEST
void frf_post_native_usb_probe(int rc, uint32_t id)
{
    post_dwc2_id=id;
    post_dwc2_rc=(uint32_t)rc;
    frf_post_draw();
}
void frf_post_native_usb_key(void)
{
    __atomic_add_fetch(&post_key_count,1u,__ATOMIC_RELAXED);
    frf_post_draw();
}
#endif
#ifdef FRF_USB_NATIVE_DWC2
void frf_post_native_usb_cpu1_seen(unsigned chip_base_set)
{
    __atomic_store_n(&native_usb_cpu1_seen,1u,__ATOMIC_RELEASE);
    native_usb_cpu1_chip=!!chip_base_set;
    frf_post_draw();
}
void frf_post_r26_get_stats(uint32_t a,uint32_t ok,uint32_t fail,uint32_t pending)
{
    native_r26_get[0]=a;native_r26_get[1]=ok;native_r26_get[2]=fail;native_r26_get[3]=pending;
    frf_post_draw();
}
void frf_post_r20_compat(const uint32_t fields[6])
{
    if(!fields) return;
    for(unsigned i=0;i<6u;i++)native_r20_compat[i]=fields[i];
    frf_post_draw();
}
void frf_post_r19_hid_map(const uint32_t fields[6])
{
    if(!fields) return;
    for(unsigned i=0;i<6u;++i)native_r19_map[i]=fields[i];
    frf_post_draw();
}
void frf_post_native_usb_packet_bytes(const uint32_t words[55])
{
    if(!words) return;
    for(unsigned i=0;i<55u;++i) native_usb_packet_words[i]=words[i];
    frf_post_draw();
}
void frf_post_native_usb_probe_trace(const uint32_t fields[7])
{
    if(!fields) return;
    for(unsigned i=0;i<7u;i++) native_usb_probe[i]=fields[i];
    frf_post_draw();
}
void frf_post_native_usb_input_trace(const uint32_t *fields,unsigned count)
{
    if(!fields) return;
    if(count>15u) count=15u;
    for(unsigned i=0;i<count;++i) native_usb_link[i]=fields[i];
    frf_post_draw();
}
void frf_post_native_usb_device_event(uint32_t addr,uint32_t vidpid,
                                       uint32_t iface,uint32_t event)
{
    if(addr==0u || addr>=FRF_USB_ADDR_MAX) return;
    struct frf_device_snapshot *d=&native_usb_addr[addr];
    if(event==0u) {
        d->flags=0;d->iface=0;d->id=0;
    } else {
        if(vidpid) d->id=vidpid;
        if(iface) d->iface=iface;
        const uint32_t flag= event==1u?1u:event==2u?2u:
                             event==3u?4u:event==4u?8u:0u;
        d->flags|=flag;
    }
    ++native_usb_addr_events;
    frf_post_draw();
}
void frf_post_native_usb_pad_stats(uint32_t accepted,uint32_t ignored)
{
    native_dyonder_ok=accepted;
    native_dyonder_ignored=ignored;
    frf_post_draw();
}
void frf_post_native_usb_vendor_probe(uint32_t vidpid,uint32_t iface,uint32_t kind)
{
    /* Captured directly from class-driver probe when TinyUSB parses
       configuration descriptors. kind=0 unsupported; 1 XInput, 2 GIP. */
    native_vendor_id=vidpid;
    native_vendor_iface=iface;
    native_vendor_kind=kind;
    ++native_vendor_mounts;
    frf_post_draw();
}
void frf_post_native_usb_peripherals(uint32_t msc,uint32_t pads,uint32_t blocks)
{
    native_usb_storage_detected=msc;
    native_usb_pad_detected=pads;
    native_usb_storage_blocks=blocks;
    frf_post_draw();
}
void frf_post_native_usb_ep0(unsigned hcchar,unsigned hctsiz)
{
    if (!native_usb_ep0_hcchar) {
        native_usb_ep0_hcchar=hcchar;
        native_usb_ep0_hctsiz=hctsiz;
    }
}
#ifdef FRF_USB_DIAG_HOLD
void frf_post_native_usb_trace(const uint32_t *snapshot,unsigned n)
{
    if (n>16u) n=16u;
    for(unsigned i=0;i<n;i++) native_usb_live[i]=snapshot[i];
    frf_post_draw();
}
#endif
void frf_post_native_usb_ahb(unsigned raw)
{
    native_usb_ahb_config=raw;
    frf_post_draw();
}
void frf_post_native_usb_stage(unsigned stage, int rc,
                               uint32_t detail, uint32_t aux)
{
    native_usb_stage=stage;
    native_usb_rc=(uint32_t)rc;
    native_usb_detail=detail;
    native_usb_aux=aux;
    frf_post_draw();
}
#endif
void frf_post_usb_devices(unsigned kb,unsigned ms,unsigned gp,unsigned msc)
{
    post_kb=kb;post_ms=ms;post_gp=gp;post_storage=msc;
    frf_post_draw();
}
void frf_post_pad_sample(unsigned rx,unsigned bad,unsigned err,
                         unsigned id,unsigned len,unsigned raw0,unsigned raw1,
                         unsigned decoded,unsigned events,unsigned joy,unsigned fire)
{
    /* Called from the bounded 10Hz USB service loop, never USB IRQ context. */
    post_rx=rx;post_bad=bad;post_err=err;post_id=id;post_len=len;
    post_raw0=raw0;post_raw1=raw1;
    post_decoded=decoded;post_events=events;post_joy=joy;post_fire=fire;
    frf_post_draw();
}
void frf_boot_stage(unsigned stage)
{
    if (stage<1u || stage>10u || __atomic_load_n(&post_stop,__ATOMIC_ACQUIRE)) return;
    __atomic_fetch_or(&post_flags,1u<<stage,__ATOMIC_ACQ_REL);
    frf_post_draw();
    /* Stop all CPU1 framebuffer writes before Amiga owns video memory. */
    if (stage==5u) __atomic_store_n(&post_stop,1u,__ATOMIC_RELEASE);
}

void display_logo()
{
    uint16_t *fb;
    struct Size sz = get_display_size();
    uint32_t start_x, start_y;
    of_node_t *e = NULL;

    e = dt_find_node("/chosen");
    if (e)
    {
        of_property_t * prop = dt_find_property(e, "bootargs");
        if (prop)
        {
            const char *tok;
            if ((tok = find_token(prop->op_value, "logo=")))
            {
                tok += 5;

                if (strncmp(tok, "purple", 6) == 0)
                    purple = 1;
                else if (strncmp(tok, "black", 5) == 0)
                    black = 1;
            }
        }
    }

    kprintf("[BOOT] Display size is %dx%d\n", sz.width, sz.height);
    fb_width = sz.width;
    fb_height = sz.height;
    init_display(sz, (void**)&framebuffer, &pitch);
    kprintf("[BOOT] Framebuffer @ %08x\n", framebuffer);
    fb = framebuffer;

    struct EmuLogo *emu68logo = rle_decode(logo_emu68, logo_emu68_len);
    uint8_t *data = emu68logo->el_Data;
    start_x = (sz.width - emu68logo->el_Width) / 2;
    start_y = (sz.height - emu68logo->el_Height) / 2;

    kprintf("[BOOT] Logo start coordinate: %dx%d, size: %dx%d\n", start_x, start_y, emu68logo->el_Width, emu68logo->el_Height);

    /* Calculate text coordinate for version string */
    text_y = (fb_height - 16 - 5) / 16;
    text_x = (fb_width - strlen("EMU68AGA Version 0.1-Alpha") * 8 - 1) / 8;

#if defined(PISTORM_ANY_MODEL)
    struct EmuLogo *pistormlogo = rle_decode(logo_pistorm, logo_pistorm_len);
    start_y -= (pistormlogo->el_Height + 10) / 2;
#ifdef AGA_PISTORM
    /* a third line under the PiStorm logo; keep the whole stack centred */
    struct EmuLogo *agalogo = rle_decode(logo_aga, logo_aga_len);
    start_y -= (agalogo->el_Height + 10) / 2;
#endif
#if defined(PISTORM)
    const uint8_t pistorm_model = pistorm_get_model();
    switch(pistorm_model)
    {
        case PISTORM_MODEL_16:
            text_x -= strlen("PiStorm16, ");
            break;
        case PISTORM_MODEL_32:
            text_x -= strlen("PiStorm32lite, ");
            break;
    }
#elif defined(PISTORM_CLASSIC)
    /* Tester splash uses a single centered footer. */
#endif
#endif

    /* First clear the screen. Use color in top left corner of RLE image for that */
    {
        uint8_t gray = data[0];
        uint16_t color;

        if (purple)
        {
            gray = 240 - gray;
            int r=-330,g=-343,b=-91;
            r += (gray * 848) >> 8;
            g += (gray * 768) >> 8;
            b += (gray * 341) >> 8;

            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            color = (b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11);
        }
        else if (black)
        {
            gray = 0;
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }
        else
        {
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }
#ifdef AGA_PISTORM
        /* black behind the AGA-PISTORM splash whatever logo= says - the
           picture has its own colours, and CaffeineOS's logo=purple framed it
           in purple (1.110) */
        color = 0;
#endif

        for (int i=0; i < sz.width * sz.height; i++)
            fb[i] = LE16(color);
    }

    /* Then draw logos according to current settings.
       AGA-PISTORM shows one picture of its own instead of the stack of three
       tinted logos. Everything decoded above gets freed unused - a few
       microseconds once at boot, and it keeps this diff small. */
#ifdef AGA_PISTORM
    {
        /* about three quarters of the screen, in whole pixels: 1:1 it was a
           tenth of a 1080p screen */
        uint32_t sw = (sz.width * 3 / 4) / LOGO_AGA_RGB_WIDTH;
        uint32_t sh = (sz.height * 3 / 4) / LOGO_AGA_RGB_HEIGHT;
        uint32_t sc = sw < sh ? sw : sh;
        if (sc < 1) sc = 1;
        uint32_t lw = LOGO_AGA_RGB_WIDTH * sc, lh = LOGO_AGA_RGB_HEIGHT * sc;
        uint32_t lx = (sz.width > lw) ? (sz.width - lw) / 2 : 0;
        uint32_t ly = (sz.height > lh) ? (sz.height - lh) / 2 : 0;
        draw_logo_rgb(lx, ly, sc);
        tlsf_free(tlsf, emu68logo);
#if defined(PISTORM_ANY_MODEL)
        tlsf_free(tlsf, pistormlogo);
        tlsf_free(tlsf, agalogo);
#endif
    }
#else
    draw_logo(emu68logo, start_x, start_y);

    if (dt_find_property(dt_find_node("/emu68"), "ppc-enable"))
    {
        struct EmuLogo *ppclogo = rle_decode(logo_ppc, logo_ppc_len);
        draw_logo(ppclogo, start_x + emu68logo->el_Width, start_y + 49);
        tlsf_free(tlsf, ppclogo);
    }
    start_y += emu68logo->el_Height + 10;
    tlsf_free(tlsf, emu68logo);

#if defined(PISTORM_ANY_MODEL)
    draw_logo(pistormlogo, (sz.width - pistormlogo->el_Width) / 2, start_y);
    tlsf_free(tlsf, pistormlogo);
#endif
#endif
#if defined(PISTORM_ANY_MODEL)
#if defined(PISTORM)
    switch(pistorm_model)
    {
        case PISTORM_MODEL_16:
            kprintf_pc(__putc, NULL, "PiStorm16, ");
            break;
        case PISTORM_MODEL_32:
            kprintf_pc(__putc, NULL, "PiStorm32lite, ");
            break;
    }
#elif defined(PISTORM_CLASSIC)
    /* Tester splash: label printed below instead. */
#endif
#endif
    /* Print EMu68 version number and git sha. */
    kprintf_pc(__putc, NULL, "EMU68AGA Version 0.1-Alpha");
    
    /* Reset test coordinates for further text printing (e.g. buptest) */
    text_x = 0;
    text_y = 0;

    #if 0
extern unsigned char pistorm_get_model();
    kprintf_pc(__putc, NULL, "PiStorm model: %d\n", pistorm_get_model());
    kprintf_pc(__putc, NULL, "Firmware file: %p\n", firmware_file);
    kprintf_pc(__putc, NULL, "Firmware size: %d\n", firmware_size);
    #endif
}

uintptr_t top_of_ram;

#ifdef PISTORM_ANY_MODEL
#include "ps_protocol.h"

extern int block_c0;
#endif

// Helper function to map peripheral ranges
void map_peripheral_ranges(char *node_name, uint32_t *start_map)
{
    int is_scb = strcmp(node_name, "/scb") == 0;

    of_node_t *e = dt_find_node(node_name);
    if (!e)
        return;

    of_property_t *p = dt_find_property(e, "ranges");
    if (!p)
        return;

    uint32_t *ranges = p->op_value;
    int32_t len = p->op_length;

    int addr_cpu_len = dt_get_property_value_u32(e->on_parent, "#address-cells", 1, FALSE);
    int addr_bus_len = dt_get_property_value_u32(e, "#address-cells", 1, TRUE);
    int size_bus_len = dt_get_property_value_u32(e, "#size-cells", 1, TRUE);

    int pos_abus = addr_bus_len - 1;
    int pos_acpu = pos_abus + addr_cpu_len;
    int pos_sbus = pos_acpu + size_bus_len;

    while (len > 0)
    {
        uint32_t addr_bus, addr_cpu;
        uint32_t addr_len;

        addr_bus = BE32(ranges[pos_abus]);
        addr_cpu = BE32(ranges[pos_acpu]);
        addr_len = BE32(ranges[pos_sbus]);

        /* Ignore large identity mappings in /scb branch */
        if(is_scb && addr_bus == addr_cpu)
        {
            len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
            ranges += addr_bus_len + addr_cpu_len + size_bus_len;
            continue;
        }

        mmu_map(addr_cpu, *start_map << 21, addr_len,
                MMU_ACCESS | MMU_ALLOW_EL0 | MMU_ATTR_DEVICE, 0);

        kprintf("bus: %08x, cpu: %08x, len: %08x\n", addr_bus, addr_cpu, addr_len);

        ranges[pos_acpu] = BE32(*start_map << 21);

        if (addr_bus == 0x40000000) {
            extern uintptr_t local_intc_base;
            local_intc_base = *start_map << 21;
        }

        // Round up length to nearest 2 MiB and calculate addend for next mapping
        uint32_t addend = (addr_len + (1 << 21) - 1) >> 21;
        
        *start_map += addend;

        len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
        ranges += addr_bus_len + addr_cpu_len + size_bus_len;
    }
}

/* Helper to clamp and map PCIe MMIO window and publish DT properties */
static void map_pcie_mmio_window(uint32_t *start_map)
{
    /* Resolve PCIe node via alias pcie0 */
    of_node_t *pcie = NULL;
    of_node_t *aliases = dt_find_node("/aliases");
    if (aliases)
    {
        of_property_t *ap = dt_find_property(aliases, "pcie0");
        if (ap && ap->op_value && ap->op_length > 0)
        {
            pcie = dt_find_node((char *)ap->op_value);
        }
    }

    if (!pcie)
    {
        kprintf("[PCIE] No PCIe node, skipping\n");
        return;
    }

    of_property_t *rp = dt_find_property(pcie, "ranges");
    if (!rp)
    {
        kprintf("[PCIE] FATAL: Unable to find PCIe ranges property\n");
        return;
    }

    uint32_t *ranges = (uint32_t *)rp->op_value;
    int len = rp->op_length;

    int addr_cpu_len = dt_get_property_value_u32(pcie->on_parent, "#address-cells", 2, FALSE);
    int addr_bus_len = dt_get_property_value_u32(pcie, "#address-cells", 3, TRUE);
    int size_bus_len = dt_get_property_value_u32(pcie, "#size-cells", 2, TRUE);

    while (len > 0)
    {
        /* Decode the child (PCI) address space code from the first cell */
        uint32_t space_code = BE32(ranges[0]);
        int is_mem32 = ((space_code & 0x03000000u) == 0x02000000u);
        int is_prefetch = ((space_code & 0x40000000u) != 0);
        kprintf("[PCIE] space_code=%08x mem32=%d prefetch=%d\n", space_code, is_mem32, is_prefetch);

        if (is_mem32 && !is_prefetch)
        {
            uint64_t phys;
            if (addr_cpu_len >= 2)
            {
                phys = BE32(ranges[addr_bus_len + addr_cpu_len - 2]);
                phys = (phys << 32) | BE32(ranges[addr_bus_len + addr_cpu_len - 1]);
            } else if (addr_cpu_len == 1) {
                phys = BE32(ranges[addr_bus_len + addr_cpu_len - 1]);
            } else {
                kprintf("[PCIE] Unsupported addr_cpu_len %d in PCIe ranges\n", addr_cpu_len);
                break;
            }

            /* Clamp size to 64 MiB */
            const uint32_t map_size = 0x04000000u;
            if (size_bus_len >= 2) {
                ranges[addr_bus_len + addr_cpu_len + size_bus_len - 2] = 0;
            }
            ranges[addr_bus_len + addr_cpu_len + size_bus_len - 1] = BE32(map_size);

            /* Map into low virtual 0xF2xxxxxx area - it must fit below 0xff00xxxx which is Zorro3 config space */
            if (*start_map + (map_size >> 21) >= 0xff0 / 2)
            {
                kprintf("[PCIE] FATAL: Out of low-virt address space for PCIe MMIO!\n");
                break;
            }

            uintptr_t virt = (uintptr_t)(*start_map << 21);
            *start_map += map_size >> 21;
            kprintf("[PCIE] Next low-virt addr: %08x\n", (*start_map) << 21);

            mmu_map(phys, virt, (uintptr_t)map_size,
                    MMU_ACCESS | MMU_ALLOW_EL0 | MMU_ATTR_DEVICE, 0);

            kprintf("[PCIE] MMIO: phys=%08x%08x virt=%08x size=%08x\n",
                    (uint32_t)(phys >> 32), (uint32_t)phys, (uint32_t)virt, (uint32_t)map_size);

            /* Expose phys/virt/size as emu68-specific properties for the guest */
            uint32_t buf64[2];
            buf64[0] = BE32((uint32_t)(phys >> 32));
            buf64[1] = BE32((uint32_t)(phys & 0xffffffffu));
            dt_add_property(pcie, "emu68,pci-mmio-phys", buf64, sizeof(buf64));

            buf64[0] = BE32(virt);
            dt_add_property(pcie, "emu68,pci-mmio-virt", buf64, sizeof(buf64[0]));

            buf64[0] = BE32(map_size);
            dt_add_property(pcie, "emu68,pci-mmio-size", buf64, sizeof(buf64[0]));

            /* Only adjust/map the first non-prefetchable mem32 window */
            break;
        }

        len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
        ranges += addr_bus_len + addr_cpu_len + size_bus_len;
    }
}

void platform_init()
{
    uint32_t start_map = 0xf20 / 2;

    /*
        Prepare mapping for peripherals. Use and update the data from device tree here
        All peripherals are mapped in the lower 4G address space so that they can be
        accessed from m68k.
    */
    map_peripheral_ranges("/soc", &start_map);
    map_peripheral_ranges("/scb", &start_map);

    /* 
        Map PCIe MMIO window if PCIe is present.
        Reduces the window size to 64 MiB to fit into the low-virt area.
        Exposes the phys/virt/size as emu68,pci-mmio-phys/virt/size properties.
        The mapping in ranges remains using physical address.
        The PCIe driver needs both the physical and virtual address.
    */
    map_pcie_mmio_window(&start_map);
}

#if defined(FRF_WIFI_GUEST_COMPAT)
extern void frf_wifi_guest_compat(void);
#endif
#if defined(FRF_WIFI_HW_PROBE)
extern void frf_wifi_hw_probe(void);
#endif
#if defined(FRF_WIFI_SD1_ENUM)
extern void frf_wifi_sd1_identify(void);
static int frf_wifi_opted_in(void)
{
    of_node_t *node=dt_find_node("/chosen");
    of_property_t *prop=node?dt_find_property(node,"bootargs"):0;
    const char opt[]="frf_armwifi=1";
    if(!prop || !prop->op_value) return 0;
    const char *p=(const char*)prop->op_value;
    for(unsigned i=0;i+sizeof(opt)-1 <= prop->op_length;i++)
       if(!memcmp(p+i,opt,sizeof(opt)-1)) return 1;
    return 0;
}
#endif

void platform_post_init()
{
    void *base_vcmem;
    uint32_t size_vcmem;

    kprintf("[BOOT] Platform post init\n");

    if (get_max_clock_rate(3) != get_clock_rate(3)) {
        kprintf("[BOOT] Changing ARM clock from %d MHz to %d MHz\n", get_clock_rate(3)/1000000, get_max_clock_rate(3)/1000000);
        set_clock_rate(3, get_max_clock_rate(3));
    }
    kprintf("[BOOT] ARM Clock at %d MHz\n", get_clock_rate(3) / 1000000);  
    kprintf("[BOOT] CORE Clock at %d MHz\n", get_clock_rate(4) / 1000000);

    get_vc_memory(&base_vcmem, &size_vcmem);
    kprintf("[BOOT] VC4 memory: %p-%p\n", (intptr_t)base_vcmem, (intptr_t)base_vcmem + size_vcmem - 1);

    if (base_vcmem && size_vcmem)
    {
        mmu_map((uintptr_t)base_vcmem, (uintptr_t)base_vcmem, size_vcmem,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_WRITETHROUGH, 0);
    }

    display_logo();
#if defined(FRF_WIFI_GUEST_COMPAT)
    frf_wifi_guest_compat();
#endif
#if defined(FRF_WIFI_HW_PROBE)
    frf_wifi_hw_probe();
#endif
#if defined(FRF_WIFI_SD1_ENUM)
    if(frf_wifi_opted_in())
        frf_wifi_sd1_identify();
    else
        kprintf("[FRFNET] ARM SD1 hardware initialization DISABLED. Add frf_armwifi=1 to bootargs for experimental test.\n");
#endif
    frf_boot_stage(1);

#ifdef PISTORM_ANY_MODEL
#ifndef FRF_VIRTUAL_AGA

#if defined(PISTORM_CLASSIC)
    int usercode = 0;

    if (firmware_file && firmware_size) {
        kprintf("[BOOT] Flashing CPLD firmware...\n");
        ps_cpld_load(firmware_file, firmware_size, 1, 0);
    }
    usercode =ps_cpld_load(firmware_file, firmware_size, 0, 0);

    (void)usercode; //suppress warning for now...for later inclusion in DT
#endif

    kprintf("[BOOT] sending RESET signal to Amiga\n");
    ps_pulse_reset();

    block_c0 = 0;

    ps_write_8_int(0xde1000, 0);
    if (ps_read_8_int(0xde1000) & 0x80)
    {
        if (ps_read_8_int(0xde1000) & 0x80)
        {
            if (!(ps_read_8_int(0xde1000) & 0x80))
            {
                if (ps_read_8_int(0xde1000) & 0x80)
                {
                    kprintf("[BOOT] Gayle appears to be present\n");
                    block_c0 = 1;
                }
            }
        }
    }

    if (block_c0 == 0)
    {
        kprintf("[BOOT] Gayle not detected\n");
    }

    of_node_t *e = dt_find_node("/emu68");
    if (e)
    {
        if (dt_find_property(e, "beamcon0-pal-clear")) {
            ps_write_16_int(0xdff1dc, 0x00);
        }
        else if (dt_find_property(e, "beamcon0-pal-set")) {
            ps_write_16_int(0xdff1dc, 0x20);
        }
    }

#else
    kprintf("[BOOT] FRF virtual AGA: skipped physical PiStorm reset/CPLD/Gayle startup\n");
    block_c0 = 0;
#endif
#endif

    //*(volatile uint32_t *)0xf3000034 = LE32((7680000) | 0x30000000);
}

#if defined(__BCM2708A0__)
   #define UNICAM_CTRL    0x000
   #define UNICAM_STA     0x004
   #define UNICAM_ANA     0x008
   #define UNICAM_PRI     0x00c
   #define UNICAM_CLK     0x010
   #define UNICAM_DAT0    0x014
   #define UNICAM_DAT1    0x018
   #define UNICAM_DAT2    0x01c
   #define UNICAM_DAT3    0x020
   #define UNICAM_CMP0    0x024
   #define UNICAM_CMP1    0x028
   #define UNICAM_CAP0    0x02c
   #define UNICAM_CAP1    0x030
   #define UNICAM_DBG0    0x0f0
   #define UNICAM_DBG1    0x0f4
   #define UNICAM_DBG2    0x0f8
   #define UNICAM_ICTL    0x100
   #define UNICAM_ISTA    0x104
   #define UNICAM_IDI     0x108
   #define UNICAM_IPIPE   0x10c
   #define UNICAM_IBSA    0x110
   #define UNICAM_IBEA    0x114
   #define UNICAM_IBLS    0x118
   #define UNICAM_IBWP    0x11c
   #define UNICAM_IHWIN   0x120
   #define UNICAM_IHSTA   0x124
   #define UNICAM_IVWIN   0x128
   #define UNICAM_IVSTA   0x12c
   #define UNICAM_DCS     0x200
   #define UNICAM_DBSA    0x204
   #define UNICAM_DBEA    0x208
   #define UNICAM_DBWP    0x20c
#else
   #define UNICAM_CTRL    0x000
   #define UNICAM_STA     0x004
   #define UNICAM_ANA     0x008
   #define UNICAM_PRI     0x00c
   #define UNICAM_CLK     0x010
   #define UNICAM_CLT     0x014
   #define UNICAM_DAT0    0x018
   #define UNICAM_DAT1    0x01c
   #define UNICAM_DAT2    0x020
   #define UNICAM_DAT3    0x024
   #define UNICAM_DLT     0x028
   #define UNICAM_CMP0    0x02c
   #define UNICAM_CMP1    0x030
   #define UNICAM_CAP0    0x034
   #define UNICAM_CAP1    0x038
   #define UNICAM_ICTL    0x100
   #define UNICAM_ISTA    0x104
   #define UNICAM_IDI0    0x108
   #define UNICAM_IPIPE   0x10c
   #define UNICAM_IBSA0   0x110
   #define UNICAM_IBEA0   0x114
   #define UNICAM_IBLS    0x118
   #define UNICAM_IBWP    0x11c
   #define UNICAM_IHWIN   0x120
   #define UNICAM_IHSTA   0x124
   #define UNICAM_IVWIN   0x128
   #define UNICAM_IVSTA   0x12c
   #define UNICAM_ICC     0x130
   #define UNICAM_ICS     0x134
   #define UNICAM_IDC     0x138
   #define UNICAM_IDPO    0x13c
   #define UNICAM_IDCA    0x140
   #define UNICAM_IDCD    0x144
   #define UNICAM_IDS     0x148
   #define UNICAM_DCS     0x200
   #define UNICAM_DBSA0   0x204
   #define UNICAM_DBEA0   0x208
   #define UNICAM_DBWP    0x20c
   #define UNICAM_DBCTL   0x300
   #define UNICAM_IBSA1   0x304
   #define UNICAM_IBEA1   0x308
   #define UNICAM_IDI1    0x30c
   #define UNICAM_DBSA1   0x310
   #define UNICAM_DBEA1   0x314
   #define UNICAM_MISC    0x400
#endif

/*
 * The following bitmasks are from the kernel released by Broadcom
 * for Android - https://android.googlesource.com/kernel/bcm/
 * The Rhea, Hawaii, and Java chips all contain the same VideoCore4
 * Unicam block as BCM2835, as defined in eg
 * arch/arm/mach-rhea/include/mach/rdb_A0/brcm_rdb_cam.h and similar.
 * Values reworked to use the kernel BIT and GENMASK macros.
 *
 * Some of the bit mnenomics have been amended to match the datasheet.
 */
/* UNICAM_CTRL Register */
#define UNICAM_CPE BIT(0)
#define UNICAM_MEM BIT(1)
#define UNICAM_CPR BIT(2)
#define UNICAM_CPM_MASK GENMASK(3, 3)
#define UNICAM_CPM_CSI2 0
#define UNICAM_CPM_CCP2 1
#define UNICAM_SOE BIT(4)
#define UNICAM_DCM_MASK GENMASK(5, 5)
#define UNICAM_DCM_STROBE 0
#define UNICAM_DCM_DATA 1
#define UNICAM_SLS BIT(6)
#define UNICAM_PFT_MASK GENMASK(11, 8)
#define UNICAM_OET_MASK GENMASK(20, 12)

/* UNICAM_STA Register */
#define UNICAM_SYN BIT(0)
#define UNICAM_CS BIT(1)
#define UNICAM_SBE BIT(2)
#define UNICAM_PBE BIT(3)
#define UNICAM_HOE BIT(4)
#define UNICAM_PLE BIT(5)
#define UNICAM_SSC BIT(6)
#define UNICAM_CRCE BIT(7)
#define UNICAM_OES BIT(8)
#define UNICAM_IFO BIT(9)
#define UNICAM_OFO BIT(10)
#define UNICAM_BFO BIT(11)
#define UNICAM_DL BIT(12)
#define UNICAM_PS BIT(13)
#define UNICAM_IS BIT(14)
#define UNICAM_PI0 BIT(15)
#define UNICAM_PI1 BIT(16)
#define UNICAM_FSI_S BIT(17)
#define UNICAM_FEI_S BIT(18)
#define UNICAM_LCI_S BIT(19)
#define UNICAM_BUF0_RDY BIT(20)
#define UNICAM_BUF0_NO BIT(21)
#define UNICAM_BUF1_RDY BIT(22)
#define UNICAM_BUF1_NO BIT(23)
#define UNICAM_DI BIT(24)

#define UNICAM_STA_MASK_ALL                                                    \
  (UNICAM_DL + UNICAM_SBE + UNICAM_PBE + UNICAM_HOE + UNICAM_PLE +             \
   UNICAM_SSC + UNICAM_CRCE + UNICAM_IFO + UNICAM_OFO + UNICAM_PS +            \
   UNICAM_PI0 + UNICAM_PI1)

/* UNICAM_ANA Register */
#define UNICAM_APD BIT(0)
#define UNICAM_BPD BIT(1)
#define UNICAM_AR BIT(2)
#define UNICAM_DDL BIT(3)
#define UNICAM_CTATADJ_MASK GENMASK(7, 4)
#define UNICAM_PTATADJ_MASK GENMASK(11, 8)

/* UNICAM_PRI Register */
#define UNICAM_PE BIT(0)
#define UNICAM_PT_MASK GENMASK(2, 1)
#define UNICAM_NP_MASK GENMASK(7, 4)
#define UNICAM_PP_MASK GENMASK(11, 8)
#define UNICAM_BS_MASK GENMASK(15, 12)
#define UNICAM_BL_MASK GENMASK(17, 16)

/* UNICAM_CLK Register */
#define UNICAM_CLE BIT(0)
#define UNICAM_CLPD BIT(1)
#define UNICAM_CLLPE BIT(2)
#define UNICAM_CLHSE BIT(3)
#define UNICAM_CLTRE BIT(4)
#define UNICAM_CLAC_MASK GENMASK(8, 5)
#define UNICAM_CLSTE BIT(29)

/* UNICAM_CLT Register */
#define UNICAM_CLT1_MASK GENMASK(7, 0)
#define UNICAM_CLT2_MASK GENMASK(15, 8)

/* UNICAM_DATn Registers */
#define UNICAM_DLE BIT(0)
#define UNICAM_DLPD BIT(1)
#define UNICAM_DLLPE BIT(2)
#define UNICAM_DLHSE BIT(3)
#define UNICAM_DLTRE BIT(4)
#define UNICAM_DLSM BIT(5)
#define UNICAM_DLFO BIT(28)
#define UNICAM_DLSTE BIT(29)

#define UNICAM_DAT_MASK_ALL (UNICAM_DLSTE + UNICAM_DLFO)

/* UNICAM_DLT Register */
#define UNICAM_DLT1_MASK GENMASK(7, 0)
#define UNICAM_DLT2_MASK GENMASK(15, 8)
#define UNICAM_DLT3_MASK GENMASK(23, 16)

/* UNICAM_ICTL Register */
#define UNICAM_FSIE BIT(0)
#define UNICAM_FEIE BIT(1)
#define UNICAM_IBOB BIT(2)
#define UNICAM_FCM BIT(3)
#define UNICAM_TFC BIT(4)
#define UNICAM_LIP_MASK GENMASK(6, 5)
#define UNICAM_LCIE_MASK GENMASK(28, 16)

/* UNICAM_IDI0/1 Register */
#define UNICAM_ID0_MASK GENMASK(7, 0)
#define UNICAM_ID1_MASK GENMASK(15, 8)
#define UNICAM_ID2_MASK GENMASK(23, 16)
#define UNICAM_ID3_MASK GENMASK(31, 24)

/* UNICAM_ISTA Register */
#define UNICAM_FSI BIT(0)
#define UNICAM_FEI BIT(1)
#define UNICAM_LCI BIT(2)

#define UNICAM_ISTA_MASK_ALL (UNICAM_FSI + UNICAM_FEI + UNICAM_LCI)

/* UNICAM_IPIPE Register */
#define UNICAM_PUM_MASK GENMASK(2, 0)
/* Unpacking modes */
#define UNICAM_PUM_NONE 0
#define UNICAM_PUM_UNPACK6 1
#define UNICAM_PUM_UNPACK7 2
#define UNICAM_PUM_UNPACK8 3
#define UNICAM_PUM_UNPACK10 4
#define UNICAM_PUM_UNPACK12 5
#define UNICAM_PUM_UNPACK14 6
#define UNICAM_PUM_UNPACK16 7
#define UNICAM_DDM_MASK GENMASK(6, 3)
#define UNICAM_PPM_MASK GENMASK(9, 7)
/* Packing modes */
#define UNICAM_PPM_NONE 0
#define UNICAM_PPM_PACK8 1
#define UNICAM_PPM_PACK10 2
#define UNICAM_PPM_PACK12 3
#define UNICAM_PPM_PACK14 4
#define UNICAM_PPM_PACK16 5
#define UNICAM_DEM_MASK GENMASK(11, 10)
#define UNICAM_DEBL_MASK GENMASK(14, 12)
#define UNICAM_ICM_MASK GENMASK(16, 15)
#define UNICAM_IDM_MASK GENMASK(17, 17)

/* UNICAM_ICC Register */
#define UNICAM_ICFL_MASK GENMASK(4, 0)
#define UNICAM_ICFH_MASK GENMASK(9, 5)
#define UNICAM_ICST_MASK GENMASK(12, 10)
#define UNICAM_ICLT_MASK GENMASK(15, 13)
#define UNICAM_ICLL_MASK GENMASK(31, 16)

/* UNICAM_DCS Register */
#define UNICAM_DIE BIT(0)
#define UNICAM_DIM BIT(1)
#define UNICAM_DBOB BIT(3)
#define UNICAM_FDE BIT(4)
#define UNICAM_LDP BIT(5)
#define UNICAM_EDL_MASK GENMASK(15, 8)

/* UNICAM_DBCTL Register */
#define UNICAM_DBEN BIT(0)
#define UNICAM_BUF0_IE BIT(1)
#define UNICAM_BUF1_IE BIT(2)

/* UNICAM_CMP[0,1] register */
#define UNICAM_PCE BIT(31)
#define UNICAM_GI BIT(9)
#define UNICAM_CPH BIT(8)
#define UNICAM_PCVC_MASK GENMASK(7, 6)
#define UNICAM_PCDT_MASK GENMASK(5, 0)

/* UNICAM_MISC register */
#define UNICAM_FL0 BIT(6)
#define UNICAM_FL1 BIT(9)


#define BIT(n) (UINT32_C(1) << (n))
#define u32 uint32_t
#define AARCH 32
#define GENMASK(h, l) ((~0 - (1 << (l)) + 1) & (~0 >> (AARCH - 1 - (h))))

#define ARM_IO_BASE (uintptr_t)0xf2000000
#define ARM_CSI0_BASE (ARM_IO_BASE + 0x800000)
#define ARM_CSI0_END (ARM_CSI0_BASE + 0x7FF)
#define ARM_CSI0_CLKGATE (ARM_IO_BASE + 0x802000) // 4 bytes
#define ARM_CSI1_BASE (ARM_IO_BASE + 0x801000)
#define ARM_CSI1_END (ARM_CSI1_BASE + 0x7FF)
#define ARM_CSI1_CLKGATE (ARM_IO_BASE + 0x802004) // 4 bytes
#define ARM_CM_BASE (ARM_IO_BASE + 0x101000)
#define ARM_CM_CAM0CTL (ARM_CM_BASE + 0x40)
#define ARM_CM_CAM0DIV (ARM_CM_BASE + 0x44)
#define ARM_CM_CAM1CTL (ARM_CM_BASE + 0x48)
#define ARM_CM_CAM1DIV (ARM_CM_BASE + 0x4C)
#define ARM_CM_PASSWD (0x5A << 24)

static void usleep(uint64_t delta)
{
    volatile struct
    {
        uint32_t LO;
        uint32_t HI;
    } *CLOCK = (volatile void *)0xf2003004;

    uint64_t hi = CLOCK->HI;
    uint64_t lo = CLOCK->LO;
    uint64_t t1, t2;

    if (unlikely(hi != CLOCK->HI))
    {
        hi = CLOCK->HI;
        lo = CLOCK->LO;
    }

    t1 = LE64(hi) | LE32(lo);
    t1 += delta;
    t2 = 0;

    do
    {
        hi = CLOCK->HI;
        lo = CLOCK->LO;
        if (unlikely(hi != CLOCK->HI))
        {
            hi = CLOCK->HI;
            lo = CLOCK->LO;
        }
        t2 = LE64(hi) | LE32(lo);
    } while (t2 < t1);
}

void setup_csiclk()
{
    *(volatile uint32_t *)(ARM_CM_CAM1CTL) = LE32(ARM_CM_PASSWD | (1 << 5));
    usleep(100);
    while ((*(volatile uint32_t *)(ARM_CM_CAM1CTL)) & LE32(1 << 7)) {}
    usleep(100);
    *(volatile uint32_t *)(ARM_CM_CAM1DIV) =
        LE32(ARM_CM_PASSWD | (4 << 12)); // divider , 12=100MHz on pi3 ??
    usleep(100);
    *(volatile uint32_t *)(ARM_CM_CAM1CTL) =
        LE32(ARM_CM_PASSWD | 6 | (1 << 4)); // pll? 6=plld, 5=pllc
    usleep(100);
    while (((*(volatile uint32_t *)(ARM_CM_CAM1CTL)) & LE32(1 << 7)) == 0) {}
    usleep(100);
}

void ClockWrite(uint32_t nValue)
{
    *(volatile uint32_t *)(ARM_CSI1_CLKGATE) = LE32(ARM_CM_PASSWD | nValue);
}


void SetField(uint32_t *pValue, uint32_t nValue, uint32_t nMask)
{
    uint32_t nTempMask = nMask;
    while (!(nTempMask & 1)) {
        nValue <<= 1;
        nTempMask >>= 1;
    }

    *pValue = (*pValue & ~nMask) | nValue;
}

uint32_t ReadReg(uint32_t nOffset)
{
    uint32_t temp;
    temp = LE32(*(volatile uint32_t *)(ARM_CSI1_BASE + nOffset));

    return temp;
}

void WriteReg(uint32_t nOffset, uint32_t nValue)
{
    *(volatile uint32_t *)(ARM_CSI1_BASE + nOffset) = LE32(nValue);
}

void WriteRegField(uint32_t nOffset, uint32_t nValue, uint32_t nMask)
{
    uint32_t nBuffer = ReadReg(nOffset);
    SetField(&nBuffer, nValue, nMask);
    WriteReg(nOffset, nBuffer);
}


void unicam_run(uintptr_t address , uint8_t lanes, uint8_t datatype, uint32_t width , uint32_t height , uint8_t bbp)
{
    //enable power domain
    enable_unicam_domain();

    //enable to clock to unicam
    setup_csiclk();

    // Enable lane clocks (2 lanes)
    ClockWrite(0b010101);

    // Basic init
    WriteReg(UNICAM_CTRL, UNICAM_MEM);

    // Enable analogue control, and leave in reset.
    uint32_t nValue = UNICAM_AR;
    SetField(&nValue, 7, UNICAM_CTATADJ_MASK);
    SetField(&nValue, 7, UNICAM_PTATADJ_MASK);
    WriteReg(UNICAM_ANA, nValue);

    usleep(1000);

    // Come out of reset
    WriteRegField(UNICAM_ANA, 0, UNICAM_AR);

    // Peripheral reset
    WriteRegField(UNICAM_CTRL, 1, UNICAM_CPR);
    WriteRegField(UNICAM_CTRL, 0, UNICAM_CPR);

    WriteRegField(UNICAM_CTRL, 0, UNICAM_CPE);

    // Enable Rx control (CSI2 DPHY)
    nValue = ReadReg(UNICAM_CTRL);
    SetField(&nValue, UNICAM_CPM_CSI2, UNICAM_CPM_MASK);
    SetField(&nValue, UNICAM_DCM_STROBE, UNICAM_DCM_MASK);

    // Packet framer timeout
    SetField(&nValue, 0xf, UNICAM_PFT_MASK);
    SetField(&nValue, 128, UNICAM_OET_MASK);
    WriteReg(UNICAM_CTRL, nValue);

    WriteReg(UNICAM_IHWIN, 0);
    WriteReg(UNICAM_IVWIN, 0);

    // AXI bus access QoS setup
    nValue = ReadReg(UNICAM_PRI);
    SetField(&nValue, 0, UNICAM_BL_MASK);
    SetField(&nValue, 0, UNICAM_BS_MASK);
    SetField(&nValue, 0xe, UNICAM_PP_MASK);
    SetField(&nValue, 8, UNICAM_NP_MASK);
    SetField(&nValue, 2, UNICAM_PT_MASK);
    SetField(&nValue, 1, UNICAM_PE);
    WriteReg(UNICAM_PRI, nValue);

    WriteRegField(UNICAM_ANA, 0, UNICAM_DDL);

    uint32_t nLineIntFreq = height >> 2;
    nValue = UNICAM_FSIE | UNICAM_FEIE | UNICAM_IBOB;
    SetField(&nValue, nLineIntFreq >= 128 ? nLineIntFreq : 128, UNICAM_LCIE_MASK);
    WriteReg(UNICAM_ICTL, nValue);
    WriteReg(UNICAM_STA, UNICAM_STA_MASK_ALL);
    WriteReg(UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);

    WriteRegField(UNICAM_CLT, 2, UNICAM_CLT1_MASK); // tclk_term_en
    WriteRegField(UNICAM_CLT, 6, UNICAM_CLT2_MASK); // tclk_settle
    WriteRegField(UNICAM_DLT, 2, UNICAM_DLT1_MASK); // td_term_en
    WriteRegField(UNICAM_DLT, 6, UNICAM_DLT2_MASK); // ths_settle
    WriteRegField(UNICAM_DLT, 0, UNICAM_DLT3_MASK); // trx_enable

    WriteRegField(UNICAM_CTRL, 0, UNICAM_SOE);

    // Packet compare setup - required to avoid missing frame ends
    nValue = 0;
    SetField(&nValue, 1, UNICAM_PCE);
    SetField(&nValue, 1, UNICAM_GI);
    SetField(&nValue, 1, UNICAM_CPH);
    SetField(&nValue, 0, UNICAM_PCVC_MASK);
    SetField(&nValue, 1, UNICAM_PCDT_MASK);
    WriteReg(UNICAM_CMP0, nValue);

    // Enable clock lane and set up terminations (CSI2 DPHY, non-continous clock)
    nValue = 0;
    SetField(&nValue, 1, UNICAM_CLE);
    SetField(&nValue, 1, UNICAM_CLLPE);
    WriteReg(UNICAM_CLK, nValue);

    // Enable required data lanes with appropriate terminations.
    // The same value needs to be written to UNICAM_DATn registers for
    // the active lanes, and 0 for inactive ones.
    // (CSI2 DPHY, non-continous clock, 2 data lanes)
    nValue = 0;
    SetField(&nValue, 1, UNICAM_DLE);
    SetField(&nValue, 1, UNICAM_DLLPE);
    WriteReg(UNICAM_DAT0, nValue);
    if (lanes == 1)
        WriteReg(UNICAM_DAT1, 0);
    if (lanes == 2)
        WriteReg(UNICAM_DAT1, nValue);

    WriteReg(UNICAM_IBLS, width*(bbp/8));

    // Write DMA buffer address

    WriteReg(UNICAM_IBSA0, ((u32)(address) & ~0xC0000000) | 0xC0000000);
    WriteReg(UNICAM_IBEA0,
            ((uint32_t)(address + (width * height * (bbp/8))) & ~0xC0000000) | 0xC0000000);

    // Set packing configuration
    uint32_t nUnPack = UNICAM_PUM_NONE;
    uint32_t nPack = UNICAM_PPM_NONE;

    nValue = 0;
    SetField(&nValue, nUnPack, UNICAM_PUM_MASK);
    SetField(&nValue, nPack, UNICAM_PPM_MASK);
    WriteReg(UNICAM_IPIPE, nValue);

    // CSI2 mode, hardcode VC 0 for now.
    WriteReg(UNICAM_IDI0, (0 << 6) | datatype);

    nValue = ReadReg(UNICAM_MISC);
    SetField(&nValue, 1, UNICAM_FL0);
    SetField(&nValue, 1, UNICAM_FL1);
    WriteReg(UNICAM_MISC, nValue);

    // Clear ED setup
    WriteReg(UNICAM_DCS, 0);

    // Enable peripheral
    WriteRegField(UNICAM_CTRL, 1, UNICAM_CPE);

    // Load image pointers
    WriteRegField(UNICAM_ICTL, 1, UNICAM_LIP_MASK);
}

#define UNICAM_MODE     0x22
#define UNICAM_WIDTH    720
#define UNICAM_HEIGHT   576
#define UNICAM_BPP      16

/* 
    Report stealth mode - in normal cases add a text on the screen, unless user requested 
    framethrower support during boot. In that case, set the framethrower output.
*/
void platform_report_stealth()
{
    int unicam_initialized = 0;
    of_property_t *prop = dt_find_property(dt_find_node("/emu68/unicam"), "status");

    if (prop && strcmp(prop->op_value, "okay") == 0)
    {
        struct Size sz = { 720, 576 };
        void *framebuffer = NULL;
        uint32_t pitch = 0;

        init_display(sz, &framebuffer, &pitch);

        setup_csiclk();
        unicam_run((uintptr_t)framebuffer, 1, UNICAM_MODE, 720, 576, UNICAM_BPP);
        
        unicam_initialized = 1;
    }

    if (!unicam_initialized)
    {
        struct Size sz = get_display_size();
        uint32_t last_x = text_x;
        uint32_t last_y = text_y;

        /* Get the logo height for stealth mode. Assume PiStorm */
        uint32_t logo_h = 10;
        logo_h += (logo_emu68[2] << 8) + logo_emu68[3];
        logo_h += logo_pistorm[2] << 8 | logo_pistorm[3];

        uint32_t start_y = (sz.height + logo_h) / 2;
        
        text_y = start_y / 16;
        text_x = (sz.width - 20 * 8) / 16;

        kprintf_pc(__putc, NULL, "!!! STEALTH MODE !!!");

        text_x = last_x;
        text_y = last_y;
    }
}
