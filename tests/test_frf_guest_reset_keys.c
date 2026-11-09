/* FRF guest-only reset HID regressions.
 * Build: cc -std=c11 -Wall -Wextra -Werror -I machine/Emu68/src/frf_usb/native
 *   tests/test_frf_guest_reset_keys.c
 *   machine/Emu68/src/frf_usb/native/frf_native_keyboard.c -o /tmp/...
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "frf_native_keyboard.h"
static unsigned guest_resets,osd_opens;
void frf_vcia_guest_soft_reset_request(void){++guest_resets;}
void frf_vcia_osd_hotkey(void){++osd_opens;}
static void emit(void*ctx,uint8_t raw){(void)ctx;(void)raw;}
static void report(struct frf_native_keyboard*s,uint8_t mod,uint8_t key)
{
    uint8_t r[8]={0};r[0]=mod;r[2]=key;
    frf_native_keyboard_report(s,r,emit,0);
}
int main(void)
{
    struct frf_native_keyboard s={0};
    /* Ctrl + LWin + RWin is one guest reset on rising edge. */
    report(&s,0x89,0);assert(guest_resets==1);
    report(&s,0x89,0);assert(guest_resets==1);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    report(&s,0x09,0);report(&s,0x89,0);
    assert(guest_resets==2);
    report(&s,0,0);
    /* Ctrl+LeftAlt+F11 is also a guest reset, never Pi reset. */
    report(&s,0x05,0x44);assert(guest_resets==3);
    report(&s,0x05,0x44);assert(guest_resets==3);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    report(&s,0x05,0);report(&s,0x05,0x44);
    assert(guest_resets==4);
    report(&s,0,0);
    /* Ctrl+LEFT Shift+F11 remains a distinct Pi reboot. */
    report(&s,0x03,0x44);assert(guest_resets==4);
    assert(frf_native_keyboard_take_reboot(&s)==1);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    report(&s,0,0);
    /* OSD F12 does not reset either machine. */
    report(&s,0x05,0x45);assert(guest_resets==4);
    assert(osd_opens==1);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    report(&s,0,0);
    /* If Alt+Shift are both held, guest soft reset wins: never reboot Pi. */
    report(&s,0x07,0x44);assert(guest_resets==5);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    report(&s,0,0);
    /* Right Ctrl is a valid Amiga soft-reset modifier. */
    report(&s,0x14,0x44);assert(guest_resets==6);
    assert(frf_native_keyboard_take_reboot(&s)==0);
    puts("PASS: Ctrl+LWin+RWin and Ctrl+LAlt+F11 guest reset; host reboot isolated");
    return 0;
}
