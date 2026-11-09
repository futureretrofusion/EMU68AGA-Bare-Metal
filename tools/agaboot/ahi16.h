/* agaboot - HiFi sound: AHI programs straight to HDMI (ahi16.c)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_AHI16_H
#define AGABOOT_AHI16_H

int  ahi16_pref(void);                          /* ENVARC:AGA_AHI16: 0 off, 1 ON, 2 LOWLATENCY */
/* On: the driver and its modes into DEVS:, AHI's units 0 and Music at the
   HiFi HDMI mode; Off: those units back to what they had. msg gets what came
   of it ("" for nothing to say). 0 fine, 5 a word of warning, 20 failed. */
int  ahi16_set(int on, char *msg, int len);
/* at boot (MENU), when it is on: the driver and its modes as this agaboot
   carries them - an update brings a new driver with it */
void ahi16_refresh(void);
void ahi16_check(void);                         /* agaboot CHECK's lines */

#endif
