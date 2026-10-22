/* Real passive USB report capture logic tested on x86 and AArch64 BE. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
extern void frf_usb_packet_capture(uint8_t,uint8_t,uint8_t,const uint8_t*,uint16_t);
extern void frf_usb_packet_capture_snapshot(uint32_t out[55]);
extern void frf_usb_target_capture(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern uint32_t frf_usb_target_rx_count(void);
int main(void)
{
    uint32_t s[55]={0};
    frf_usb_packet_capture_snapshot(s);
    assert(s[0]==0u && s[1]==0u);
    uint8_t hid[10]={1,2,3,4,5,6,7,8,9,10};
    frf_usb_packet_capture(1u,3u,0u,hid,10u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[0]==1u && s[1]==1u && s[3]==0x03000a01u);
    assert(s[7]==0x01020304u && s[8]==0x01020304u && s[9]==0u);
    uint8_t vendor[64]={0};
    vendor[0]=0x20u;vendor[1]=0x11u;vendor[5]=0x42u;
    frf_usb_packet_capture(2u,4u,0x81u,vendor,64u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[0]==2u && s[1]==1u && s[3]==0x04814020u);
    assert(s[7]==0x20110000u && s[8]==0x20110000u && s[9]==0);
    vendor[5]=0x43u;
    vendor[12]=0x80u;
    frf_usb_packet_capture(2u,4u,0x81u,vendor,64u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[1]==2u && s[2]==1u && s[4]==2u);
    assert(s[7+3]==0x00420000u);
    assert(s[7+3+1]==0x00430000u);
    assert(s[7+3+2]==0x00010000u);
    assert(s[7+9]==0x00000000u);
    assert(s[7+9+1]==0x80000000u);
    assert(s[7+9+2]==0x80000000u);
    /* new report ID switches comparison baseline (never flag fake diff) */
    vendor[0]=0x21u;
    frf_usb_packet_capture(2u,4u,0x81u,vendor,64u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[1]==3u && s[9]==0u && s[2]==1u);
    /* R35: same receiver, independent ID01 and ID02 baselines. */
    uint8_t id1[27]={1,0,15,128,128};
    uint8_t id2[27]={2,0,7,128,128};
    frf_usb_target_capture(1u,0u,id1,27u);
    frf_usb_target_capture(1u,0u,id2,27u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[0]==3u && s[3]==0x01001b02u && s[9]==0u);
    id1[1]=0x04u;
    frf_usb_target_capture(1u,0u,id1,27u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[0]==3u && s[3]==0x01001b01u);
    assert(s[7]==0x01000f80u && s[8]==0x01040f80u && s[9]==0x00040000u);
    frf_usb_target_capture(1u,0u,id2,27u);
    frf_usb_packet_capture_snapshot(s);
    assert(s[3]==0x01001b01u && s[9]==0x00040000u);
    assert(frf_usb_target_rx_count()==4u);
    puts("R18 64-byte REAL USB HID/VENDOR baseline/current/XOR packet delta PASS");
}
