/* R23 portable keyboard HID input firewall. */
#ifndef FRF_R23_KBD_GUARD_H
#define FRF_R23_KBD_GUARD_H
#include <stdint.h>
static inline int frf_r23_keyboard_allowed(uint16_t vid,uint16_t pid,
                                            unsigned protocol,unsigned len)
{
    /* Genuine boot keyboards use exactly 8 bytes. Never interpret the
       observed 7E05:0920 controller's composite HID bytes as keys. */
    return protocol==1u && len==8u &&
           !(vid==0x7e05u && pid==0x0920u);
}
#endif
