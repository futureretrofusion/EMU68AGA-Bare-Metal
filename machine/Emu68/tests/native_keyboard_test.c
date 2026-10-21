#include "../src/frf_usb/native/frf_native_keyboard.h"
#include <assert.h>
#include <stdio.h>
static uint8_t received[64];
static unsigned count;
static void emit(void *unused, uint8_t c) { (void)unused; received[count++]=c; }
int main(void)
{
  struct frf_native_keyboard s = {0};
  const uint8_t press[8]={0x02,0,0x04,0,0,0,0,0}; /* Shift+A */
  const uint8_t release[8]={0};
  const uint8_t rollover[8]={0,0,0x01,0,0,0,0,0};
  frf_native_keyboard_report(&s, press, emit, 0);
  assert(count==2 && received[0]==0x60 && received[1]==0x20);
  frf_native_keyboard_report(&s, press, emit, 0);
  assert(count==2);
  frf_native_keyboard_report(&s, rollover, emit, 0);
  assert(count==2 && s.reports==2);
  frf_native_keyboard_report(&s, release, emit, 0);
  assert(count==4 && received[2]==0xe0 && received[3]==0xa0);
  /* Ctrl (left or right) + left Shift + F11, strictly edge-triggered. */
  const uint8_t chord[8]={0x03,0,0x44,0,0,0,0,0};
  const uint8_t rctrl[8]={0x12,0,0x44,0,0,0,0,0};
  const uint8_t wrongshift[8]={0x05,0,0x44,0,0,0,0,0};
  frf_native_keyboard_report(&s,chord,emit,0);
  assert(frf_native_keyboard_take_reboot(&s)==1);
  assert(frf_native_keyboard_take_reboot(&s)==0);
  frf_native_keyboard_report(&s,chord,emit,0);
  assert(frf_native_keyboard_take_reboot(&s)==0);
  frf_native_keyboard_report(&s,release,emit,0);
  frf_native_keyboard_report(&s,rctrl,emit,0);
  assert(frf_native_keyboard_take_reboot(&s)==1);
  frf_native_keyboard_report(&s,wrongshift,emit,0);
  assert(frf_native_keyboard_take_reboot(&s)==0);
  puts("native Ctrl+LeftShift+F11 host reboot edge test PASS");
  puts("native HID keyboard translation PASS");
  return 0;
}
