/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FRF_R19_HID_GAMEPAD_H
#define FRF_R19_HID_GAMEPAD_H
#include <stdint.h>
#define FRF_HID_FIELDS 48u
#define FRF_HID_REPORTS 256u
struct frf_hid_field {
 uint16_t bit;
 uint8_t width,kind,report_id;
 int32_t min,max;
};
struct frf_hid_map {
 uint8_t valid,count,has_ids,first_id;
 uint16_t report_bits[FRF_HID_REPORTS];
 struct frf_hid_field field[FRF_HID_FIELDS];
};
int frf_hid_gamepad_parse(struct frf_hid_map*,const uint8_t*,uint16_t);
int frf_hid_gamepad_decode(const struct frf_hid_map*,const uint8_t*,
                           uint16_t,uint16_t*,int*);
#endif
