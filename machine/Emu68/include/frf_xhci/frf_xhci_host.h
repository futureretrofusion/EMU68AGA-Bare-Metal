#ifndef FRF_XHCI_HOST_H
#define FRF_XHCI_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FRF CPU1 xHCI Host V0.4 / God Mode V0.2
 *
 * Controlled-write first host stage for the BCM2711 internal xHCI.
 * All controller ownership operations are executed by CPU1.  CPU0/God Mode
 * only posts bounded requests and reads the resulting snapshot.
 */

enum {
    FRFXHCI_HOST_OFFLINE = 0,
    FRFXHCI_HOST_PROBED = 1,
    FRFXHCI_HOST_OWNED = 2,
    FRFXHCI_HOST_DEVICE = 3,
    FRFXHCI_HOST_MASS_STORAGE = 4,
    FRFXHCI_HOST_ERROR = 5
};

enum {
    FRFXHCI_HE_OK = 0,
    FRFXHCI_HE_NOT_CPU1 = 1,
    FRFXHCI_HE_PROBE = 2,
    FRFXHCI_HE_NO_DEVICE = 3,
    FRFXHCI_HE_HALT_TIMEOUT = 4,
    FRFXHCI_HE_RESET_TIMEOUT = 5,
    FRFXHCI_HE_CNR_TIMEOUT = 6,
    FRFXHCI_HE_PAGE_SIZE = 7,
    FRFXHCI_HE_DMA_ADDRESS = 8,
    FRFXHCI_HE_SCRATCHPAD = 9,
    FRFXHCI_HE_RUN_TIMEOUT = 10,
    FRFXHCI_HE_PORT_RESET = 11,
    FRFXHCI_HE_COMMAND_TIMEOUT = 12,
    FRFXHCI_HE_COMMAND_FAILED = 13,
    FRFXHCI_HE_ADDRESS_DEVICE = 14,
    FRFXHCI_HE_CONTROL_TRANSFER = 15,
    FRFXHCI_HE_DESCRIPTOR = 16,
    FRFXHCI_HE_CONFIG_ENDPOINT = 17,
    FRFXHCI_HE_REQUEST_BUSY = 18,
    FRFXHCI_HE_REQUEST_TIMEOUT = 19,
    FRFXHCI_HE_UNSUPPORTED = 20
};

#define FRFXHCI_HOST_STRING_BYTES 64U

struct FRFXHCIHostInfo {
    uint32_t state;
    uint32_t error;
    uint32_t generation;
    uint32_t owned;
    uint32_t port_connected;
    uint32_t port_enabled;
    uint32_t speed_id;
    uint32_t slot_id;
    uint32_t usb_address;
    uint32_t vid;
    uint32_t pid;
    uint32_t bcd_usb;
    uint32_t device_class;
    uint32_t device_subclass;
    uint32_t device_protocol;
    uint32_t configuration_value;
    uint32_t interface_number;
    uint32_t interface_class;
    uint32_t interface_subclass;
    uint32_t interface_protocol;
    uint32_t ep0_mps;
    uint32_t bulk_in_ep;
    uint32_t bulk_out_ep;
    uint32_t bulk_in_mps;
    uint32_t bulk_out_mps;
    uint32_t msc_ready;
    char manufacturer[FRFXHCI_HOST_STRING_BYTES];
    char product[FRFXHCI_HOST_STRING_BYTES];
    char serial[FRFXHCI_HOST_STRING_BYTES];
};

/* Called by the CPU1 native-services worker. Returns non-zero if it consumed
 * a posted host request. */
int frf_xhci_host_cpu1_work(void);

/* CPU0/God Mode request API.  These requests wake CPU1 with SEV and block
 * only until the bounded operation is complete or timeout_ms expires. */
int frf_xhci_host_request_scan(uint32_t timeout_ms);
int frf_xhci_host_request_load_file(uint32_t index, void *dst,
                                    uint32_t capacity, uint32_t timeout_ms);

const volatile struct FRFXHCIHostInfo *frf_xhci_host_info(void);
int frf_xhci_host_hid_scan_cpu1(void);
int frf_xhci_host_hid_ready(void);
int frf_xhci_host_hid_poll(void *data, uint32_t length, uint32_t *actual);
/* R80 ARM CPU1 native multi-HID service: up to four concurrently polled
 * interrupt-IN endpoints behind Pi4 VL805 internal USB2 hub. */
uint32_t frf_xhci_host_native_count(void);
int frf_xhci_host_native_info(uint32_t index,uint32_t *proto,uint32_t *iface);
int frf_xhci_host_native_identity(uint32_t index,uint32_t *vid,
                                  uint32_t *pid,uint32_t *report_desc_length);
int frf_xhci_host_native_control(uint32_t index,
        uint8_t request_type,uint8_t request,uint16_t value,uint16_t ifnum,
        void *data,uint16_t length,int direction_in,uint32_t *actual);
int frf_xhci_host_native_poll_any(void *dst,uint32_t capacity,
        uint32_t *actual,uint32_t *device,uint32_t *protocol);
/* R96 Switch Pro 057e:2009 USB init, before async HID polling starts. */
int frf_xhci_host_native_interrupt_out(uint32_t index,const uint8_t *bytes,
                                       uint32_t length);
int frf_xhci_host_native_interrupt_in_once(uint32_t index,
             uint8_t *bytes,uint32_t capacity,uint32_t *actual);


/* CPU1-only transport used by the read-only USB boot layer after successful
 * enumeration. */
int frf_xhci_host_control(uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length, int direction_in,
                          uint32_t *actual);
int frf_xhci_host_bulk_in(void *data, uint32_t length, uint32_t *actual);
int frf_xhci_host_bulk_out(const void *data, uint32_t length, uint32_t *actual);

#ifdef __cplusplus
}
#endif



/* FRF_CPU1_USB_HUB_SELFCONTAINED_V0_5_5: one-tier boot enumeration telemetry. */
#define FRFXHCI_HUB_MAX_DEVICES 8U
struct FRFXHCIHubDeviceInfo {
    uint32_t valid;
    uint32_t hub_port;
    uint32_t slot_id;
    uint32_t speed_id;
    uint32_t vid;
    uint32_t pid;
    uint32_t device_class;
    uint32_t device_subclass;
    uint32_t device_protocol;
    uint32_t interface_class;
    uint32_t interface_subclass;
    uint32_t interface_protocol;
    uint32_t mass_storage;
    uint32_t is_hub;
    char manufacturer[64];
    char product[96];
    char serial[64];
};
uint32_t frf_xhci_host_device_count(void);
const volatile struct FRFXHCIHubDeviceInfo *frf_xhci_host_device(uint32_t index);
uint32_t frf_xhci_host_hub_port_count(void);
#endif
