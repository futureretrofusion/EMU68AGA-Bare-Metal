#include "frf_native_usb_input.h"
#include <stdint.h>
extern void frf_vcia_keyboard_raw(uint8_t raw);
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
extern void aga_virtual_joy_set(int port, uint16_t joydat, int fire);
extern void aga_virtual_pad_buttons(int port,uint32_t buttons);

volatile uint32_t frf_native_usb_key_reports;
volatile uint32_t frf_native_usb_key_events;
volatile uint32_t frf_native_usb_mouse_reports;
volatile uint32_t frf_native_usb_pad_reports;
/* Pi4 ARM host-only hard-reset shortcut: LShift + either Ctrl + F11.
 * CPU1's keyboard parser sets the flag, CPU1 USB worker executes
 * aga_reboot(0) outside the HID report callback. */
volatile uint32_t frf_native_usb_arm_reset_pending;
volatile uint32_t frf_native_usb_arm_reset_count;
static uint8_t arm_reset_chord_latched;
static uint8_t last_mod,last_keys[6];
static uint8_t hid_to_amiga(uint8_t h)
{
    if (h >= 4u && h <= 0x1du) {
        static const uint8_t alpha[26] = {
            0x20,0x35,0x33,0x22,0x12,0x23,0x24,0x25,0x17,0x26,0x27,0x28,0x37,
            0x36,0x18,0x19,0x10,0x13,0x21,0x14,0x16,0x34,0x11,0x32,0x15,0x31 };
        return alpha[h-4u];
    }
    if (h >= 0x1eu && h <= 0x26u) return (uint8_t)(h-0x1du);
    switch (h) {
    case 0x27: return 0x0a; case 0x28: return 0x44; case 0x29: return 0x45;
    case 0x2a: return 0x41; case 0x2b: return 0x42; case 0x2c: return 0x40;
    case 0x2d: return 0x0b; case 0x2e: return 0x0c; case 0x2f: return 0x1a;
    case 0x30: return 0x1b; case 0x31: return 0x0d; case 0x32: return 0x30;
    case 0x33: return 0x29; case 0x34: return 0x2a; case 0x35: return 0x00;
    case 0x36: return 0x38; case 0x37: return 0x39; case 0x38: return 0x3a;
    case 0x39: return 0x62;
    case 0x3a: return 0x50; case 0x3b: return 0x51; case 0x3c: return 0x52;
    case 0x3d: return 0x53; case 0x3e: return 0x54; case 0x3f: return 0x55;
    case 0x40: return 0x56; case 0x41: return 0x57; case 0x42: return 0x58;
    case 0x43: return 0x59;
    case 0x49: return 0x5f; case 0x4c: return 0x46; case 0x4f: return 0x4e;
    case 0x50: return 0x4f; case 0x51: return 0x4d; case 0x52: return 0x4c;
    case 0x54: return 0x5c; case 0x55: return 0x5d; case 0x56: return 0x4a;
    case 0x57: return 0x5e; case 0x58: return 0x43;
    case 0x59: return 0x1d; case 0x5a: return 0x1e; case 0x5b: return 0x1f;
    case 0x5c: return 0x2d; case 0x5d: return 0x2e; case 0x5e: return 0x2f;
    case 0x5f: return 0x3d; case 0x60: return 0x3e; case 0x61: return 0x3f;
    case 0x62: return 0x0f; case 0x63: return 0x3c;
    default: return 0xffu;
    }
}
static int key_in(const uint8_t keys[6],uint8_t key)
{
    for(unsigned i=0;i<6u;i++) if(keys[i]==key) return 1;
    return 0;
}
static void emit_key(uint8_t raw,int release)
{
    if(raw==0xffu) return;
    frf_vcia_keyboard_raw((uint8_t)(raw|(release?0x80u:0u)));
    __atomic_add_fetch(&frf_native_usb_key_events,1u,__ATOMIC_RELAXED);
}
void frf_native_usb_keyboard_report(const uint8_t report[8])
{
    if (!report) return;
    const uint8_t mod=report[0],*keys=&report[2];
    /* HID rollover/error usages must not release all held keys. */
    for(unsigned i=0;i<6u;i++) if(keys[i]>=1u && keys[i]<=3u) return;
    /* USB HID boot keyboard: F11 usage 0x44, modifier bit1=LShift,
       bits0/4=left/right Ctrl. The left shift requirement is intentional.
       Detect held chord on any report, even if a modifier was pressed
       after F11. Trigger only once until released. The shortcut belongs
       to the ARM host, never an Amiga raw-key or soft reset. */
    const int arm_chord=(mod&0x02u) && (mod&0x11u) &&
                        key_in(keys,0x44u);
    if(arm_chord && !arm_reset_chord_latched) {
        arm_reset_chord_latched=1u;
        __atomic_add_fetch(&frf_native_usb_arm_reset_count,1u,__ATOMIC_RELAXED);
        __atomic_store_n(&frf_native_usb_arm_reset_pending,1u,__ATOMIC_RELEASE);
    } else if(!arm_chord) {
        arm_reset_chord_latched=0u;
    }
    const uint8_t mods[8]={0x63,0x60,0x64,0x66,0x63,0x61,0x65,0x67};
    uint8_t changed=mod^last_mod;
    for(unsigned b=0;b<8u;b++) if(changed & (1u<<b))
        emit_key(mods[b],!(mod & (1u<<b)));
    for(unsigned i=0;i<6u;i++) {
        uint8_t k=last_keys[i];
        if(k<4u || key_in(keys,k) || k==0x53u || k==0x47u) continue;
        emit_key(hid_to_amiga(k),1);
    }
    for(unsigned i=0;i<6u;i++) {
        uint8_t k=keys[i];
        if(k<4u || key_in(last_keys,k) || k==0x53u || k==0x47u) continue;
        emit_key(hid_to_amiga(k),0);
    }
    last_mod=mod;
    for(unsigned i=0;i<6u;i++) last_keys[i]=keys[i];
    __atomic_add_fetch(&frf_native_usb_key_reports,1u,__ATOMIC_RELAXED);
}
void frf_native_usb_mouse_report(const uint8_t *r,unsigned length)
{
    if(!r || length<3u) return;
    /* USB HID boot protocol: button mask, signed X and Y deltas. */
    unsigned buttons=(unsigned)(r[0]&7u);
    aga_virtual_mouse_update((int8_t)r[1],(int8_t)r[2],buttons);
    __atomic_add_fetch(&frf_native_usb_mouse_reports,1u,__ATOMIC_RELAXED);
}
void frf_native_usb_pad_release(void)
{
    /* Wireless disconnect: release held stick/fire without incrementing
       report counters or modifying unrelated keyboard/mouse input. */
    aga_virtual_joy_set(1,0u,0);
    aga_virtual_pad_buttons(1,0u);
}
void frf_native_usb_pad_update_axes(int x,int y,int x2,int y2,
                                    unsigned buttons,int hat)
{
    /* Match the working Pi3 v0.10.56 axis 0/1 plus D-pad axis 2/3:
       20% deflection threshold, OR with HID hat. */
    int l=x<-32 || x2<-32,rr=x>32 || x2>32;
    int u=y<-32 || y2<-32,d=y>32 || y2>32;
    if(hat>=0 && hat<8) {
        u|=(hat==0||hat==1||hat==7);
        rr|=(hat==1||hat==2||hat==3);
        d|=(hat==3||hat==4||hat==5);
        l|=(hat==5||hat==6||hat==7);
    }
    /* Exactly the working Pi3 JOY1DAT quadrature encoding. */
    uint16_t joy=0;
    if(l) joy|=0x0200u;
    if(rr)joy|=0x0002u;
    if(!!l != !!u) joy|=0x0100u;
    if(!!rr != !!d)joy|=0x0001u;
    aga_virtual_joy_set(1,joy,(buttons&1u)!=0u);
    /* Pi3 port-2: button2=POTGOR14, button3=POTGOR12.
       Unlike the old four-buttons->fire collapse, these remain distinct. */
    aga_virtual_pad_buttons(1,buttons);
    __atomic_add_fetch(&frf_native_usb_pad_reports,1u,__ATOMIC_RELAXED);
}
void frf_native_usb_pad_update(int x,int y,unsigned buttons,int hat)
{
    frf_native_usb_pad_update_axes(x,y,0,0,buttons,hat);
}
