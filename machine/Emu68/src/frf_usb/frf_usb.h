#ifndef FRF_USB_H
#define FRF_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void frf_usb_main(void) __attribute__((noreturn));
int  frf_uspi_irq_dispatch(void);
void frf_uspi_timer_service(void);
int  frf_uspi_env_init(void);

enum frf_usb_event_type {
    FRF_USB_EV_NONE = 0,
    FRF_USB_EV_KEYBOARD = 1,
    FRF_USB_EV_MOUSE = 2,
    FRF_USB_EV_GAMEPAD = 3
};

struct frf_usb_event {
    uint32_t type;
    uint32_t index;
    uint32_t a, b, c, d;
    uint64_t tick;
};

/* Future Amiga/menu input consumers call this. Returns 1 if one event was read. */
int frf_usb_event_pop(struct frf_usb_event *out);

#ifdef __cplusplus
}
#endif

#endif
