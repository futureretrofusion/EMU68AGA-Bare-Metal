/* FRF bare-metal native USB input translation shared by Pi3 / Pi4 host adaptors.
 * USB lives on ARM CPU1; callbacks inject guest hardware-shaped input.
 * This module does NOT link AmigaOS, Poseidon, Circle or USB HID OS drivers. */
#ifndef FRF_NATIVE_USB_INPUT_H
#define FRF_NATIVE_USB_INPUT_H
#include <stdint.h>
void frf_native_usb_keyboard_report(const uint8_t report[8]);
void frf_native_usb_mouse_report(const uint8_t *report, unsigned len);
void frf_native_usb_pad_update(int x, int y, unsigned buttons, int hat);
void frf_native_usb_pad_release(void);
/* Pi3-compatible axes 2/3 + port2 POTGOR extra fire buttons. */
void frf_native_usb_pad_update_axes(int x,int y,int x2,int y2,unsigned buttons,int hat);
void frf_native_usb_pad_set_identity_for(unsigned index,uint32_t vid,uint32_t pid);
int frf_native_usb_pad_enable_shanwan_for(unsigned index);
int frf_native_usb_pad_enable_switchpro_for(unsigned index);
int frf_native_usb_pad_set_descriptor(const uint8_t *bytes, unsigned length);
int frf_native_usb_pad_set_descriptor_for(unsigned index,const uint8_t *bytes,unsigned length);
int frf_native_usb_pad_report(const uint8_t *bytes, unsigned length);
int frf_native_usb_pad_report_for(unsigned index,const uint8_t *bytes,unsigned length);

extern volatile uint32_t frf_native_usb_key_reports;
extern volatile uint32_t frf_native_usb_key_events;
extern volatile uint32_t frf_native_usb_mouse_reports;
extern volatile uint32_t frf_native_usb_pad_reports;
extern volatile uint32_t frf_native_usb_arm_reset_pending;
extern volatile uint32_t frf_native_usb_arm_reset_count;
#endif
