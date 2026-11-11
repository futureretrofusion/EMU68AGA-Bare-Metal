#ifndef FRF_SERVICES_ABI_H
#define FRF_SERVICES_ABI_H

#include <stdint.h>

#define FRF_SERVICE_MAGIC                0x46524653UL /* 'FRFS' */
#define FRF_SERVICE_ABI_VERSION          5UL

/* Existing V0.1 discovery/mailbox window: ABI is extended in place. */
#define FRF_SERVICE_DISCOVERY_BASE       0xFFF0DE00UL
#define FRFS_DISC_MAGIC                  (FRF_SERVICE_DISCOVERY_BASE + 0x00UL)
#define FRFS_DISC_ABI                    (FRF_SERVICE_DISCOVERY_BASE + 0x04UL)
#define FRFS_DISC_FEATURES               (FRF_SERVICE_DISCOVERY_BASE + 0x08UL)
#define FRFS_DISC_WORKER_CORE            (FRF_SERVICE_DISCOVERY_BASE + 0x0CUL)
#define FRFS_DISC_WORKER_ONLINE          (FRF_SERVICE_DISCOVERY_BASE + 0x10UL)
#define FRFS_DISC_COUNTER_HZ             (FRF_SERVICE_DISCOVERY_BASE + 0x14UL)
#define FRFS_DISC_SUBMITTED              (FRF_SERVICE_DISCOVERY_BASE + 0x18UL)
#define FRFS_DISC_COMPLETED              (FRF_SERVICE_DISCOVERY_BASE + 0x1CUL)
#define FRFS_DISC_ERRORS                 (FRF_SERVICE_DISCOVERY_BASE + 0x20UL)
#define FRFS_DISC_LAST_SEQUENCE          (FRF_SERVICE_DISCOVERY_BASE + 0x24UL)
#define FRFS_DISC_LAST_STATUS            (FRF_SERVICE_DISCOVERY_BASE + 0x28UL)
#define FRFS_DISC_DOORBELL               (FRF_SERVICE_DISCOVERY_BASE + 0x2CUL)
#define FRFS_DISC_REGISTER_BASE          (FRF_SERVICE_DISCOVERY_BASE + 0x30UL)
#define FRFS_DISC_REGISTER_BYTES         (FRF_SERVICE_DISCOVERY_BASE + 0x34UL)
#define FRFS_DISC_INPUT_BASE             (FRF_SERVICE_DISCOVERY_BASE + 0x38UL)
#define FRFS_DISC_INPUT_BYTES            (FRF_SERVICE_DISCOVERY_BASE + 0x3CUL)
#define FRFS_DISC_CPU1_IRQS              (FRF_SERVICE_DISCOVERY_BASE + 0x40UL)
#define FRFS_DISC_CPU1_IRQ_UNHANDLED     (FRF_SERVICE_DISCOVERY_BASE + 0x44UL)
#define FRFS_DISC_END                    (FRF_SERVICE_DISCOVERY_BASE + 0x48UL)

#define FRF_SERVICE_DOORBELL_ADDR        0xFFF0DE40UL

/* Existing single-producer (CPU0), single-consumer (CPU1) command mailbox. */
#define FRF_SERVICE_REGISTER_BASE        0xFFF0DE80UL
#define FRFS_REG_COMMAND                 (FRF_SERVICE_REGISTER_BASE + 0x00UL)
#define FRFS_REG_SEQUENCE                (FRF_SERVICE_REGISTER_BASE + 0x04UL)
#define FRFS_REG_ARG0                    (FRF_SERVICE_REGISTER_BASE + 0x08UL)
#define FRFS_REG_ARG1                    (FRF_SERVICE_REGISTER_BASE + 0x0CUL)
#define FRFS_REG_RESULT0                 (FRF_SERVICE_REGISTER_BASE + 0x10UL)
#define FRFS_REG_RESULT1                 (FRF_SERVICE_REGISTER_BASE + 0x14UL)
#define FRFS_REG_STATE                   (FRF_SERVICE_REGISTER_BASE + 0x18UL)
#define FRFS_REG_STATUS                  (FRF_SERVICE_REGISTER_BASE + 0x1CUL)
#define FRF_SERVICE_REGISTER_BYTES       0x20UL
#define FRFS_REG_END                     (FRF_SERVICE_REGISTER_BASE + FRF_SERVICE_REGISTER_BYTES)

/* V0.2 native input window. Kept separate from the V0.1 mailbox. */
#define FRF_INPUT_MAGIC                  0x494E5054UL /* 'INPT' */
#define FRF_INPUT_ABI_VERSION            4UL
#define FRF_INPUT_REGISTER_BASE          0xFFF0DF00UL
#define FRFS_IN_MAGIC                    (FRF_INPUT_REGISTER_BASE + 0x00UL)
#define FRFS_IN_ABI                      (FRF_INPUT_REGISTER_BASE + 0x04UL)
#define FRFS_IN_FEATURES                 (FRF_INPUT_REGISTER_BASE + 0x08UL)
#define FRFS_IN_GAMEPAD0                 (FRF_INPUT_REGISTER_BASE + 0x0CUL)
#define FRFS_IN_GAMEPAD1                 (FRF_INPUT_REGISTER_BASE + 0x10UL)
#define FRFS_IN_MOUSE_XY                 (FRF_INPUT_REGISTER_BASE + 0x14UL)
#define FRFS_IN_MOUSE_BUTTONS            (FRF_INPUT_REGISTER_BASE + 0x18UL)
#define FRFS_IN_KEY_COUNT                (FRF_INPUT_REGISTER_BASE + 0x1CUL)
#define FRFS_IN_KEY_USAGE_FLAGS          (FRF_INPUT_REGISTER_BASE + 0x20UL)
#define FRFS_IN_KEY_TS_LO                (FRF_INPUT_REGISTER_BASE + 0x24UL)
#define FRFS_IN_KEY_TS_HI                (FRF_INPUT_REGISTER_BASE + 0x28UL)
#define FRFS_IN_MIDI_COUNT               (FRF_INPUT_REGISTER_BASE + 0x2CUL)
#define FRFS_IN_MIDI_WORD                (FRF_INPUT_REGISTER_BASE + 0x30UL)
#define FRFS_IN_MIDI_META                (FRF_INPUT_REGISTER_BASE + 0x34UL)
#define FRFS_IN_MIDI_TS_LO               (FRF_INPUT_REGISTER_BASE + 0x38UL)
#define FRFS_IN_MIDI_TS_HI               (FRF_INPUT_REGISTER_BASE + 0x3CUL)
#define FRFS_IN_KEY_DROPPED              (FRF_INPUT_REGISTER_BASE + 0x40UL)
#define FRFS_IN_MIDI_DROPPED             (FRF_INPUT_REGISTER_BASE + 0x44UL)
#define FRFS_IN_USB_BACKEND_STATE        (FRF_INPUT_REGISTER_BASE + 0x48UL)
#define FRFS_IN_USB_IRQ_LAST             (FRF_INPUT_REGISTER_BASE + 0x4CUL)
#define FRFS_IN_KEY_ACK                  (FRF_INPUT_REGISTER_BASE + 0x50UL)
#define FRFS_IN_MIDI_ACK                 (FRF_INPUT_REGISTER_BASE + 0x54UL)
#define FRFS_IN_CLEAR                    (FRF_INPUT_REGISTER_BASE + 0x58UL)

/* V0.3 physical xHCI read-only probe telemetry. */
#define FRFS_IN_XHCI_BUS_BASE            (FRF_INPUT_REGISTER_BASE + 0x5CUL)
#define FRFS_IN_XHCI_PHYS_BASE           (FRF_INPUT_REGISTER_BASE + 0x60UL)
#define FRFS_IN_XHCI_REG_BYTES           (FRF_INPUT_REGISTER_BASE + 0x64UL)
#define FRFS_IN_XHCI_IRQ_SPI             (FRF_INPUT_REGISTER_BASE + 0x68UL)
#define FRFS_IN_XHCI_GIC_ID              (FRF_INPUT_REGISTER_BASE + 0x6CUL)
#define FRFS_IN_XHCI_CAPBASE             (FRF_INPUT_REGISTER_BASE + 0x70UL)
#define FRFS_IN_XHCI_HCSPARAMS1          (FRF_INPUT_REGISTER_BASE + 0x74UL)
#define FRFS_IN_XHCI_HCCPARAMS1          (FRF_INPUT_REGISTER_BASE + 0x78UL)
#define FRFS_IN_XHCI_DBOFF               (FRF_INPUT_REGISTER_BASE + 0x7CUL)
#define FRFS_IN_XHCI_RTSOFF              (FRF_INPUT_REGISTER_BASE + 0x80UL)
#define FRFS_IN_XHCI_PORTSC1             (FRF_INPUT_REGISTER_BASE + 0x84UL)
#define FRFS_IN_XHCI_PROBE_STATUS        (FRF_INPUT_REGISTER_BASE + 0x88UL)
#define FRFS_IN_XHCI_PROBES              (FRF_INPUT_REGISTER_BASE + 0x8CUL)
#define FRFS_IN_XHCI_MAX_PORTS           (FRF_INPUT_REGISTER_BASE + 0x90UL)
#define FRFS_IN_XHCI_MAX_SLOTS           (FRF_INPUT_REGISTER_BASE + 0x94UL)
#define FRFS_IN_XHCI_MAX_INTRS           (FRF_INPUT_REGISTER_BASE + 0x98UL)
#define FRFS_IN_XHCI_HCI_VERSION         (FRF_INPUT_REGISTER_BASE + 0x9CUL)
/* V0.3.2 selected MMIO access method: 3=platform device mapping + 32-bit MMIO. */
#define FRFS_IN_XHCI_ACCESS_MODE         (FRF_INPUT_REGISTER_BASE + 0xA0UL)
/* V0.3.2 low virtual address created by platform_init()/map_peripheral_ranges(). */
#define FRFS_IN_XHCI_VIRT_BASE           (FRF_INPUT_REGISTER_BASE + 0xA4UL)
#define FRF_INPUT_REGISTER_BYTES         0xA8UL
#define FRFS_IN_END                      (FRF_INPUT_REGISTER_BASE + FRF_INPUT_REGISTER_BYTES)

/* V0.1 features. */
#define FRFS_FEAT_CPU1_SERVICE           (1UL << 0)
#define FRFS_FEAT_FIXED_MMIO             (1UL << 1)
#define FRFS_FEAT_ASYNC_MAILBOX          (1UL << 2)
#define FRFS_FEAT_TIMESTAMP              (1UL << 3)
#define FRFS_FEAT_VIRTUAL_HW_FRAMEWORK   (1UL << 4)

/* Reserved platform features. Advertise only when physically implemented. */
#define FRFS_FEAT_USB_HOST               (1UL << 8)
#define FRFS_FEAT_USB_MIDI               (1UL << 9)
#define FRFS_FEAT_BLOCK_STORAGE          (1UL << 10)
#define FRFS_FEAT_RESIDENT_LAUNCHER      (1UL << 11)
#define FRFS_FEAT_NATIVE_DISPLAY         (1UL << 12)

/* V0.2 input-routing features. These do NOT claim a live xHCI backend. */
#define FRFS_FEAT_INPUT_ROUTER            (1UL << 13)
#define FRFS_FEAT_AMIGA_GAMEPORT_BRIDGE   (1UL << 14)
#define FRFS_FEAT_KEY_EVENT_RING          (1UL << 15)
#define FRFS_FEAT_MIDI_EVENT_RING         (1UL << 16)
#define FRFS_FEAT_CPU1_USB_IRQ_ROUTE      (1UL << 17)
#define FRFS_FEAT_USB_BACKEND_HOOK        (1UL << 18)
/* V0.3 proves CPU1 can safely read the enabled BCM2711 internal xHCI MMIO.
 * This is NOT a full host stack and therefore does not imply USB_HOST. */
#define FRFS_FEAT_XHCI_MMIO_PROBE         (1UL << 19)

#define FRFS_BASE_FEATURES (FRFS_FEAT_CPU1_SERVICE | \
                            FRFS_FEAT_FIXED_MMIO | \
                            FRFS_FEAT_ASYNC_MAILBOX | \
                            FRFS_FEAT_TIMESTAMP | \
                            FRFS_FEAT_VIRTUAL_HW_FRAMEWORK | \
                            FRFS_FEAT_INPUT_ROUTER | \
                            FRFS_FEAT_AMIGA_GAMEPORT_BRIDGE | \
                            FRFS_FEAT_KEY_EVENT_RING | \
                            FRFS_FEAT_MIDI_EVENT_RING | \
                            FRFS_FEAT_CPU1_USB_IRQ_ROUTE | \
                            FRFS_FEAT_USB_BACKEND_HOOK | \
                            FRFS_FEAT_XHCI_MMIO_PROBE)

#define FRFS_INPUT_FEATURES (FRFS_FEAT_INPUT_ROUTER | \
                             FRFS_FEAT_AMIGA_GAMEPORT_BRIDGE | \
                             FRFS_FEAT_KEY_EVENT_RING | \
                             FRFS_FEAT_MIDI_EVENT_RING | \
                             FRFS_FEAT_CPU1_USB_IRQ_ROUTE | \
                             FRFS_FEAT_USB_BACKEND_HOOK | \
                             FRFS_FEAT_XHCI_MMIO_PROBE)

/* Gamepad d-pad bits passed by an ARM USB/HID backend. */
#define FRFS_PAD_UP                     (1UL << 0)
#define FRFS_PAD_DOWN                   (1UL << 1)
#define FRFS_PAD_LEFT                   (1UL << 2)
#define FRFS_PAD_RIGHT                  (1UL << 3)

/* Generic pad buttons. Button 1 maps to the Amiga fire line. */
#define FRFS_PAD_BUTTON1                (1UL << 0)
#define FRFS_PAD_BUTTON2                (1UL << 1)
#define FRFS_PAD_BUTTON3                (1UL << 2)
#define FRFS_PAD_BUTTON4                (1UL << 3)
#define FRFS_PAD_START                  (1UL << 8)
#define FRFS_PAD_SELECT                 (1UL << 9)

#define FRFS_MOUSE_LEFT                 (1UL << 0)
#define FRFS_MOUSE_RIGHT                (1UL << 1)
#define FRFS_MOUSE_MIDDLE               (1UL << 2)

#define FRFS_KEY_PRESSED                (1UL << 8)
#define FRFS_KEY_MOD_SHIFT              (1UL << 16)
#define FRFS_KEY_MOD_CTRL               (1UL << 17)
#define FRFS_KEY_MOD_ALT                (1UL << 18)
#define FRFS_KEY_MOD_GUI                (1UL << 19)

#define FRFS_USB_BACKEND_OFFLINE        0UL
#define FRFS_USB_BACKEND_READY          1UL /* reserved for a complete host backend */
#define FRFS_USB_BACKEND_ERROR          2UL
#define FRFS_USB_BACKEND_PROBED         3UL /* xHCI MMIO/capability access proven */
#define FRFS_USB_BACKEND_NEEDS_CONFIG   4UL /* e.g. CM4 /scb/xhci not enabled */

#define FRFS_KEY_RING_ENTRIES           64UL
#define FRFS_MIDI_RING_ENTRIES          128UL

enum {
    FRFS_STATE_IDLE = 0,
    FRFS_STATE_READY = 1,
    FRFS_STATE_RUNNING = 2,
    FRFS_STATE_DONE = 3,
    FRFS_STATE_ERROR = 4
};

enum {
    FRFS_STATUS_OK = 0,
    FRFS_STATUS_NOT_READY = 1,
    FRFS_STATUS_BUSY = 2,
    FRFS_STATUS_BAD_COMMAND = 3,
    FRFS_STATUS_BAD_ACCESS = 4,
    FRFS_STATUS_BAD_ARGUMENT = 5
};

enum {
    FRFS_CMD_NOP = 0,
    FRFS_CMD_PING = 1,
    FRFS_CMD_COUNTER = 2,
    FRFS_CMD_INPUT_SELFTEST = 3,
    FRFS_CMD_INPUT_CLEAR = 4,
    FRFS_CMD_INPUT_SNAPSHOT = 5,
    FRFS_CMD_USB_XHCI_PROBE = 6
};

struct FRFServiceControl {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t features;
    uint32_t worker_core;
    uint32_t worker_online;
    uint32_t counter_hz;
    uint32_t submitted;
    uint32_t completed;
    uint32_t errors;
    uint32_t last_sequence;
    uint32_t last_status;
    uint32_t cpu1_irqs;
    uint32_t cpu1_irq_unhandled;
};

struct FRFServiceMailbox {
    uint32_t command;
    uint32_t sequence;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t result0;
    uint32_t result1;
    uint32_t state;
    uint32_t status;
};

struct FRFKeyEvent {
    uint32_t usage_flags;
    uint32_t timestamp_lo;
    uint32_t timestamp_hi;
};

struct FRFMidiEvent {
    uint32_t word;
    uint32_t meta;
    uint32_t timestamp_lo;
    uint32_t timestamp_hi;
};

#endif
