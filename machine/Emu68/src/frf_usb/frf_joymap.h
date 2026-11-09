/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FRF_JOYMAP_H
#define FRF_JOYMAP_H
#include <stdint.h>
void frf_joymap_init(void);
uint32_t frf_joymap_apply(uint32_t decoded_buttons);
void frf_joymap_get(int out_bits[4], int *active);
#endif
