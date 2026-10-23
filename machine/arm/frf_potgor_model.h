#ifndef FRF_POTGOR_MODEL_H
#define FRF_POTGOR_MODEL_H
#include <stdint.h>

static inline uint16_t frf_potgor_model(uint16_t pg, uint8_t mouse_buttons,
                                        uint16_t pad_mask)
{
    uint16_t v = (uint16_t)((pg & 0xAA00u) | 0x5500u);

    /* Output-configured pins read back DAT. Input-configured pins may be
       pulled low by external mouse/pad switches. */
    if ((pg & 0x8000u) && !(pg & 0x4000u)) v &= (uint16_t)~0x4000u;
    if ((pg & 0x2000u) && !(pg & 0x1000u)) v &= (uint16_t)~0x1000u;
    if ((pg & 0x0800u) && !(pg & 0x0400u)) v &= (uint16_t)~0x0400u;
    if ((pg & 0x0200u) && !(pg & 0x0100u)) v &= (uint16_t)~0x0100u;

    if (mouse_buttons & 2u) v &= (uint16_t)~0x0400u;
    if (mouse_buttons & 4u) v &= (uint16_t)~0x0100u;

    /* Historical v0.10.56/RC13 compatibility: port-2 second fire
       (RY / POTGOR bit 14) is an external active-low switch and remains
       visible regardless of POTGO OUTRY state. */
    v &= (uint16_t)~(pad_mask & 0x4000u);

    /* Button 3 remains POTGO-aware for now. */
    if (!(pg & 0x2000u)) v &= (uint16_t)~(pad_mask & 0x1000u);
    if (!(pg & 0x0800u)) v &= (uint16_t)~(pad_mask & 0x0400u);
    if (!(pg & 0x0200u)) v &= (uint16_t)~(pad_mask & 0x0100u);
    return v;
}
#endif
