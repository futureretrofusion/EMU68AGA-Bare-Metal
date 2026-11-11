#include <stdint.h>
#include "frf_services.h"
#include "frf_usb_boot.h"
volatile uint8_t frf_usb_host_start_allowed;
volatile uint8_t frf_usb_boot_ready;
volatile uint8_t frf_usb_mouse_present;
volatile uint8_t frf_usb_mouse_seen;
volatile uint8_t frf_usb_mouse_buttons;
int frf_usb_early_startup_active(void) { return 0; }
void frf_services_usb_backend_state(uint32_t state) { (void)state; }
int frf_usb_boot_cpu1_scan(void) { return 0; }
int frf_usb_boot_cpu1_load(uint32_t index, void *dst, uint32_t capacity) { (void)index; (void)dst; (void)capacity; return 0; }
void frf_usb_boot_clear(void) {}
int frf_usb_boot_cpu1_read_physical_blocks(uint64_t lba, uint16_t count, void *dst) { (void)lba; (void)count; (void)dst; return 0; }
int frf_usb_boot_cpu1_read_hdf_blocks(uint32_t index, uint64_t lba, uint16_t count, void *dst) { (void)index; (void)lba; (void)count; (void)dst; return 0; }

/* R76: xHCI metadata for async, standalone Pi4 input worker.
 * Retained in RAM so boot success is not blocked by diagnostics. */
volatile uint32_t frf_sdlog_xhci_stage;
volatile uint32_t frf_sdlog_xhci_error;
volatile uint32_t frf_r76_usb_keyboard_count,frf_r76_usb_mouse_count;
volatile uint32_t frf_r76_usb_gamepad_count,frf_r76_usb_mass_count;
#if defined(FRF_BOOT_POST)
extern void frf_boot_post_devices(void);
#endif
void frf_post_usb_devices(unsigned kb,unsigned ms,unsigned gp,unsigned msc) {
    __atomic_store_n(&frf_r76_usb_keyboard_count,kb,__ATOMIC_RELEASE);
    __atomic_store_n(&frf_r76_usb_mouse_count,ms,__ATOMIC_RELEASE);
    __atomic_store_n(&frf_r76_usb_gamepad_count,gp,__ATOMIC_RELEASE);
    __atomic_store_n(&frf_r76_usb_mass_count,msc,__ATOMIC_RELEASE);
#if defined(FRF_BOOT_POST)
    frf_boot_post_devices();
#endif
}
