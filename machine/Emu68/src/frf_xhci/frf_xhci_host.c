/*
 * FRF CPU1 xHCI Host V0.4
 * FRF_CPU1_USB_HUB_RUNTIME_FIX_V0_5_11
 * FRF_CPU1_USB_HUB_SELFCONTAINED_V0_5_5
 * Controlled-write, polling host for the BCM2711 internal xHCI.
 *
 * Scope of this donor:
 *   - ownership only after an explicit God Mode request
 *   - halt/reset/run the already-proven /scb/xhci controller
 *   - preallocated command/event/context/transfer rings
 *   - enumerate ONE device directly attached to root port 1 (no hubs yet)
 *   - read USB device/config/string descriptors
 *   - configure Bulk-Only mass-storage endpoints when present
 *   - expose synchronous control/bulk transport to frf_usb_boot.c
 *
 * Deliberately NOT enabled here:
 *   - hub topology
 *   - interrupt-driven runtime HID/MIDI
 *   - writable USB mass storage
 *   - FRFS_FEAT_USB_HOST/USB_MIDI advertisement
 *
 * All xHCI controller writes execute on ARM CPU1.  God Mode (CPU0) only posts
 * a request and waits for CPU1 to complete it.  Any unexpected geometry or
 * timeout fails closed and normal Amiga boot remains available after a cold
 * restart.
 */

#include <stdint.h>
#include "frf_sdlog.h"
#include "frf_timebase.h"
#include <stddef.h>

#include "support.h"
#include "mmu.h"
#include "frf_services.h"
#include "frf_xhci_probe.h"
#include "frf_xhci_host.h"
#include "frf_native_usb_input.h"
#include "frf_usb_boot.h"

#if !defined(PISTORM_CLASSIC)
#error "FRF CPU1 xHCI Host V0.4 is PiStorm Classic only"
#endif

#define XHCI_CAP_HCSPARAMS2          0x08U

#define XHCI_OP_USBCMD               0x00U
#define XHCI_OP_USBSTS               0x04U
#define XHCI_OP_PAGESIZE             0x08U
#define XHCI_OP_CRCR                 0x18U
#define XHCI_OP_DCBAAP               0x30U
#define XHCI_OP_CONFIG               0x38U
#define XHCI_OP_PORTSC1              0x400U

#define XHCI_CMD_RUN                 (1U << 0)
#define XHCI_CMD_HCRST               (1U << 1)
#define XHCI_STS_HCH                 (1U << 0)
#define XHCI_STS_CNR                 (1U << 11)

#define XHCI_PORT_CCS                (1U << 0)
#define XHCI_PORT_PED                (1U << 1)
#define XHCI_PORT_PR                 (1U << 4)
#define XHCI_PORT_PLS_MASK           (0xFU << 5)
#define XHCI_PORT_PP                 (1U << 9)
#define XHCI_PORT_SPEED_SHIFT        10U
#define XHCI_PORT_SPEED_MASK         (0xFU << XHCI_PORT_SPEED_SHIFT)
#define XHCI_PORT_PIC_MASK           (3U << 14)
#define XHCI_PORT_LWS                (1U << 16)
#define XHCI_PORT_CHANGE_MASK        (0x7FU << 17)
#define XHCI_PORT_WAKE_MASK          (7U << 25)
#define XHCI_PORT_WPR                (1U << 31)

#define XHCI_RT_IR0                  0x20U
#define XHCI_IR_IMAN                 0x00U
#define XHCI_IR_IMOD                 0x04U
#define XHCI_IR_ERSTSZ               0x08U
#define XHCI_IR_ERSTBA               0x10U
#define XHCI_IR_ERDP                 0x18U
#define XHCI_ERDP_EHB                (1U << 3)

#define XHCI_TRB_CYCLE               (1U << 0)
#define XHCI_TRB_TC                  (1U << 1)
#define XHCI_TRB_ISP                 (1U << 2)
#define XHCI_TRB_CHAIN               (1U << 4)
#define XHCI_TRB_IOC                 (1U << 5)
#define XHCI_TRB_IDT                 (1U << 6)
#define XHCI_TRB_DIR_IN              (1U << 16)
#define XHCI_TRB_TYPE_SHIFT          10U
#define XHCI_TRB_TYPE(x)             ((uint32_t)(x) << XHCI_TRB_TYPE_SHIFT)

#define XHCI_TRB_NORMAL              1U
#define XHCI_TRB_SETUP               2U
#define XHCI_TRB_DATA                3U
#define XHCI_TRB_STATUS              4U
#define XHCI_TRB_LINK                6U
#define XHCI_CMD_ENABLE_SLOT         9U
#define XHCI_CMD_DISABLE_SLOT        10U
#define XHCI_CMD_ADDRESS_DEVICE      11U
#define XHCI_CMD_CONFIG_EP           12U
#define XHCI_CMD_EVALUATE_CONTEXT    13U
#define XHCI_CMD_RESET_ENDPOINT      14U
#define XHCI_CMD_STOP_ENDPOINT       15U
#define XHCI_CMD_SET_TR_DEQUEUE      16U
#define XHCI_EVT_TRANSFER            32U
#define XHCI_EVT_COMMAND             33U
#define XHCI_EVT_PORT_STATUS         34U

#define XHCI_CC_SUCCESS              1U
#define XHCI_CC_SHORT_PACKET         13U

#define XHCI_RING_TRBS               256U
#define XHCI_EVENT_TRBS              256U
#define XHCI_MAX_SLOTS_USED          8U
/* R97: contexts must not alias a live external hub or another HID.
 * Root/internal hub + up to two external hubs + up to 4 HIDs = 7 slots. */
#define R97_CHILD_CONTEXT_BANKS       8U
#define R97_MAX_EXTERNAL_HUBS         2U
/* R42: R40 reports FRFXHCI_HE_SCRATCHPAD after valid VL805 PCIe caps.
 * The previous hard limit was 8 buffers; Pi4 xHCI can request more.
 * Cap at 32 and fail closed for impossible/unsupported higher counts. */
#define XHCI_MAX_SCRATCHPADS         32U
#define XHCI_CTRL_BUFFER_BYTES       2048U

#define USB_DT_DEVICE                1U
#define USB_DT_CONFIG                2U
#define USB_DT_STRING                3U
#define USB_DT_INTERFACE             4U
#define USB_DT_ENDPOINT              5U
#define USB_DT_HUB                   0x29U

#define USB_REQ_GET_STATUS           0U
#define USB_REQ_CLEAR_FEATURE        1U
#define USB_REQ_GET_DESCRIPTOR       6U
#define USB_REQ_SET_FEATURE          3U
#define USB_REQ_SET_CONFIGURATION    9U

#define USB_CLASS_HUB                9U
#define USB_HUB_PORT_CONNECTION      (1U << 0)
#define USB_HUB_PORT_ENABLE          (1U << 1)
#define USB_HUB_PORT_RESET           (1U << 4)
#define USB_HUB_PORT_LOW_SPEED       (1U << 9)
#define USB_HUB_PORT_HIGH_SPEED      (1U << 10)
#define USB_HUB_FEAT_PORT_RESET      4U
#define USB_HUB_FEAT_PORT_POWER      8U
#define USB_HUB_FEAT_C_PORT_CONNECTION 16U
#define USB_HUB_FEAT_C_PORT_ENABLE   17U
#define USB_HUB_FEAT_C_PORT_RESET    20U

#define USB_CLASS_HID                3U
#define USB_CLASS_MASS_STORAGE       8U
#define USB_HID_SUBCLASS_BOOT        1U
#define USB_HID_PROTOCOL_KEYBOARD    1U
#define USB_HID_PROTOCOL_MOUSE       2U
#define USB_REQ_SET_PROTOCOL         0x0BU
#define USB_MSC_SUBCLASS_SCSI        6U
#define USB_MSC_PROTOCOL_BOT         0x50U

#define HOST_REQ_IDLE                0U
#define HOST_REQ_READY               1U
#define HOST_REQ_RUNNING             2U
#define HOST_REQ_DONE                3U
#define HOST_REQ_ERROR               4U
#define HOST_REQ_SCAN                1U
#define HOST_REQ_LOAD_FILE           2U

/* Freestanding zero helper.  Keep this local and volatile so the compiler
 * cannot lower it to fortified libc __memset_chk in the bare-metal image. */
static void frf_zero_bytes(void *dst, size_t bytes)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    while (bytes--)
        *d++ = 0U;
}

struct XHCITrb {
    uint32_t parameter_lo;
    uint32_t parameter_hi;
    uint32_t status;
    uint32_t control;
};

struct XHCIERSTEntry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t size;
    uint32_t reserved;
};

struct XHCIRing {
    struct XHCITrb *trbs;
    uint32_t count;
    uint32_t enqueue;
    uint32_t cycle;
};

struct XHCIEvent {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct HostRequest {
    volatile uint32_t state;
    volatile uint32_t command;
    volatile uint32_t file_index;
    volatile uintptr_t dst;
    volatile uint32_t capacity;
    volatile uint32_t result;
    volatile uint32_t sequence;
};

static volatile struct FRFXHCIHostInfo g_host;
volatile uint32_t frf_xhci_dma_cmd;
volatile uint32_t frf_xhci_dma_event;
volatile uint32_t frf_xhci_dma_dcbaa;
volatile uint32_t frf_xhci_dma_erst;
/* CPU1-only writes, sampled read-only by the Pi4 HUD.
 * These are measurements, not synthetic capability values. */
volatile uint32_t frf_usb_hcsparams2;
volatile uint32_t frf_usb_scratchpads_needed;
volatile uint32_t frf_usb_scratchpads_ready;
volatile uint32_t frf_usb_host_phase;
volatile uint32_t frf_usb_root_port_max,frf_usb_root_port_selected,frf_usb_root_port_status;
volatile uint32_t frf_usb_port_status_first8[8];
volatile uint32_t frf_usb2_hub_mouse_port;
volatile uint32_t frf_usb2_hub_mouse_ep;
volatile uint32_t frf_usb2_hub_mouse_slot;
volatile uint32_t frf_usb2_hub_mouse_status;
/* R45: raw read-only register evidence around the xHCI RUN transition. */
volatile uint32_t frf_usb_run_cmd_before, frf_usb_run_cmd_written, frf_usb_run_cmd_after;
volatile uint32_t frf_usb_run_sts_before, frf_usb_run_sts_after, frf_usb_run_sts_timeout;
/* R54: four raw USBSTS checkpoints and physical controller retry count.
 * CRCR pointer bits are READ-AS-ZERO by xHCI specification: not an error! */
volatile uint32_t frf_usb_r54_sts_after_reset;
volatile uint32_t frf_usb_r54_sts_before_clear;
volatile uint32_t frf_usb_r54_sts_after_clear;
volatile uint32_t frf_usb_r54_sts_after_run;
volatile uint32_t frf_usb_r54_attempts;
/* R58: CPU1 MMIO/event-ring snapshots, displayed by CPU3 HDMI. */
volatile uint32_t frf_r58_usbcmd,frf_r58_usbsts,frf_r58_portsc1;
volatile uint32_t frf_r58_evt_ctrl[4],frf_r58_evt_status[4];
volatile uint32_t frf_r58_evt_type,frf_r58_evt_cc,frf_r58_evt_slot,frf_r58_evt_count;
static void frf_r58_note_event(uint32_t status,uint32_t control);

/* R55: snapshots following each hardware programming stage, not one sample
 * near RUN.  0 reset, 1 clear, 2 CONFIG, 3 DCBAA, 4 CRCR, 5 IMAN/IMOD,
 * 6 ERSTSZ, 7 ERSTBA, 8 ERDP, 9 before RUN, 10 after RUN, 11 timeout. */
volatile uint32_t frf_r55_sts[12];
volatile uint32_t frf_r55_first_hse;
volatile uint32_t frf_r55_dma_bad;
volatile uint64_t frf_r55_phys_cmd,frf_r55_phys_evt,frf_r55_phys_dcbaa,frf_r55_phys_erst;
volatile uint64_t frf_r55_bus_cmd,frf_r55_bus_evt,frf_r55_bus_dcbaa,frf_r55_bus_erst;
extern volatile uint64_t frf_r55_dma_cpu_base,frf_r55_dma_bus_base,frf_r55_dma_window_size;



volatile uint32_t frf_usb_run_crcr_lo, frf_usb_run_crcr_hi;
volatile uint32_t frf_usb_run_dcbaa_lo, frf_usb_run_dcbaa_hi;
volatile uint32_t frf_usb_run_config, frf_usb_run_pagesize;
volatile uint32_t frf_usb_run_hcs1, frf_usb_run_hcc1;
volatile uint32_t frf_usb_cmd_crcr_after_write,frf_usb_cmd_crcr_at_timeout;
volatile uint32_t frf_usb_cmd_erst_after_write,frf_usb_cmd_erdp_after_write;
volatile uint32_t frf_usb_cmd_event_0_ctrl,frf_usb_cmd_event_0_status;
volatile uint32_t frf_usb_cmd_last_type,frf_usb_cmd_last_trb_dma;





static volatile struct HostRequest g_request;

static uintptr_t g_mmio;
static uintptr_t g_op;
static uintptr_t g_rt;
static uintptr_t g_db;
static uint32_t g_reg_bytes;
static uint32_t g_context_size;
static uint32_t g_cap_length;
static uint32_t g_port_speed;
static uint32_t g_slot_id;
static uint32_t g_ep0_mps;
static uint32_t g_bulk_in_dci;
static uint32_t g_bulk_out_dci;
static uint32_t g_hid_in_dci, g_hid_iface, g_hid_mps, g_hid_ready;
/* R80: one live xHCI slot and interrupt IN ring per device, owned by CPU1.
 * Each endpoint has independent DMA, TRB producer state and report storage.
 * Do not touch the CPU0 Emu68 guest execution path. */
#define FRF_NATIVE_HID_MAX 4U
struct NativeHID {
    uint32_t valid,slot,protocol,interface,ep,dci,mps,failures;
    uint32_t pending_length;
    uint32_t vid,pid,report_desc_length;
    uint32_t out_ep,out_dci,out_mps;
    struct XHCIRing out_ring;
    uint8_t out_buffer[64] __attribute__((aligned(64)));
    /* R92: after link loss, keep the slot. The state machine schedules a
       bounded per-device recovery, never a second global xHCI HCRST. */
    uint32_t recover_pending,recover_cc,recover_count,recover_stop;
    /* R99: two disjoint DMA transfer-ring regions per HID. Delayed
       completions from a stopped ring can never match the first new TD
       after reset; h->bank indexes xHCI DMA contexts, not HID devices. */
    uint32_t ring_bank,ring_epoch;
    uint64_t retry_after_cnt,pending_since_cnt;
    uint64_t pending;
    struct XHCIRing ring,ep0;
    uint8_t report[64] __attribute__((aligned(64)));
};
static struct NativeHID g_native_hid[FRF_NATIVE_HID_MAX] __attribute__((aligned(64)));
static uint32_t g_native_count, g_native_bank;
static uint32_t g_r97_next_bank;
volatile uint32_t frf_r97_ext_hub_slot,frf_r97_ext_hub_ports;
volatile uint32_t frf_r97_ext_connected_mask,frf_r97_ext_live_mask;
volatile uint32_t frf_r97_ext_error,frf_r97_ext_stage;
volatile uint32_t frf_r97_ext_child_count,frf_r97_ext_tt_slot;
volatile uint32_t frf_r97_ext_last_cc,frf_r97_ext_last_port;
volatile uint32_t frf_r97_ext_hub_vidpid,frf_r97_hub_count;
volatile uint32_t frf_r97_ext_level,frf_r97_ext_unsupported;
/* R98 root-port hub Address Device recovery: actual xHC completion codes. */
volatile uint32_t frf_r98_root_addr_cc[4],frf_r98_root_enable_cc[4];
volatile uint32_t frf_r98_root_speed[4],frf_r98_root_slot[4];
volatile uint32_t frf_r98_root_attempts[4],frf_r98_root_stage[4];
volatile uint32_t frf_r98_root_last_status[4],frf_r98_root_power_cycles;
volatile uint32_t frf_r98_root_hs_recovery_ms;
/* Live stability trace. The earlier POST only showed boot-time counts. */
volatile uint32_t frf_r99_root_status,frf_r99_root_change;
volatile uint32_t frf_r99_root_poll_ok,frf_r99_root_poll_fail;
volatile uint32_t frf_r99_root_link_lost,frf_r99_root_link_up;
volatile uint32_t frf_r99_root_monitor_port,frf_r99_live_link;
volatile uint32_t frf_r99_last_hid_error_cc,frf_r99_hid_error_count;
volatile uint32_t frf_r99_ring_swaps,frf_r99_stale_events;
volatile uint32_t frf_r99_hid_recovered_total,frf_r99_hid_last_age_seconds;
volatile uint32_t frf_r99_hid_active_pending;
static struct XHCIRing g_r99_root_ep0_ring;
static uint32_t g_r99_root_ep0_mps,g_r99_root_valid;
static uint64_t g_r99_root_next_check;
#ifndef FRF_RELEASE_QUIET
static void frf_r99_monitor_root_port(void);
#endif


/* R89: sticky per physical hub port live/failed stage records, independent
 * of host-global g_slot_id/EP0 ring which change during enumeration. */
volatile uint32_t frf_r89_native_seen_mask,frf_r89_native_live_mask;
volatile uint32_t frf_r89_native_fail_mask,frf_r89_native_candidate_mask;
volatile uint32_t frf_r89_native_protocol[4],frf_r89_native_slot[4];
volatile uint32_t frf_r89_native_count;
volatile uint32_t frf_r89_native_singlepass=1u;
/* R94: expose all hub-child identities, not only successfully configured
 * generic HID gamepads. Vendor-defined wireless receivers must appear
 * in the live POST even when the HID parser cannot handle their protocol. */
volatile uint32_t frf_r94_port_vidpid[4],frf_r94_port_classproto[4];
volatile uint32_t frf_r94_last_unhandled_vidpid,frf_r94_last_unhandled_classproto;
volatile uint32_t frf_r94_last_seen_vidpid,frf_r94_hotplug_connects;
volatile uint32_t frf_r94_hotplug_disconnects,frf_r94_hotplug_failures;
volatile uint32_t frf_r94_usb_connected_mask,frf_r94_usb_live_mask;
volatile uint32_t frf_r94_usb_pending_mask,frf_r94_hotplug_stage;
volatile uint32_t frf_r94_hub_poll_errors;
/* R95 sticky diagnostics from the most recent per-port reset/address.
 * A gamepad receiver that fails Address Device never has a VID:PID.
 * These codes are the xHC command completion codes, not HID input bytes. */
volatile uint32_t frf_r95_addr_attempts[4],frf_r95_addr_cc[4];
volatile uint32_t frf_r95_enable_cc[4],frf_r95_reset_ok[4];
volatile uint32_t frf_r95_port_speed[4],frf_r95_last_slot[4];
volatile uint32_t frf_r95_addr_rescued_mask,frf_r95_addr_failed_mask;

/* R90: hub port USB reset must happen directly before Address Device.
   Per-port statuses are sticky even when a later Enable/Disable Slot
   command overwrites the global frf_r85_cmd_last_cc. */
volatile uint32_t frf_r90_port_reset_result[4];
volatile uint32_t frf_r90_port_address_cc[4];
volatile uint32_t frf_r90_port_enable_cc[4];
volatile uint32_t frf_r90_port_speed[4];
volatile uint32_t frf_r90_sequence_step[4];
/* R83: publish transport milestones, independent of AmigaOS */
/* R84: sticky CPU1-only hub control trace, visible on CPU3 without
   re-issuing destructive controller initialization. */
volatile uint32_t frf_r84_hub_power_ok, frf_r84_hub_power_fail;
volatile uint32_t frf_r84_hub_status_ok, frf_r84_hub_status_fail;
volatile uint32_t frf_r84_hub_last_status[4], frf_r84_hub_last_change[4];
volatile uint32_t frf_r84_hub_cc,frf_r84_hub_evt_kind,frf_r84_hub_td_reason;
volatile uint32_t frf_r84_snapshot[4];
volatile uint32_t frf_r84_hub_scan_step;
/* R85: raw child slot/address completion codes and controller-assigned IDs.
   0=not attempted, 0xFE=timeout, otherwise xHCI completion code. */
volatile uint32_t frf_r85_enable_cc[4],frf_r85_address_cc[4];
volatile uint32_t frf_r85_speed[4],frf_r85_slot[4];
volatile uint32_t frf_r85_cmd_last_type,frf_r85_cmd_last_cc;
volatile uint32_t frf_r85_child_last_error,frf_r85_address_ok;
/* R86: the specific EP0 descriptor request and transfer that fails.
 * Capture before another port's synchronous requests overwrite counters. */
volatile uint32_t frf_r86_desc_phase[4],frf_r86_desc_fail_cc[4];
volatile uint32_t frf_r86_desc_fail_reason[4],frf_r86_desc_got[4];
volatile uint32_t frf_r86_desc_fail_slot[4],frf_r86_desc_fail_ep[4];
volatile uint32_t frf_r86_last_evt_slot,frf_r86_last_evt_ep;
volatile uint32_t frf_r86_last_ctl_request,frf_r86_last_ctl_length;
volatile uint32_t frf_r86_address_recovery_ms;
static uint32_t frf_r86_track_port;
volatile uint32_t frf_r83_hid_count;
volatile uint32_t frf_r83_downstream_status[8];
volatile uint32_t frf_r83_downstream_step[8];
volatile uint32_t frf_r83_last_slot,frf_r83_last_ep,frf_r83_last_cc;

/* R82: per-device USB bus-level diagnostics; ARM CPU1 is the sole writer. */
volatile uint32_t frf_r82_interval[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_speed[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_arm[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_completed[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_failed[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_last_cc[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_last_bytes[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_last_word[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r82_unknown_events;
/* R88: live xHCI interrupt report cadence and actual TD buffer length.
   CPU1 updates after each completed TD; CPU3 can display without locks. */
volatile uint32_t frf_r88_hid_td_size[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r88_hid_gap_max_ms[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r88_hid_gap_last_ms[FRF_NATIVE_HID_MAX];
static uint64_t frf_r88_hid_last_cnt[FRF_NATIVE_HID_MAX];

/* R92 sticky per-device recovery telemetry, safe read-only from CPU3.
   State 0=healthy,1=retry backoff,2=reset/stop attempt,
   3=rearmed, E1=reset/stop failed, E2=set-dequeue failed. */
volatile uint32_t frf_r92_recovery_state[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r92_recovery_attempts[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r92_last_cc[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r92_recovery_ok[FRF_NATIVE_HID_MAX];
volatile uint32_t frf_r92_deferred_events,frf_r92_deferred_overflow;
volatile uint32_t frf_r92_silent_timeout[FRF_NATIVE_HID_MAX];

/* A synchronous command during live HID polling can race completions from
   other devices. Retain their xHCI transfer events rather than silently
   dropping them in event_wait() / event_wait_transfer_trb(). */
#define FRF_R92_DEFERRED_MAX 256U
static struct XHCIEvent frf_r92_deferred[FRF_R92_DEFERRED_MAX];
static uint32_t frf_r92_defer_rd,frf_r92_defer_wr;
static void frf_r92_defer_event(const struct XHCIEvent *e)
{
    uint32_t next=(frf_r92_defer_wr+1u)%FRF_R92_DEFERRED_MAX;
    if(next==frf_r92_defer_rd) {
        ++frf_r92_deferred_overflow;
        return;
    }
    frf_r92_deferred[frf_r92_defer_wr]=*e;
    frf_r92_defer_wr=next;
    ++frf_r92_deferred_events;
}
static int frf_r92_pop_event(struct XHCIEvent *e)
{
    if(frf_r92_defer_rd==frf_r92_defer_wr)return 0;
    *e=frf_r92_deferred[frf_r92_defer_rd];
    frf_r92_defer_rd=(frf_r92_defer_rd+1u)%FRF_R92_DEFERRED_MAX;
    return 1;
}

static uint32_t g_event_dequeue;
static uint32_t g_event_cycle;

static struct XHCIRing g_cmd_ring;
static struct XHCIRing g_ep0_ring;
static struct XHCIRing g_bulk_in_ring;
static struct XHCIRing g_bulk_out_ring;
static struct XHCIRing g_hid_in_ring;
/* R96: Nintendo Pro Controller requires interrupt OUT for HID USB commands. */
static struct XHCIRing g_hid_out_ring;
static uint32_t g_hid_out_dci,g_r96_hid_out_ep,g_r96_hid_out_mps,g_r96_hid_out_interval;
volatile uint32_t frf_r96_switch_out_config,frf_r96_switch_tx_ok;
volatile uint32_t frf_r96_switch_tx_fail,frf_r96_switch_last_rx;
volatile uint32_t frf_r96_switch_last_tx,frf_r96_switch_last_cc;
volatile uint32_t frf_r96_switch_report30,frf_r96_switch_report3f;


static struct XHCITrb g_cmd_trbs[XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
static struct XHCITrb g_event_trbs[XHCI_EVENT_TRBS * 2U] __attribute__((aligned(8192)));
static struct XHCITrb g_ep0_trbs[XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
static struct XHCITrb g_bulk_in_trbs[XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
static struct XHCITrb g_bulk_out_trbs[XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
/* R99: ring_init clears 2*TRBS (the second half is VL805 DMA padding).
 * A recovery MUST alternate between disjoint 2*TRBS regions, not an
 * overlapping +TRBS offset, or ring_init would overrun its allocation. */
static struct XHCITrb g_hid_in_trbs[R97_CHILD_CONTEXT_BANKS][XHCI_RING_TRBS * 4U] __attribute__((aligned(8192)));
static struct XHCITrb g_hid_out_trbs[R97_CHILD_CONTEXT_BANKS][XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
static struct XHCIERSTEntry g_erst __attribute__((aligned(64)));
static uint64_t g_dcbaa[65] __attribute__((aligned(64)));
static uint64_t g_scratchpad_ptrs[XHCI_MAX_SCRATCHPADS] __attribute__((aligned(64)));
static uint8_t g_scratchpads[XHCI_MAX_SCRATCHPADS][4096] __attribute__((aligned(4096)));
static uint8_t g_device_context[32U * 64U] __attribute__((aligned(64)));
static uint8_t g_input_context[33U * 64U] __attribute__((aligned(64)));
static uint8_t g_ctrl_buffer[XHCI_CTRL_BUFFER_BYTES] __attribute__((aligned(64)));

/* FRF_CPU1_USB_HUB_SELFCONTAINED_V0_5_5
 * One-tier USB2 hub topology state.  Root and child device contexts must not
 * alias: xHCI keeps the root-hub slot live while a downstream child is active.
 */
static uint32_t g_route_string;
static uint32_t g_root_hub_port = 1U;
static uint32_t g_selected_root_port=1u;
static uint32_t g_slot_hub;
static uint32_t g_slot_num_ports;
static uint32_t g_tt_hub_slot;
static uint32_t g_tt_port;
static uint32_t g_tt_think_time;
static uint32_t g_root_hub_slot;
static uint32_t g_root_hub_speed;
static uint32_t g_hub_num_ports;
static uint32_t g_hub_tt_think_time;
static struct XHCIRing g_child_ep0_ring;
static struct XHCITrb g_child_ep0_trbs[R97_CHILD_CONTEXT_BANKS][XHCI_RING_TRBS * 2U] __attribute__((aligned(8192)));
static uint8_t g_child_device_context[R97_CHILD_CONTEXT_BANKS][32U * 64U] __attribute__((aligned(64)));
static struct FRFXHCIHubDeviceInfo g_hub_devices[FRFXHCI_HUB_MAX_DEVICES];
static uint32_t g_hub_device_count;

/* Hub topology is persistent, including the original Pi4 VL805 internal
 * root hub and two downstream Huasheng 214b:7260 hub controllers.
 * route holds 4-bit port nibbles from the root, depth=0 for internal. */
struct R97HubState {
    uint32_t slot,speed,route,root_port,depth;
    uint32_t ports,tt_think,power_ms,vidpid,bank;
    uint32_t live;
    struct XHCIRing ep0;
    uint32_t ep0_mps;
};
static struct R97HubState g_r97_external[R97_MAX_EXTERNAL_HUBS];
static uint32_t g_r97_hub_used;


static uint64_t host_counter(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t host_counter_hz(void)
{
    return frf_timebase_hz();
}

static uint32_t host_cpu(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return (uint32_t)(v & 3U);
}

static uint64_t host_deadline_ms(uint32_t ms)
{
    const uint64_t hz = host_counter_hz();
    return host_counter() + (hz * (uint64_t)ms + 999ULL) / 1000ULL;
}

static int host_timed_out(uint64_t deadline)
{
    return (int64_t)(host_counter() - deadline) >= 0;
}

static uint32_t host_cache_line(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4U << ((ctr >> 16) & 0xFU);
}

static void host_cache_clean(const void *ptr, uint32_t bytes)
{
    if (!ptr || !bytes)
        return;
    const uint32_t line = host_cache_line();
    uintptr_t p = (uintptr_t)ptr & ~((uintptr_t)line - 1U);
    const uintptr_t end = ((uintptr_t)ptr + bytes + line - 1U) & ~((uintptr_t)line - 1U);
    for (; p < end; p += line)
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void host_cache_invalidate(void *ptr, uint32_t bytes)
{
    if (!ptr || !bytes)
        return;
    const uint32_t line = host_cache_line();
    uintptr_t p = (uintptr_t)ptr & ~((uintptr_t)line - 1U);
    const uintptr_t end = ((uintptr_t)ptr + bytes + line - 1U) & ~((uintptr_t)line - 1U);
    for (; p < end; p += line)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t host_dma(const void *ptr)
{
    const uint64_t pa=(uint64_t)mmu_virt2phys((uintptr_t)ptr);
    const uint64_t cpu=frf_r55_dma_cpu_base;
    const uint64_t bus=frf_r55_dma_bus_base;
    const uint64_t sz=frf_r55_dma_window_size;
    if (!sz || pa < cpu || pa-cpu >= sz || bus > UINT64_MAX-(pa-cpu)) {
        frf_r55_dma_bad |= 1U;
        return UINT64_MAX; /* must fail before programming xHCI rings */
    }
    return bus+(pa-cpu);
}

static uint32_t host_read32(uintptr_t addr)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    const uint32_t v = LE32(*p);
    dsb();
    return v;
}

static void r55_sample_sts(uint32_t stage,uintptr_t op)
{
    uint32_t v = host_read32(op+XHCI_OP_USBSTS);
    frf_r55_sts[stage] = v;
    if ((v & 4U) && !frf_r55_first_hse)
        frf_r55_first_hse = stage+1u;
}

static void host_write32(uintptr_t addr, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    *p = LE32(v);
    dsb();
}

static void host_write64(uintptr_t addr, uint64_t v)
{
    host_write32(addr + 0U, (uint32_t)v);
    host_write32(addr + 4U, (uint32_t)(v >> 32));
}

static void host_zero(void *ptr, uint32_t bytes)
{
    frf_zero_bytes(ptr, bytes);
    host_cache_clean(ptr, bytes);
}

static void host_clear_info(uint32_t next_generation)
{
    frf_zero_bytes((void *)(uintptr_t)&g_host, sizeof(g_host));
    g_host.generation = next_generation;
    g_host.state = FRFXHCI_HOST_OFFLINE;
    dsb();
}

const volatile struct FRFXHCIHostInfo *frf_xhci_host_info(void)
{
    return &g_host;
}

static int host_wait_mask(uintptr_t addr, uint32_t mask, uint32_t want, uint32_t timeout_ms)
{
    const uint64_t deadline = host_deadline_ms(timeout_ms);
    while (!host_timed_out(deadline)) {
        if ((host_read32(addr) & mask) == want)
            return 1;
        __asm__ volatile("yield");
    }
    return 0;
}

static void ring_init(struct XHCIRing *ring, struct XHCITrb *trbs, uint32_t count)
{
    ring->trbs = trbs;
    ring->count = count;
    ring->enqueue = 0;
    ring->cycle = 1;
    /* VL805 quirk: second 4K is harmless DMA-owned prefetch padding. */
    host_zero(trbs, count * 2U * (uint32_t)sizeof(*trbs));

    const uint64_t base = host_dma(trbs);
    struct XHCITrb *link = &trbs[count - 1U];
    link->parameter_lo = LE32((uint32_t)base);
    link->parameter_hi = LE32((uint32_t)(base >> 32));
    link->status = 0;
    link->control = LE32(XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | XHCI_TRB_CYCLE);
    host_cache_clean(link, sizeof(*link));
}

static struct XHCITrb *ring_next(struct XHCIRing *ring, uint64_t *dma_out)
{
    if (ring->enqueue >= ring->count - 1U) {
        struct XHCITrb *link = &ring->trbs[ring->count - 1U];
        uint32_t ctrl = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC |
                        (ring->cycle ? XHCI_TRB_CYCLE : 0U);
        link->control = LE32(ctrl);
        host_cache_clean(link, sizeof(*link));
        ring->enqueue = 0;
        ring->cycle ^= 1U;
    }

    struct XHCITrb *t = &ring->trbs[ring->enqueue++];
    frf_zero_bytes(t, sizeof(*t));
    if (dma_out)
        *dma_out = host_dma(t);
    return t;
}

static void event_ack(void)
{
    g_event_dequeue++;
    if (g_event_dequeue >= XHCI_EVENT_TRBS) {
        g_event_dequeue = 0;
        g_event_cycle ^= 1U;
    }
    const uint64_t next = host_dma(&g_event_trbs[g_event_dequeue]);
    host_write64(g_rt + XHCI_RT_IR0 + XHCI_IR_ERDP, next | XHCI_ERDP_EHB);
}

static int event_wait(uint32_t wanted_type, uint32_t timeout_ms, struct XHCIEvent *out)
{
    const uint64_t deadline = host_deadline_ms(timeout_ms);
    while (!host_timed_out(deadline)) {
        struct XHCITrb *t = &g_event_trbs[g_event_dequeue];
        host_cache_invalidate(t, sizeof(*t));
        const uint32_t control = LE32(t->control);
        if ((control & XHCI_TRB_CYCLE) == (g_event_cycle ? XHCI_TRB_CYCLE : 0U)) {
            struct XHCIEvent e;
            e.parameter = (uint64_t)LE32(t->parameter_lo) |
                          ((uint64_t)LE32(t->parameter_hi) << 32);
            e.status = LE32(t->status);
            e.control = control;
            frf_r58_note_event(e.status,e.control);
            event_ack();
            const uint32_t type = (control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
            if (type == wanted_type) {
                if (out)
                    *out = e;
                return 1;
            }
            /* R92: recovery commands run with other HID interrupt-IN
             * endpoints active. Do not destroy their completed input when
             * waiting for a Reset Endpoint / Set TR Dequeue command. */
            if (type==XHCI_EVT_TRANSFER)
                frf_r92_defer_event(&e);
            /* Port-change events still handled by topology diagnostics. */
        } else {
            __asm__ volatile("yield");
        }
    }
    return 0;
}

/*
 * Wait for the completion of one synchronous transfer TRB and reject stale
 * transfer events.  A control-IN TD may generate an early SHORT_PACKET event
 * for its Data Stage when ISP is set and then a second SUCCESS event for the
 * IOC Status Stage.  The caller therefore needs a pointer-matched event, not
 * merely the first Transfer Event in the ring.
 */
static int event_wait_transfer_trb(uint64_t wanted_trb_dma, uint64_t short_trb_dma,
                                   uint32_t timeout_ms, uint32_t requested,
                                   uint32_t *actual)
{
    const uint64_t deadline = host_deadline_ms(timeout_ms);
    uint32_t short_actual = requested;
    int have_short_actual = 0;

    while (!host_timed_out(deadline)) {
        struct XHCITrb *t = &g_event_trbs[g_event_dequeue];
        host_cache_invalidate(t, sizeof(*t));
        const uint32_t control = LE32(t->control);
        if ((control & XHCI_TRB_CYCLE) !=
            (g_event_cycle ? XHCI_TRB_CYCLE : 0U)) {
            __asm__ volatile("yield");
            continue;
        }

        struct XHCIEvent e;
        e.parameter = (uint64_t)LE32(t->parameter_lo) |
                      ((uint64_t)LE32(t->parameter_hi) << 32);
        e.status = LE32(t->status);
        e.control = control;
        frf_r58_note_event(e.status,e.control);
        event_ack();

        const uint32_t type = (control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type != XHCI_EVT_TRANSFER)
            continue;

        const uint32_t cc = (e.status >> 24) & 0xFFU;
        const uint32_t residual = e.status & 0x00FFFFFFU;
        frf_r86_last_evt_slot=(control>>24)&0xffu;
        frf_r86_last_evt_ep=(control>>16)&0x1fu;
        frf_r84_hub_cc=cc;frf_r84_hub_evt_kind=(control >> XHCI_TRB_TYPE_SHIFT)&0x3fu;

        /* An ISP Data Stage short event belongs to the same TD but is not the
         * IOC Status Stage we are waiting for.  Preserve its byte count and
         * continue until the pointer-matched status event arrives. */
        if ((e.parameter & ~0xFULL) != (wanted_trb_dma & ~0xFULL)) {
            if (short_trb_dma &&
                (e.parameter & ~0xFULL) == (short_trb_dma & ~0xFULL) &&
                cc == XHCI_CC_SHORT_PACKET && residual <= requested) {
                short_actual = requested - residual;
                have_short_actual = 1;
                continue;
            }

            /* Other HID endpoints may complete while a gamepad's
             * CLEAR_FEATURE request is running. Retain their events. */
            frf_r92_defer_event(&e);
            continue;
        }

        if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
            frf_r84_hub_td_reason=3u;
            return 0;
        }

        if (actual) {
            uint32_t got = requested;
            if (have_short_actual)
                got = short_actual;
            else if (residual <= requested)
                got = requested - residual;
            *actual = got;
        }
        return 1;
    }
    frf_r84_hub_td_reason=4u; /* timeout waiting for transfer TD */
    return 0;
}

static int command_submit(uint32_t type, uint64_t parameter, uint32_t control_extra,
                          uint32_t timeout_ms, struct XHCIEvent *evt)
{
    uint64_t trb_dma = 0;
    struct XHCITrb *t = ring_next(&g_cmd_ring, &trb_dma);
    t->parameter_lo = LE32((uint32_t)parameter);
    t->parameter_hi = LE32((uint32_t)(parameter >> 32));
    t->status = 0;
    t->control = LE32(XHCI_TRB_TYPE(type) | control_extra |
                      (g_cmd_ring.cycle ? XHCI_TRB_CYCLE : 0U));
    host_cache_clean(t, sizeof(*t));
    frf_usb_cmd_last_type=type;
    frf_r85_cmd_last_type=type;
    frf_usb_cmd_last_trb_dma=(uint32_t)trb_dma;
    host_write32(g_db + 0U, 0U);

    struct XHCIEvent e;
    if (!event_wait(XHCI_EVT_COMMAND, timeout_ms, &e)) {
        /* Capture after failed command; do not alter ring state. */
        frf_usb_cmd_crcr_at_timeout=host_read32(g_op+XHCI_OP_CRCR);
        host_cache_invalidate(&g_event_trbs[0],sizeof(g_event_trbs[0]));
        frf_usb_cmd_event_0_ctrl=LE32(g_event_trbs[0].control);
        frf_usb_cmd_event_0_status=LE32(g_event_trbs[0].status);
        frf_r85_cmd_last_cc=0xFEu;
        g_host.error = FRFXHCI_HE_COMMAND_TIMEOUT;
        return 0;
    }
    const uint32_t cc = (e.status >> 24) & 0xFFU;
    frf_r85_cmd_last_cc=cc;
    if (cc != XHCI_CC_SUCCESS) {
        g_host.error = FRFXHCI_HE_COMMAND_FAILED;
        kprintf("[FRFXHCI-HOST] command type=%u completion=%u trb=%08x\n",
                type, cc, (uint32_t)trb_dma);
        return 0;
    }
    if (evt)
        *evt = e;
    return 1;
}

static uint32_t scratchpad_count(uint32_t hcs2)
{
    return ((hcs2 >> 27) & 0x1FU) | ((hcs2 >> 16) & 0x3E0U);
}

void frf_xhci_host_diag_snapshot_cpu1(void)
{
    if (host_cpu()!=1U || !g_op || !g_mmio) return;
    frf_r58_usbcmd=host_read32(g_op+XHCI_OP_USBCMD);
    frf_r58_usbsts=host_read32(g_op+XHCI_OP_USBSTS);
    frf_r58_portsc1=host_read32(g_op+XHCI_OP_PORTSC1);
    if (!g_rt) return; /* no event ring yet */
    for (unsigned i=0;i<4u;i++) {
        host_cache_invalidate(&g_event_trbs[i],sizeof(g_event_trbs[i]));
        frf_r58_evt_ctrl[i]=LE32(g_event_trbs[i].control);
        frf_r58_evt_status[i]=LE32(g_event_trbs[i].status);
    }
}
static void frf_r58_note_event(uint32_t status,uint32_t control)
{
    frf_r58_evt_type=(control>>XHCI_TRB_TYPE_SHIFT)&0x3fu;
    frf_r58_evt_cc=(status>>24)&0xffu;
    frf_r58_evt_slot=(control>>24)&0xffu;
    __atomic_add_fetch(&frf_r58_evt_count,1u,__ATOMIC_RELAXED);
}

static uint32_t port_neutral(uint32_t v)
{
    v &= ~XHCI_PORT_PED;
    v &= ~XHCI_PORT_PR;
    v &= ~XHCI_PORT_PLS_MASK;
    v &= ~XHCI_PORT_PIC_MASK;
    v &= ~XHCI_PORT_LWS;
    v &= ~XHCI_PORT_CHANGE_MASK;
    v &= ~XHCI_PORT_WAKE_MASK;
    v &= ~XHCI_PORT_WPR;
    return v;
}


/* R43: xHCI USB2/USB3 ports are not necessarily PORTSC1. */
static uintptr_t root_port_reg(uint32_t n)
{
    if (!n || n>32u) return 0u;
    uint32_t off=g_cap_length+XHCI_OP_PORTSC1+(n-1u)*16u;
    if (off>g_reg_bytes || g_reg_bytes-off<4u) return 0u;
    return g_mmio+(uintptr_t)off;
}
static int port_reset(void)
{
    const volatile struct FRFXHCIProbeInfo *probe=frf_xhci_probe_info();
    uint32_t count=probe?probe->max_ports:0u;
    if (count>32u) count=32u;
    frf_usb_root_port_max=count;
    frf_usb_root_port_selected=0u;
    frf_usb_root_port_status=0u;
    for (unsigned i=0;i<8u;i++) frf_usb_port_status_first8[i]=0u;
    frf_usb_host_phase=0x09u; /* examining all root ports */
    uint32_t selected=0u, p=0u, selected_rank=0u;
    uint64_t stable_deadline=0u;
    const uint64_t deadline=host_deadline_ms(1500u);
    while (!host_timed_out(deadline)) {
        for (uint32_t n=1u;n<=count;n++) {
            uintptr_t reg=root_port_reg(n);
            if (!reg) continue;
            uint32_t v=host_read32(reg);
            if (n<=8u) frf_usb_port_status_first8[n-1u]=v;
            if (selected==n && !(v&XHCI_PORT_CCS)) {
                selected=0u; selected_rank=0u; stable_deadline=0u;
            }
            if (!(v&XHCI_PORT_CCS)) continue;
            uint32_t speed=(v&XHCI_PORT_SPEED_MASK)>>XHCI_PORT_SPEED_SHIFT;
            /* R81: VL805 has USB3 and USB2 root ports. The four Pi4 USB-A
             * sockets present ordinary boot HID on the USB2 companion hub.
             * Prefer a connected high-speed root (speed ID 3) over the first
             * SuperSpeed (ID >=4) root, which may have no USB2 children.
             * Low/full speed devices remain valid fallback candidates. */
            uint32_t rank=(speed==3u)?3u:
                          ((speed==1u||speed==2u)?2u:
                          (speed>=4u?1u:0u));
            if(!selected || rank>selected_rank) {
                selected=n; selected_rank=rank;
                stable_deadline=host_deadline_ms(rank>=3u?100u:500u);
                p=v;
                kprintf("[R81-USB2] candidate root%u speed%u rank%u PORTSC=%08x\n",
                        n,speed,rank,v);
            } else if(selected==n) {
                p=v;
            }
        }
        if (selected && host_timed_out(stable_deadline)) break;
        __asm__ volatile("yield");
    }
    if (!selected || !(p&XHCI_PORT_CCS) ||
        !host_timed_out(stable_deadline)) {
        frf_usb_host_phase=0x0au; /* no attached root device */
        g_host.port_connected=0u;
        g_host.error=FRFXHCI_HE_NO_DEVICE;
        kprintf("[R43] no attached USB root port among %u\n",count);
        return 0;
    }
    const uintptr_t port=root_port_reg(selected);
    if (!port) {g_host.error=FRFXHCI_HE_PORT_RESET;return 0;}
    const uint32_t attach_initial=p;
    g_selected_root_port=selected;
    g_root_hub_port=selected;
    frf_usb_root_port_selected=selected;
    frf_usb_root_port_status=p;
    g_host.port_connected=1u;
    frf_usb_host_phase=0x12u; /* attached */
    kprintf("[FRFXHCI-HOST] root attach stable initial=%08x attached=%08x\n",
            attach_initial, p);

    uint32_t speed = (p & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
    if (speed >= 4U) {
        host_write32(port, port_neutral(p) | XHCI_PORT_WPR);
        if (!host_wait_mask(port, XHCI_PORT_WPR, 0U, 500U)) {
            g_host.error = FRFXHCI_HE_PORT_RESET;
            return 0;
        }
    } else {
        host_write32(port, port_neutral(p) | XHCI_PORT_PR);
        if (!host_wait_mask(port, XHCI_PORT_PR, 0U, 500U)) {
            g_host.error = FRFXHCI_HE_PORT_RESET;
            return 0;
        }
    }

    frf_usb_host_phase=0x13u;
    kprintf("[FRFXHCI-HOST] root post-reset PORTSC=%08x\n",
            host_read32(port));

    p = host_read32(port);
    /* R87: publish the POST-reset root port snapshot.  R86 HUD sampled
       the pre-reset value and misleadingly showed PED=0 during successful
       downstream hub enumeration. */
    frf_usb_root_port_status=p;
    speed = (p & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
    g_port_speed = speed;
    g_host.speed_id = speed;
    g_host.port_connected = (p & XHCI_PORT_CCS) ? 1U : 0U;
    g_host.port_enabled = (p & XHCI_PORT_PED) ? 1U : 0U;
    if (!(p & XHCI_PORT_CCS) || !(p & XHCI_PORT_PED)) {
        g_host.error = FRFXHCI_HE_PORT_RESET;
        return 0;
    }
    return 1;
}

static uint32_t ep0_initial_mps(uint32_t speed)
{
    if (speed >= 4U)
        return 512U;
    if (speed == 3U)
        return 64U;
    return 8U;
}

static uint32_t *input_ctx(uint32_t index)
{
    return (uint32_t *)(void *)(g_input_context + (uintptr_t)index * g_context_size);
}

static void context_write32(uint32_t *ctx, uint32_t index, uint32_t value)
{
    ctx[index] = LE32(value);
}

static void fill_slot_context(uint32_t context_entries)
{
    uint32_t *slot = input_ctx(1U);
    uint32_t dw0 = (g_route_string & 0xFFFFFU) |
                   ((g_port_speed & 0xFU) << 20) |
                   (g_slot_hub ? (1U << 26) : 0U) |
                   ((context_entries & 0x1FU) << 27);
    uint32_t dw1 = ((g_root_hub_port & 0xFFU) << 16) |
                   ((g_slot_num_ports & 0xFFU) << 24);
    /* xHCI slot context table: TT Think Time is permitted on the
       HIGH-SPEED HUB's own slot only, not on its FS/LS children.
       Children carry TT Hub Slot ID and TT Port Number, but TTT=0. */
    uint32_t dw2 = (g_tt_hub_slot & 0xFFU) |
                   ((g_tt_port & 0xFFU) << 8) |
                   (((g_slot_hub && g_port_speed == 3U ?
                      g_tt_think_time : 0U) & 3U) << 16);
    context_write32(slot, 0U, dw0);
    context_write32(slot, 1U, dw1);
    context_write32(slot, 2U, dw2);
    context_write32(slot, 3U, 0U);
}

static void fill_ep_context(uint32_t dci, struct XHCIRing *ring,
                            uint32_t ep_type, uint32_t max_packet,
                            uint32_t interval)
{
    uint32_t *ep = input_ctx(dci + 1U);
    const uint64_t dma = host_dma(ring->trbs) | 1ULL; /* DCS=1 */
    context_write32(ep, 0U, (interval & 0xFFU) << 16);
    context_write32(ep, 1U, (3U << 1) | ((ep_type & 7U) << 3) |
                              ((max_packet & 0xFFFFU) << 16));
    context_write32(ep, 2U, (uint32_t)dma);
    context_write32(ep, 3U, (uint32_t)(dma >> 32));
    context_write32(ep, 4U, max_packet & 0xFFFFU);
}

static int address_device(void)
{
    struct XHCIEvent e;
    if (!command_submit(XHCI_CMD_ENABLE_SLOT, 0, 0, 1000U, &e))
        return 0;

    g_slot_id = (e.control >> 24) & 0xFFU;
    if (!g_slot_id || g_slot_id > XHCI_MAX_SLOTS_USED) {
        g_host.error = FRFXHCI_HE_ADDRESS_DEVICE;
        return 0;
    }

    host_zero(g_device_context, sizeof(g_device_context));
    host_zero(g_input_context, sizeof(g_input_context));
    ring_init(&g_ep0_ring, g_ep0_trbs, XHCI_RING_TRBS);

    const uint64_t dev_dma = host_dma(g_device_context);
    g_dcbaa[g_slot_id] = LE64(dev_dma);
    host_cache_clean(&g_dcbaa[g_slot_id], sizeof(g_dcbaa[g_slot_id]));

    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 0U, 0U);          /* drop flags */
    context_write32(icc, 1U, (1U << 0) | (1U << 1)); /* slot + EP0 */

    fill_slot_context(1U);
    g_ep0_mps = ep0_initial_mps(g_port_speed);
    fill_ep_context(1U, &g_ep0_ring, 4U, g_ep0_mps, 0U);
    host_cache_clean(g_input_context, sizeof(g_input_context));

    if (!command_submit(XHCI_CMD_ADDRESS_DEVICE, host_dma(g_input_context),
                        g_slot_id << 24, 1500U, &e)) {
        g_host.error = FRFXHCI_HE_ADDRESS_DEVICE;
        return 0;
    }

    g_host.slot_id = g_slot_id;
    g_host.ep0_mps = g_ep0_mps;
    return 1;
}

static int evaluate_ep0_mps(uint32_t new_mps)
{
    if (!new_mps || new_mps == g_ep0_mps)
        return 1;

    host_zero(g_input_context, sizeof(g_input_context));
    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 1U, (1U << 1)); /* EP0 only */
    fill_ep_context(1U, &g_ep0_ring, 4U, new_mps, 0U);
    host_cache_clean(g_input_context, sizeof(g_input_context));

    if (!command_submit(XHCI_CMD_EVALUATE_CONTEXT, host_dma(g_input_context),
                        g_slot_id << 24, 1000U, NULL))
        return 0;
    g_ep0_mps = new_mps;
    g_host.ep0_mps = new_mps;
    return 1;
}

int frf_xhci_host_control(uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length, int direction_in,
                          uint32_t *actual)
{
    if (host_cpu() != 1U || !g_slot_id)
        return 0;
    if (length && !data)
        return 0;

    frf_r86_last_ctl_request=((uint32_t)request_type<<24)|
        ((uint32_t)request<<16)|((uint32_t)value&0xffffu);
    frf_r86_last_ctl_length=length;
    uint64_t setup_dma = 0, data_dma = 0, status_dma = 0;
    struct XHCITrb *setup = ring_next(&g_ep0_ring, &setup_dma);
    /* R87: retain the cycle that belongs to SETUP.  Later ring_next()
       calls can move the producer past a Link TRB and toggle ring->cycle. */
    const uint32_t setup_cycle=g_ep0_ring.cycle;
    struct XHCITrb *data_trb = NULL;
    struct XHCITrb *status = NULL;

    const uint32_t setup0 = (uint32_t)request_type |
                            ((uint32_t)request << 8) |
                            ((uint32_t)value << 16);
    const uint32_t setup1 = (uint32_t)index | ((uint32_t)length << 16);
    setup->parameter_lo = LE32(setup0);
    setup->parameter_hi = LE32(setup1);
    setup->status = LE32(8U);
    uint32_t trt = length ? (direction_in ? 3U : 2U) : 0U;
    /* Linux xhci_queue_ctrl_tx uses no CHAIN bit on the SETUP or single
       DATA Stage TRBs.  Keep the first TRB NOT OWNED by xHC until the
       complete DATA and STATUS stages are ready (cycle deliberately
       opposite), then publish it last immediately before ringing doorbell.
       This avoids xHC executing partially constructed control TDs. */
    const uint32_t setup_ready=XHCI_TRB_TYPE(XHCI_TRB_SETUP) |
        XHCI_TRB_IDT | (trt << 16) |
        (setup_cycle ? XHCI_TRB_CYCLE : 0U);
    setup->control=LE32(setup_ready ^ XHCI_TRB_CYCLE);
    host_cache_clean(setup,sizeof(*setup));

    if (length) {
        data_trb = ring_next(&g_ep0_ring, &data_dma);
        const uint64_t dma = host_dma(data);
        data_trb->parameter_lo = LE32((uint32_t)dma);
        data_trb->parameter_hi = LE32((uint32_t)(dma >> 32));
        data_trb->status = LE32((uint32_t)length);
        data_trb->control = LE32(XHCI_TRB_TYPE(XHCI_TRB_DATA) |
                                 XHCI_TRB_ISP |
                                 (direction_in ? XHCI_TRB_DIR_IN : 0U) |
                                 (g_ep0_ring.cycle ? XHCI_TRB_CYCLE : 0U));
        if (direction_in)
            host_cache_invalidate(data, length);
        else
            host_cache_clean(data, length);
        host_cache_clean(data_trb, sizeof(*data_trb));
    }

    status = ring_next(&g_ep0_ring, &status_dma);
    status->status = 0;
    status->control = LE32(XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
                           ((!length || !direction_in) ? XHCI_TRB_DIR_IN : 0U) |
                           (g_ep0_ring.cycle ? XHCI_TRB_CYCLE : 0U));
    host_cache_clean(status, sizeof(*status));

    /* Release complete control TD atomically via SETUP cycle, as in Linux.
       xHC cannot see an executable Setup Stage before its following stages. */
    setup->control=LE32(setup_ready);
    host_cache_clean(setup,sizeof(*setup));
    host_write32(g_db + (uintptr_t)g_slot_id * 4U, 1U);

    uint32_t got = length;
    if (!event_wait_transfer_trb(status_dma, data_dma, 1500U, length, &got)) {
        g_host.error = FRFXHCI_HE_CONTROL_TRANSFER;
        kprintf("[FRFXHCI-HOST] control req=%02x/%02x TD completion failed\n",
                request_type, request);
        return 0;
    }

    if (direction_in && length)
        host_cache_invalidate(data, length);
    if (actual)
        *actual = got;
    (void)setup_dma; (void)data_dma;
    return 1;
}

static int get_descriptor(uint8_t type, uint8_t index, uint16_t lang,
                          void *buf, uint16_t len, uint32_t *actual)
{
    return frf_xhci_host_control(0x80U, USB_REQ_GET_DESCRIPTOR,
                                 ((uint16_t)type << 8) | index,
                                 lang, buf, len, 1, actual);
}

static void usb_string_ascii(uint8_t index, uint16_t lang, char *dst, uint32_t cap)
{
    if (!cap) return;
    dst[0] = 0;
    if (!index)
        return;
    frf_zero_bytes(g_ctrl_buffer, 256U);
    uint32_t got = 0;
    if (!get_descriptor(USB_DT_STRING, index, lang, g_ctrl_buffer, 255U, &got) || got < 2U)
        return;
    uint32_t n = g_ctrl_buffer[0];
    if (n > got) n = got;
    uint32_t o = 0;
    for (uint32_t i = 2U; i + 1U < n && o + 1U < cap; i += 2U) {
        uint16_t wc = (uint16_t)g_ctrl_buffer[i] | ((uint16_t)g_ctrl_buffer[i + 1U] << 8);
        dst[o++] = (wc >= 32U && wc < 127U) ? (char)wc : '?';
    }
    dst[o] = 0;
}

static int configure_bulk_endpoints(uint32_t in_ep, uint32_t out_ep,
                                    uint32_t in_mps, uint32_t out_mps)
{
    g_bulk_in_dci = 2U * (in_ep & 0x0FU) + 1U;
    g_bulk_out_dci = 2U * (out_ep & 0x0FU);
    if (g_bulk_in_dci < 2U || g_bulk_in_dci > 31U ||
        g_bulk_out_dci < 2U || g_bulk_out_dci > 31U)
        return 0;

    ring_init(&g_bulk_in_ring, g_bulk_in_trbs, XHCI_RING_TRBS);
    ring_init(&g_bulk_out_ring, g_bulk_out_trbs, XHCI_RING_TRBS);
    host_zero(g_input_context, sizeof(g_input_context));

    const uint32_t highest = g_bulk_in_dci > g_bulk_out_dci ? g_bulk_in_dci : g_bulk_out_dci;
    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 1U, (1U << 0) | (1U << g_bulk_in_dci) | (1U << g_bulk_out_dci));
    fill_slot_context(highest);
    fill_ep_context(g_bulk_in_dci, &g_bulk_in_ring, 6U, in_mps, 0U);
    fill_ep_context(g_bulk_out_dci, &g_bulk_out_ring, 2U, out_mps, 0U);
    host_cache_clean(g_input_context, sizeof(g_input_context));

    if (!command_submit(XHCI_CMD_CONFIG_EP, host_dma(g_input_context),
                        g_slot_id << 24, 1500U, NULL)) {
        g_host.error = FRFXHCI_HE_CONFIG_ENDPOINT;
        return 0;
    }
    return 1;
}

/* FRF_R20_DIRECT_BOOT_MOUSE */
static int host_scan_cpu1(void);

static int configure_interrupt_in(uint32_t ep, uint32_t mps, uint32_t interval)
{
    /* OUTPUT and INPUT must be configured together for Nintendo Pro USB.
       Other HID devices continue to get only their existing IN endpoint. */
    g_hid_out_dci=0u;
    if(g_r96_hid_out_ep && g_r96_hid_out_mps) {
        g_hid_out_dci=2u*(g_r96_hid_out_ep&0x0fu);
        if(g_hid_out_dci<2u || g_hid_out_dci>31u)
            g_hid_out_dci=0u;
    }
    g_hid_in_dci = 2U * (ep & 0x0FU) + 1U;
    if (g_hid_in_dci < 2U || g_hid_in_dci > 31U || !mps) return 0;
    ring_init(&g_hid_in_ring, g_hid_in_trbs[g_native_bank], XHCI_RING_TRBS);
    if(g_hid_out_dci)
        ring_init(&g_hid_out_ring,g_hid_out_trbs[g_native_bank],XHCI_RING_TRBS);
    host_zero(g_input_context, sizeof(g_input_context));
    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 1U, (1U << 0) | (1U << g_hid_in_dci) |
                               (g_hid_out_dci?(1u<<g_hid_out_dci):0u));
    uint32_t highest=g_hid_in_dci;
    if(g_hid_out_dci>highest)highest=g_hid_out_dci;
    fill_slot_context(highest);
    /* xHCI uses 125us * 2^Interval. USB FS/LS bInterval is
     * milliseconds, so floor(log2(bInterval*8)); USB HS/SS bInterval
     * is already an exponent+1. Never copy descriptor bytes directly. */
    uint32_t xint;
    if (g_port_speed >= 3U) {
        uint32_t bi=interval?interval:1U;
        if(bi>16U)bi=16U;
        xint=bi-1U;
    } else {
        uint32_t microframes=(interval?interval:1U)*8U;
        xint=0U;
        while(microframes>1U) { xint++; microframes>>=1U; }
        if(xint<3U)xint=3U;
        if(xint>10U)xint=10U;
    }
    if(g_native_bank<FRF_NATIVE_HID_MAX) {
        frf_r82_interval[g_native_bank]=xint;
        frf_r82_speed[g_native_bank]=g_port_speed;
    }
    kprintf("[R82-HID] slot=%u port=%u speed=%u ep=%u mps=%u bInt=%u xInt=%u TTslot=%u TTport=%u\n",
             g_slot_id,g_root_hub_port,g_port_speed,ep,mps,interval,xint,
             g_tt_hub_slot,g_tt_port);
    fill_ep_context(g_hid_in_dci, &g_hid_in_ring, 7U, mps, xint);
    if(g_hid_out_dci) {
        uint32_t oi=g_r96_hid_out_interval?g_r96_hid_out_interval:interval;
        uint32_t out_int;
        if(g_port_speed>=3u)out_int=(oi>16u?16u:oi?oi:1u)-1u;
        else {
            uint32_t uf=oi*8u;out_int=0u;
            while(uf>1u){out_int++;uf>>=1u;}
            if(out_int<3u)out_int=3u;
            if(out_int>10u)out_int=10u;
        }
        fill_ep_context(g_hid_out_dci,&g_hid_out_ring,3u,
                        g_r96_hid_out_mps,out_int);
    }
    host_cache_clean(g_input_context, sizeof(g_input_context));
    int ok=command_submit(XHCI_CMD_CONFIG_EP, host_dma(g_input_context),
                          g_slot_id << 24, 1500U, NULL);
    if(ok && g_hid_out_dci)++frf_r96_switch_out_config;
    return ok;
}

int frf_xhci_host_hid_ready(void) { return g_hid_ready ? 1 : 0; }

int frf_xhci_host_hid_poll(void *data, uint32_t length, uint32_t *actual)
{
    if (host_cpu() != 1U || !g_hid_ready || !data || !length) return 0;
    uint64_t trb_dma = 0;
    struct XHCITrb *t = ring_next(&g_hid_in_ring, &trb_dma);
    uint64_t dma = host_dma(data);
    t->parameter_lo = LE32((uint32_t)dma);
    t->parameter_hi = LE32((uint32_t)(dma >> 32));
    t->status = LE32(length);
    t->control = LE32(XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP |
                      (g_hid_in_ring.cycle ? XHCI_TRB_CYCLE : 0U));
    host_cache_invalidate(data, length);
    host_cache_clean(t, sizeof(*t));
    host_write32(g_db + (uintptr_t)g_slot_id * 4U, g_hid_in_dci);
    uint32_t got = length;
    if (!event_wait_transfer_trb(trb_dma, 0U, 1000U, length, &got)) return 0;
    host_cache_invalidate(data, length);
    if (actual) *actual = got;
    return 1;
}

int frf_xhci_host_hid_scan_cpu1(void)
{
    int ok=host_scan_cpu1();
    if(ok && !g_native_count && g_hid_ready &&
       g_host.interface_class==USB_CLASS_HID) {
        struct NativeHID *h=&g_native_hid[0];
        h->slot=g_slot_id; h->protocol=g_host.interface_protocol;
        h->interface=g_hid_iface; h->dci=g_hid_in_dci;
        h->ep=0u; h->mps=g_hid_mps; h->vid=g_host.vid;h->pid=g_host.pid;
        h->report_desc_length=0u; h->ring=g_hid_in_ring;
        h->ep0=g_ep0_ring;h->pending=0u;h->valid=1u;
        g_native_count=1u;
    }
    frf_r83_hid_count=g_native_count;
    return ok;
}

uint32_t frf_xhci_host_native_count(void) { return g_native_count; }
int frf_xhci_host_native_info(uint32_t index,uint32_t *proto,uint32_t *iface)
{
    if(index>=g_native_count || !g_native_hid[index].valid)return 0;
    if(proto)*proto=g_native_hid[index].protocol;
    if(iface)*iface=g_native_hid[index].interface;
    return 1;
}
int frf_xhci_host_native_identity(uint32_t index,uint32_t *vid,
                                  uint32_t *pid,uint32_t *report_desc_length)
{
    if(index>=g_native_count || !g_native_hid[index].valid)return 0;
    const struct NativeHID *h=&g_native_hid[index];
    if(vid)*vid=h->vid;
    if(pid)*pid=h->pid;
    if(report_desc_length)*report_desc_length=h->report_desc_length;
    return 1;
}
int frf_xhci_host_native_control(uint32_t index,
        uint8_t request_type,uint8_t request,uint16_t value,uint16_t ifnum,
        void *data,uint16_t length,int direction_in,uint32_t *actual)
{
    if(host_cpu()!=1U || index>=g_native_count ||
       !g_native_hid[index].valid || g_native_hid[index].pending)return 0;
    struct NativeHID *h=&g_native_hid[index];
    g_slot_id=h->slot;
    g_ep0_ring=h->ep0;
    int ok=frf_xhci_host_control(request_type,request,value,ifnum,
                                  data,length,direction_in,actual);
    h->ep0=g_ep0_ring;
    return ok;
}

/* R96: synchronous Nintendo Switch Pro HID interrupt-OUT/IN bootstrap.
 * The real Switch Pro has both interrupt endpoints (USB 057e:2009).
 * Call ONLY before native async polling begins; no concurrent IN TRBs
 * are then active for this specific slot. Transfer buffers stay 64B
 * cacheline aligned inside its persistent NativeHID record. */
int frf_xhci_host_native_interrupt_out(uint32_t index,const uint8_t *bytes,
                                       uint32_t length)
{
    if(host_cpu()!=1u || index>=g_native_count || !bytes ||
       !length || length>64u)return 0;
    struct NativeHID *h=&g_native_hid[index];
    if(!h->valid || !h->out_dci || !h->out_mps || h->pending)return 0;
    for(uint32_t n=0;n<64u;n++)h->out_buffer[n]=n<length?bytes[n]:0u;
    host_cache_clean(h->out_buffer,64u);
    uint64_t trb_dma=0u;
    struct XHCITrb *t=ring_next(&h->out_ring,&trb_dma);
    uint64_t buffer_dma=host_dma(h->out_buffer);
    if(trb_dma==UINT64_MAX || buffer_dma==UINT64_MAX)return 0;
    t->parameter_lo=LE32((uint32_t)buffer_dma);
    t->parameter_hi=LE32((uint32_t)(buffer_dma>>32));
    t->status=LE32(length);
    t->control=LE32(XHCI_TRB_TYPE(XHCI_TRB_NORMAL)|XHCI_TRB_IOC|
                    (h->out_ring.cycle?XHCI_TRB_CYCLE:0u));
    host_cache_clean(t,sizeof(*t));
    host_write32(g_db+(uintptr_t)h->slot*4u,h->out_dci);
    uint32_t actual=length;
    int ok=event_wait_transfer_trb(trb_dma,0u,800u,length,&actual);
    if(ok)++frf_r96_switch_tx_ok;
    else ++frf_r96_switch_tx_fail;
    frf_r96_switch_last_tx=bytes[0];
    frf_r96_switch_last_cc=frf_r84_hub_cc;
    return ok;
}
int frf_xhci_host_native_interrupt_in_once(uint32_t index,
             uint8_t *bytes,uint32_t capacity,uint32_t *actual)
{
    if(host_cpu()!=1u || index>=g_native_count || !bytes ||
       capacity<64u)return 0;
    struct NativeHID *h=&g_native_hid[index];
    if(!h->valid || h->pending)return 0;
    host_cache_invalidate(h->report,64u);
    uint64_t td_dma=0u;
    struct XHCITrb *t=ring_next(&h->ring,&td_dma);
    uint64_t buf_dma=host_dma(h->report);
    if(td_dma==UINT64_MAX || buf_dma==UINT64_MAX)return 0;
    uint32_t length=(h->mps && h->mps<64u)?h->mps:64u;
    t->parameter_lo=LE32((uint32_t)buf_dma);
    t->parameter_hi=LE32((uint32_t)(buf_dma>>32));
    t->status=LE32(length);
    t->control=LE32(XHCI_TRB_TYPE(XHCI_TRB_NORMAL)|XHCI_TRB_IOC|
              XHCI_TRB_ISP|(h->ring.cycle?XHCI_TRB_CYCLE:0u));
    host_cache_clean(t,sizeof(*t));
    host_write32(g_db+(uintptr_t)h->slot*4u,h->dci);
    uint32_t received=length;
    if(!event_wait_transfer_trb(td_dma,0u,300u,length,&received))
        return 0;
    host_cache_invalidate(h->report,64u);
    if(received>64u)return 0;
    for(uint32_t i=0;i<received;i++)bytes[i]=h->report[i];
    if(actual)*actual=received;
    if(received)frf_r96_switch_last_rx=bytes[0];
    return 1;
}

/* R92 per-slot endpoint recovery. A stalled xHC endpoint needs Reset
 * Endpoint then Set TR Dequeue, not just retrying a halted transfer ring.
 * Silence can also happen when a wireless receiver drops pairing while an
 * interrupt-IN TD is pending. Stop that endpoint, then set its dequeue
 * pointer; never reset the working keyboard/mouse/controller as a group.
 *
 * Both commands are CPU1-only, and concurrent Transfer Events are preserved
 * by the deferred queue above. No root-hub reset, PCIe reset, or AmigaOS
 * USB calls. */
static void frf_r92_schedule_recovery(unsigned i,uint32_t cc,int stop_first)
{
    struct NativeHID *h=&g_native_hid[i];
    uint32_t tries=h->recover_count;
    if(tries<1000000u)++tries;
    h->recover_count=tries;
    uint32_t ms=200u+(tries<18u ? tries*100u : 1800u);
    uint64_t hz=host_counter_hz();
    if(!hz)hz=54000000u;
    h->retry_after_cnt=host_counter()+((uint64_t)ms*hz)/1000u;
    h->recover_pending=1u;
    h->recover_cc=cc;
    h->recover_stop=stop_first?1u:0u;
    if(h->protocol==0u)
        frf_native_usb_pad_release();
    frf_r92_recovery_state[i]=1u;
    frf_r92_recovery_attempts[i]=tries;
    frf_r92_last_cc[i]=cc;
}
static int frf_r92_recover_endpoint(unsigned i)
{
    struct NativeHID *h=&g_native_hid[i];
    uint32_t dest=(h->slot<<24)|(h->dci<<16);
    frf_r92_recovery_state[i]=2u;
    /* A pending-but-silent TD may still be owned by the xHC. Stop its
       endpoint before abandoning it. A STALL/USB transaction error has
       already completed and may have left the endpoint HALTED. */
    int ok;
    if(h->recover_stop)
        ok=command_submit(XHCI_CMD_STOP_ENDPOINT,0u,dest,500u,NULL);
    else
        ok=command_submit(XHCI_CMD_RESET_ENDPOINT,0u,dest,500u,NULL);
    if(!ok) {
        /* STOP can race with HALTED state, while RESET can encounter a
         * Stopped rather than Halted endpoint. Try the alternate state
         * transition exactly once, without resetting another slot. */
        ok=command_submit(h->recover_stop?XHCI_CMD_RESET_ENDPOINT:
                          XHCI_CMD_STOP_ENDPOINT,0u,dest,500u,NULL);
    }
    if(!ok) {
        frf_r92_recovery_state[i]=0xE1u;
        return 0;
    }
    /* For functional STALL clear USB device-side ENDPOINT_HALT. A host
       protocol stall might not set this feature, which is harmless if
       the device accepts the standard request. Do not fail recovery
       merely because a non-HID vendor receiver declines the request. */
    if(h->recover_cc==6u) {
        (void)frf_xhci_host_native_control(i,0x02u,USB_REQ_CLEAR_FEATURE,
                                         0u,(uint16_t)(0x80u|h->ep),
                                         NULL,0u,0,NULL);
    }

    /* R99: never re-use the exact pending DMA TRB pointer in the
       next recovery generation. Reset/Stop can leave delayed transfer
       events that would otherwise match a freshly posted TD and corrupt
       the HID state after several seconds of hub activity.
       Each bank has TWO disjoint rings with independent VL805 padding. */
    h->ring_epoch^=1u;
    struct XHCITrb *next_ring=&g_hid_in_trbs[h->ring_bank]
                              [h->ring_epoch*(XHCI_RING_TRBS*2u)];
    ring_init(&h->ring,next_ring,XHCI_RING_TRBS);
    ++frf_r99_ring_swaps;
    uint64_t new_deq=host_dma(h->ring.trbs);
    if(new_deq==UINT64_MAX ||
       !command_submit(XHCI_CMD_SET_TR_DEQUEUE,new_deq|1ULL,dest,500u,NULL)) {
        frf_r92_recovery_state[i]=0xE2u;
        return 0;
    }
    h->pending=0u;
    h->pending_since_cnt=0u;
    h->recover_pending=0u;
    h->recover_stop=0u;
    frf_r92_recovery_state[i]=3u;
    ++frf_r92_recovery_ok[i];
    ++frf_r99_hid_recovered_total;
    kprintf("[R99-USB] slot%u ep%u recovered (prior CC=%u tries=%u)\n",
            h->slot,h->dci,h->recover_cc,h->recover_count);
    return 1;
}

/* Start at most one IN TRB per device, then consume all completed events
 * without blocking CPU1 for a slow or idle keyboard. Workbench's physical
 * input state and both mouse buttons are driven by the ARM callback layer. */
int frf_xhci_host_native_poll_any(void *dst,uint32_t capacity,
                     uint32_t *actual,uint32_t *device,uint32_t *protocol)
{
    if(host_cpu()!=1U || !dst || capacity<64u || !g_native_count)return 0;
    uint64_t now=host_counter(),hz=host_counter_hz();
    if(!hz)hz=54000000u;
    /* A non-destructive hub class GET_STATUS samples actual hub-link
       state while HID endpoints are running. Synchronous completion
       helper retains unrelated xHCI Transfer Events for later dispatch. */
#ifndef FRF_RELEASE_QUIET
    frf_r99_monitor_root_port(); /* diagnostics only, NOT needed for HID */
#endif
    uint32_t pending_count=0u,latest_age=0u;
    for(uint32_t i=0;i<g_native_count;i++) {
        struct NativeHID *h=&g_native_hid[i];
        if(!h->valid)continue;
        if(h->pending)++pending_count;
        uint64_t last=frf_r88_hid_last_cnt[i];
        if(last && now>=last) {
            uint64_t age=(now-last)/hz;
            if(age>255u)age=255u;
            if(age>latest_age)latest_age=(uint32_t)age;
        }
        /* A wireless gamepad receiver may stop completing its IN TD when
           the paired controller drops sync. Do not permanently strand
           that slot, but also don't reset idle keyboard/mouse endpoints.
           Allow 10 seconds without a gamepad completion before Stop EP. */
        if(h->protocol==0u && h->pending && h->pending_since_cnt &&
           now-h->pending_since_cnt>=hz*10u && !h->recover_pending) {
            ++frf_r92_silent_timeout[i];
            frf_r92_schedule_recovery(i,0xffu,1);
        }
        if(h->recover_pending) {
            if((int64_t)(now-h->retry_after_cnt)>=0) {
                if(!frf_r92_recover_endpoint(i))
                    frf_r92_schedule_recovery(i,h->recover_cc,h->recover_stop);
            }
            /* No new TD until recovery has finished successfully. */
            continue;
        }
        if(h->pending)continue;
        /* USB HID interrupt endpoints return at most wMaxPacketSize per
           service opportunity.  Previous code always requested 64B even
           for boot keyboard endpoints advertising 8B, forcing every
           ordinary report through xHCI's short-packet handling. */
        uint32_t td_len=(h->mps && h->mps<64u)?h->mps:64u;
        /* R81: an IN DMA buffer must be invalidated *before* the device
         * owns it. host_zero() used to clean a dirty zero cache line, which
         * could later evict over a real USB report during xHCI DMA. */
        host_cache_invalidate(h->report,64u);
        uint64_t trb_dma=0;
        struct XHCITrb *t=ring_next(&h->ring,&trb_dma);
        uint64_t buf_dma=host_dma(h->report);
        if(trb_dma==UINT64_MAX || buf_dma==UINT64_MAX) {
            h->valid=0; continue;
        }
        t->parameter_lo=LE32((uint32_t)buf_dma);
        t->parameter_hi=LE32((uint32_t)(buf_dma>>32));
        t->status=LE32(td_len);
        t->control=LE32(XHCI_TRB_TYPE(XHCI_TRB_NORMAL)|
                         XHCI_TRB_IOC|XHCI_TRB_ISP|
                         (h->ring.cycle?XHCI_TRB_CYCLE:0u));
        host_cache_clean(t,sizeof(*t));
        h->pending_length=td_len;
        frf_r88_hid_td_size[i]=td_len;
        h->pending=trb_dma;
        h->pending_since_cnt=host_counter();
        ++frf_r82_arm[i];
        host_write32(g_db+(uintptr_t)h->slot*4u,h->dci);
    }
    frf_r99_hid_active_pending=pending_count;
    frf_r99_hid_last_age_seconds=latest_age;
    for(uint32_t ecount=0;ecount<8u;ecount++) {
        struct XHCIEvent event;
        uint32_t control,status;
        uint64_t event_ptr;
        if(frf_r92_pop_event(&event)) {
            control=event.control;
            status=event.status;
            event_ptr=event.parameter;
        } else {
            struct XHCITrb *t=&g_event_trbs[g_event_dequeue];
            host_cache_invalidate(t,sizeof(*t));
            control=LE32(t->control);
            if((control&XHCI_TRB_CYCLE)!=
                 (g_event_cycle?XHCI_TRB_CYCLE:0u))return 0;
            event_ptr=(uint64_t)LE32(t->parameter_lo)|
                     ((uint64_t)LE32(t->parameter_hi)<<32);
            status=LE32(t->status);
            frf_r58_note_event(status,control);
            event_ack();
        }
        if(((control>>XHCI_TRB_TYPE_SHIFT)&0x3fu)!=XHCI_EVT_TRANSFER)
            continue;
        const uint32_t event_slot=(control>>24)&0xffu;
        const uint32_t event_dci=(control>>16)&0x1fu;
        frf_r83_last_slot=event_slot;frf_r83_last_ep=event_dci;
        frf_r83_last_cc=(status>>24)&0xffu;
        uint32_t matched=0u;
        for(uint32_t i=0;i<g_native_count;i++) {
            struct NativeHID *h=&g_native_hid[i];
            if(!h->valid || !h->pending || h->slot!=event_slot ||
                h->dci!=event_dci ||
                (event_ptr&~15ULL)!=(h->pending&~15ULL))continue;
            matched=1u;
            h->pending=0u;
            h->pending_since_cnt=0u;
            uint32_t cc=(status>>24)&0xffu, residual=status&0xffffffu;
            uint32_t td_len=h->pending_length;
            frf_r82_last_cc[i]=cc;
            if((cc!=XHCI_CC_SUCCESS && cc!=XHCI_CC_SHORT_PACKET) ||
               residual>td_len) {
                ++frf_r82_failed[i];
                ++frf_r99_hid_error_count;
                frf_r99_last_hid_error_cc=cc;
                /* Pi3 retries after bounded delay. Pi4 must recover
                   the xHC endpoint's ring, not mark slot permanently dead.
                   STALL and USB Transaction Error may HALT the endpoint.
                   Preserve keyboard/mouse slots throughout. */
                if(h->failures<0xffu)++h->failures;
                frf_r92_schedule_recovery(i,cc,0);
                break;
            }
            h->failures=0u;
            h->recover_count=0u;
            frf_r92_recovery_state[i]=0u;
            uint32_t bytes=td_len-residual;
            host_cache_invalidate(h->report,64u);
            uint64_t current_cnt=host_counter();
            uint64_t last_cnt=frf_r88_hid_last_cnt[i];
            if(last_cnt && current_cnt>=last_cnt) {
                uint64_t hz=host_counter_hz();
                if(hz) {
                    uint32_t delta_ms=(uint32_t)(((current_cnt-last_cnt)*1000u)/hz);
                    frf_r88_hid_gap_last_ms[i]=delta_ms;
                    if(delta_ms>frf_r88_hid_gap_max_ms[i])
                        frf_r88_hid_gap_max_ms[i]=delta_ms;
                }
            }
            frf_r88_hid_last_cnt[i]=current_cnt;
            ++frf_r82_completed[i];
            frf_r82_last_bytes[i]=bytes;
            frf_r82_last_word[i]=(uint32_t)h->report[0] |
                ((uint32_t)h->report[1]<<8) |
                ((uint32_t)h->report[2]<<16) |
                ((uint32_t)h->report[3]<<24);
            for(uint32_t j=0;j<bytes;j++)
                ((uint8_t*)dst)[j]=h->report[j];
            if(actual)*actual=bytes;
            if(device)*device=i;
            if(protocol)*protocol=h->protocol;
            return 1;
        }
        if(!matched) {
            ++frf_r82_unknown_events;
            ++frf_r99_stale_events;
        }
    }
    return 0;
}

static int parse_and_configure(void)
{
    uint32_t got = 0;
    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));

    if (!get_descriptor(USB_DT_DEVICE, 0U, 0U, g_ctrl_buffer, 8U, &got) || got < 8U) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }

    uint32_t mps = g_ep0_mps;
    if (g_port_speed >= 4U)
        mps = 1U << g_ctrl_buffer[7];
    else
        mps = g_ctrl_buffer[7];
    if (mps != 8U && mps != 16U && mps != 32U && mps != 64U && mps != 512U) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }
    if (!evaluate_ep0_mps(mps))
        return 0;

    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if (!get_descriptor(USB_DT_DEVICE, 0U, 0U, g_ctrl_buffer, 18U, &got) || got < 18U) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }

    g_host.bcd_usb = (uint32_t)g_ctrl_buffer[2] | ((uint32_t)g_ctrl_buffer[3] << 8);
    g_host.device_class = g_ctrl_buffer[4];
    g_host.device_subclass = g_ctrl_buffer[5];
    g_host.device_protocol = g_ctrl_buffer[6];
    g_host.vid = (uint32_t)g_ctrl_buffer[8] | ((uint32_t)g_ctrl_buffer[9] << 8);
    g_host.pid = (uint32_t)g_ctrl_buffer[10] | ((uint32_t)g_ctrl_buffer[11] << 8);
    const uint8_t i_mfr = g_ctrl_buffer[14];
    const uint8_t i_product = g_ctrl_buffer[15];
    const uint8_t i_serial = g_ctrl_buffer[16];

    uint16_t lang = 0x0409U;
    frf_zero_bytes(g_ctrl_buffer, 16U);
    if (get_descriptor(USB_DT_STRING, 0U, 0U, g_ctrl_buffer, 16U, &got) && got >= 4U)
        lang = (uint16_t)g_ctrl_buffer[2] | ((uint16_t)g_ctrl_buffer[3] << 8);
    usb_string_ascii(i_mfr, lang, (char *)g_host.manufacturer, sizeof(g_host.manufacturer));
    usb_string_ascii(i_product, lang, (char *)g_host.product, sizeof(g_host.product));
    usb_string_ascii(i_serial, lang, (char *)g_host.serial, sizeof(g_host.serial));

    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if (!get_descriptor(USB_DT_CONFIG, 0U, 0U, g_ctrl_buffer, 9U, &got) || got < 9U) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }
    uint32_t total = (uint32_t)g_ctrl_buffer[2] | ((uint32_t)g_ctrl_buffer[3] << 8);
    if (total < 9U || total > sizeof(g_ctrl_buffer)) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }
    const uint32_t config_value = g_ctrl_buffer[5];
    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if (!get_descriptor(USB_DT_CONFIG, 0U, 0U, g_ctrl_buffer, (uint16_t)total, &got) || got < 9U) {
        g_host.error = FRFXHCI_HE_DESCRIPTOR;
        return 0;
    }

    uint32_t current_iface = 0xFFFFFFFFU;
    uint32_t current_class = 0U, current_sub = 0U, current_proto = 0U;
    uint32_t msc_iface = 0xFFFFFFFFU;
    uint32_t bulk_in_ep = 0U, bulk_out_ep = 0U, bulk_in_mps = 0U, bulk_out_mps = 0U;
    uint32_t hid_iface = 0xFFFFFFFFU, hid_in_ep = 0U, hid_in_mps = 0U, hid_interval = 0U, hid_proto = 0U;

    for (uint32_t off = 0; off + 2U <= got; ) {
        const uint32_t len = g_ctrl_buffer[off];
        const uint32_t type = g_ctrl_buffer[off + 1U];
        if (len < 2U || off + len > got)
            break;
        if (type == USB_DT_INTERFACE && len >= 9U) {
            current_iface = g_ctrl_buffer[off + 2U];
            current_class = g_ctrl_buffer[off + 5U];
            current_sub = g_ctrl_buffer[off + 6U];
            current_proto = g_ctrl_buffer[off + 7U];
            if (g_host.interface_number == 0U && g_host.interface_class == 0U) {
                g_host.interface_number = current_iface;
                g_host.interface_class = current_class;
                g_host.interface_subclass = current_sub;
                g_host.interface_protocol = current_proto;
            }
            if (hid_iface == 0xFFFFFFFFU && current_class == USB_CLASS_HID &&
                (current_sub == USB_HID_SUBCLASS_BOOT || current_sub == 0u) &&
                (current_proto == USB_HID_PROTOCOL_KEYBOARD ||
                 current_proto == USB_HID_PROTOCOL_MOUSE ||
                 current_proto == 0u)) {
                hid_iface = current_iface; hid_proto = current_proto;
                hid_in_ep = hid_in_mps = hid_interval = 0U;
            }
            if (msc_iface == 0xFFFFFFFFU && current_class == USB_CLASS_MASS_STORAGE &&
                current_sub == USB_MSC_SUBCLASS_SCSI && current_proto == USB_MSC_PROTOCOL_BOT) {
                msc_iface = current_iface;
                g_host.interface_number = current_iface;
                g_host.interface_class = current_class;
                g_host.interface_subclass = current_sub;
                g_host.interface_protocol = current_proto;
                bulk_in_ep = bulk_out_ep = bulk_in_mps = bulk_out_mps = 0U;
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7U && current_iface == hid_iface) {
            const uint8_t addr = g_ctrl_buffer[off + 2U];
            const uint8_t attr = g_ctrl_buffer[off + 3U];
            const uint32_t ep = addr & 0x0FU;
            const uint32_t mps_ep = ((uint32_t)g_ctrl_buffer[off + 4U] |
                                     ((uint32_t)g_ctrl_buffer[off + 5U] << 8)) & 0x7FFU;
            if ((attr & 3U) == 3U && (addr & 0x80U) && ep) {
                hid_in_ep = ep; hid_in_mps = mps_ep; hid_interval = g_ctrl_buffer[off + 6U];
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7U && current_iface == msc_iface) {
            const uint8_t addr = g_ctrl_buffer[off + 2U];
            const uint8_t attr = g_ctrl_buffer[off + 3U];
            const uint32_t ep = addr & 0x0FU;
            const uint32_t mps_ep = ((uint32_t)g_ctrl_buffer[off + 4U] |
                                     ((uint32_t)g_ctrl_buffer[off + 5U] << 8)) & 0x7FFU;
            if ((attr & 3U) == 2U && ep) {
                if (addr & 0x80U) { bulk_in_ep = ep; bulk_in_mps = mps_ep; }
                else { bulk_out_ep = ep; bulk_out_mps = mps_ep; }
            }
        }
        off += len;
    }

    g_host.configuration_value = config_value;
    g_host.state = FRFXHCI_HOST_DEVICE;
    g_hid_ready = 0U;
    if (hid_iface != 0xFFFFFFFFU && hid_in_ep && hid_in_mps) {
        if (!configure_interrupt_in(hid_in_ep, hid_in_mps, hid_interval)) return 0;
        if (!frf_xhci_host_control(0x00U, USB_REQ_SET_CONFIGURATION,
                                   (uint16_t)config_value, 0U, NULL, 0U, 0, NULL)) return 0;
        (void)frf_xhci_host_control(0x21U, USB_REQ_SET_PROTOCOL, 0U,
                                    (uint16_t)hid_iface, NULL, 0U, 0, NULL);
        g_hid_iface = hid_iface; g_hid_mps = hid_in_mps; g_hid_ready = 1U;
        g_host.interface_number = hid_iface; g_host.interface_class = USB_CLASS_HID;
        g_host.interface_subclass = 0u; g_host.interface_protocol = hid_proto;
        kprintf("[PI4-NATIVE-USB] ARM HID %s iface%u ep%u mps%u interval%u\n",
                hid_proto==USB_HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse",
                hid_iface, hid_in_ep, hid_in_mps, hid_interval);
        return 1;
    }

    if (msc_iface != 0xFFFFFFFFU && bulk_in_ep && bulk_out_ep && bulk_in_mps && bulk_out_mps) {
        if (!configure_bulk_endpoints(bulk_in_ep, bulk_out_ep, bulk_in_mps, bulk_out_mps))
            return 0;
        if (!frf_xhci_host_control(0x00U, USB_REQ_SET_CONFIGURATION,
                                   (uint16_t)config_value, 0U, NULL, 0U, 0, NULL))
            return 0;
        g_host.bulk_in_ep = bulk_in_ep;
        g_host.bulk_out_ep = bulk_out_ep;
        g_host.bulk_in_mps = bulk_in_mps;
        g_host.bulk_out_mps = bulk_out_mps;
        g_host.msc_ready = 1U;
        g_host.state = FRFXHCI_HOST_MASS_STORAGE;
    }

    return 1;
}


/* -------------------------------------------------------------------------
 * FRF V0.5.5 SELF-CONTAINED ONE-TIER USB2 HUB ENUMERATION
 * ------------------------------------------------------------------------- */
static void hub_zero_devices(void)
{
    frf_zero_bytes(g_hub_devices, sizeof(g_hub_devices));
    g_hub_device_count = 0U;
}

static void hub_delay_ms(uint32_t ms)
{
    const uint64_t deadline = host_deadline_ms(ms);
    while (!host_timed_out(deadline))
        __asm__ volatile("yield");
}

static uint32_t hub_child_speed(uint16_t status)
{
    if (status & USB_HUB_PORT_HIGH_SPEED) return 3U;
    if (status & USB_HUB_PORT_LOW_SPEED) return 2U;
    return 1U;
}

/* R57: xHCI DMA must never target an unaligned four-byte stack local.
 * The old b[4] used host_cache_invalidate(), which INVALIDATED the entire
 * adjacent 64-byte line and could corrupt unrelated live stack objects.
 * Use the existing dedicated aligned descriptor buffer, controlled by CPU1. */
static int hub_port_status(uint32_t port, uint16_t *status, uint16_t *change)
{
    uint32_t got = 0U;
    frf_zero_bytes(g_ctrl_buffer, 64U);
    if (!frf_xhci_host_control(0xA3U, USB_REQ_GET_STATUS, 0U, (uint16_t)port,
                               g_ctrl_buffer, 4U, 1, &got) || got < 4U) {
        ++frf_r84_hub_status_fail;
        return 0;
    }
    ++frf_r84_hub_status_ok;
    if (status) *status = (uint16_t)g_ctrl_buffer[0] | ((uint16_t)g_ctrl_buffer[1] << 8);
    if (change) *change = (uint16_t)g_ctrl_buffer[2] | ((uint16_t)g_ctrl_buffer[3] << 8);
    if(port>=1u && port<=4u) {
        frf_r84_hub_last_status[port-1u]=(uint32_t)g_ctrl_buffer[0]|((uint32_t)g_ctrl_buffer[1]<<8);
        frf_r84_hub_last_change[port-1u]=(uint32_t)g_ctrl_buffer[2]|((uint32_t)g_ctrl_buffer[3]<<8);
    }
    return 1;
}

/* R99 live root-hub status poll, every two seconds at most.
 * No PORT_RESET, no SET_FEATURE, no xHC reset, no HID slot churn.
 * Save and restore CPU1's currently selected global EP0 context. A
 * port-status request is a *control* TD; unrelated HID transfer events
 * are deferred by the existing event_wait_transfer_trb() path. */
#ifndef FRF_RELEASE_QUIET
static void frf_r99_monitor_root_port(void)
{
    if(!g_r99_root_valid || !frf_r99_root_monitor_port ||
       !g_root_hub_slot)return;
    uint64_t hz=host_counter_hz();
    if(!hz)hz=54000000u;
    uint64_t now=host_counter();
    if(g_r99_root_next_check && now<g_r99_root_next_check)return;
    g_r99_root_next_check=now+hz*2u;

    const uint32_t saved_slot=g_slot_id,saved_speed=g_port_speed,
          saved_mps=g_ep0_mps;
    const struct XHCIRing saved_ring=g_ep0_ring;
    g_slot_id=g_root_hub_slot;
    g_port_speed=g_root_hub_speed;
    g_ep0_mps=g_r99_root_ep0_mps;
    g_ep0_ring=g_r99_root_ep0_ring;
    uint16_t st=0u,ch=0u;
    int ok=hub_port_status(frf_r99_root_monitor_port,&st,&ch);
    g_r99_root_ep0_ring=g_ep0_ring;
    g_slot_id=saved_slot;
    g_port_speed=saved_speed;
    g_ep0_mps=saved_mps;
    g_ep0_ring=saved_ring;
    if(ok) {
        ++frf_r99_root_poll_ok;
        uint32_t before=frf_r99_live_link;
        frf_r99_root_status=st;
        frf_r99_root_change=ch;
        frf_r99_live_link=
            ((st&(USB_HUB_PORT_CONNECTION|USB_HUB_PORT_ENABLE))==
                    (USB_HUB_PORT_CONNECTION|USB_HUB_PORT_ENABLE))?1u:0u;
        if(before && !frf_r99_live_link) {
            ++frf_r99_root_link_lost;
            kprintf("[R99-HUB] UPSTREAM LINK LOST port%u status=%04x change=%04x\n",
               frf_r99_root_monitor_port,st,ch);
        } else if(!before && frf_r99_live_link) {
            ++frf_r99_root_link_up;
            kprintf("[R99-HUB] UPSTREAM LINK UP port%u status=%04x change=%04x\n",
               frf_r99_root_monitor_port,st,ch);
        }
    } else {
        ++frf_r99_root_poll_fail;
    }
}
#endif /* !FRF_RELEASE_QUIET: omit diagnostic live hub poll */
static int hub_port_feature(uint32_t port, uint16_t feature, int set)
{
    return frf_xhci_host_control(0x23U,
        set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE,
        feature, (uint16_t)port, NULL, 0U, 0, NULL);
}

static int hub_reset_downstream_port(uint32_t port, uint32_t *speed_out)
{
    uint16_t st = 0U, ch = 0U;
    if (!hub_port_status(port, &st, &ch) || !(st & USB_HUB_PORT_CONNECTION))
        return 0;
    if (!hub_port_feature(port, USB_HUB_FEAT_PORT_RESET, 1))
        return 0;
    const uint64_t deadline = host_deadline_ms(750U);
    do {
        hub_delay_ms(10U);
        if (!hub_port_status(port, &st, &ch))
            return 0;
        if (!(st & USB_HUB_PORT_RESET) && (st & USB_HUB_PORT_ENABLE)) {
            (void)hub_port_feature(port, USB_HUB_FEAT_C_PORT_RESET, 0);
            (void)hub_port_feature(port, USB_HUB_FEAT_C_PORT_ENABLE, 0);
            (void)hub_port_feature(port, USB_HUB_FEAT_C_PORT_CONNECTION, 0);
            if (speed_out)
                *speed_out = hub_child_speed(st);
            /* R98: bounded per-root-port reset recovery. External
               nested hubs use 20ms when the override is unset. */
            uint32_t recovery=frf_r98_root_hs_recovery_ms;
            hub_delay_ms(recovery?recovery:20u);
            return 1;
        }
    } while (!host_timed_out(deadline));
    return 0;
}

static int hub_mark_root_slot(uint32_t ports)
{
    host_zero(g_input_context, sizeof(g_input_context));
    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 1U, (1U << 0));
    g_route_string = 0U;
    g_root_hub_port = g_selected_root_port;
    g_slot_hub = 1U;
    g_slot_num_ports = ports;
    g_tt_hub_slot = 0U;
    g_tt_port = 0U;
    g_tt_think_time = (g_root_hub_speed == 3U) ? g_hub_tt_think_time : 0U;
    fill_slot_context(1U);
    host_cache_clean(g_input_context, sizeof(g_input_context));

    /* FRF_CPU1_USB_HUB_RUNTIME_FIX_V0_5_10
     * xHCI 1.x Evaluate Context does not update Hub/Number-of-Ports/TTT.
     * These are Configure Endpoint Slot Context inputs. */
    if (!command_submit(XHCI_CMD_CONFIG_EP, host_dma(g_input_context),
                        g_slot_id << 24, 1500U, NULL)) {
        kprintf("[FRFXHCI-HOST] HUB root slot Configure Endpoint failed ports=%u ttt=%u\n",
                ports, g_tt_think_time);
        return 0;
    }
    kprintf("[FRFXHCI-HOST] HUB root slot configured ports=%u speed=%u ttt=%u\n",
            ports, g_root_hub_speed, g_tt_think_time);
    return 1;
}

static int address_child_device(uint32_t hub_port, uint32_t speed,
                                uint32_t hub_slot, uint32_t hub_speed,
                                uint32_t tt_think,
                                uint32_t hub_route, uint32_t hub_depth)
{
    struct XHCIEvent e;
    const uint32_t port_index=(hub_port>=1U && hub_port<=4U)?hub_port-1U:4U;
    if(port_index<4U) frf_r85_speed[port_index]=speed;
    if (!command_submit(XHCI_CMD_ENABLE_SLOT, 0, 0, 1000U, &e)) {
        if(port_index<4U) {
            frf_r85_enable_cc[port_index]=frf_r85_cmd_last_cc;
            frf_r83_downstream_step[port_index]=0xE1u;
        }
        frf_r85_child_last_error=0xE1u;
        return 0;
    }
    if(port_index<4U)frf_r85_enable_cc[port_index]=frf_r85_cmd_last_cc;
    const uint32_t child_slot = (e.control >> 24) & 0xFFU;
    if(port_index<4U)frf_r85_slot[port_index]=child_slot;
    if (!child_slot || child_slot > XHCI_MAX_SLOTS_USED) {
        if(port_index<4U)frf_r83_downstream_step[port_index]=0xE2u;
        frf_r85_child_last_error=0xE2u;
        return 0;
    }

    /* Each final live HID child gets separate device/EP0 DMA context.
     * Preliminary descriptor scan may reuse bank0 because those slots are
     * disabled before the next child is addressed. */
    host_zero(g_child_device_context[g_native_bank],
              sizeof(g_child_device_context[g_native_bank]));
    host_zero(g_input_context, sizeof(g_input_context));
    ring_init(&g_child_ep0_ring, g_child_ep0_trbs[g_native_bank], XHCI_RING_TRBS);
    g_dcbaa[child_slot] = LE64(host_dma(g_child_device_context[g_native_bank]));
    host_cache_clean(&g_dcbaa[child_slot], sizeof(g_dcbaa[child_slot]));

    g_slot_id = child_slot;
    g_port_speed = speed;
    g_ep0_mps = ep0_initial_mps(speed);
    /* xHCI route: internal hub uses nibble0; Huasheng stage1 uses
       nibble1, stage2 uses nibble2. Never overwrite ancestors. */
    if(hub_depth>4u || hub_port>15u || !hub_port) {
        (void)command_submit(XHCI_CMD_DISABLE_SLOT,0u,
                             child_slot<<24,1000u,NULL);
        return 0;
    }
    g_route_string = hub_route | ((hub_port&15u)<<(4u*hub_depth));
    g_root_hub_port = g_selected_root_port;
    g_slot_hub = 0U;
    g_slot_num_ports = 0U;
    g_tt_hub_slot = ((speed == 1U || speed == 2U) && hub_speed == 3U) ? hub_slot : 0U;
    g_tt_port = g_tt_hub_slot ? hub_port : 0U;
    /* TTT applies to the parent HS hub slot, not the FS/LS child.
       TT Hub Slot ID and TT Port are retained above for split transactions. */
    g_tt_think_time = 0U;
    (void)tt_think;

    uint32_t *icc = input_ctx(0U);
    context_write32(icc, 1U, (1U << 0) | (1U << 1));
    fill_slot_context(1U);
    fill_ep_context(1U, &g_child_ep0_ring, 4U, g_ep0_mps, 0U);
    host_cache_clean(g_input_context, sizeof(g_input_context));
    if (!command_submit(XHCI_CMD_ADDRESS_DEVICE, host_dma(g_input_context),
                        child_slot << 24, 1500U, &e)) {
        /* Capture BEFORE Disable Slot overwrites the last command code. */
        if(port_index<4U) {
            frf_r85_address_cc[port_index]=frf_r85_cmd_last_cc;
            frf_r83_downstream_step[port_index]=0xE3u;
        }
        frf_r85_child_last_error=0xE3u;
        kprintf("[R85-ADDR] port%u speed%u hubSlot%u ttSlot%u ttPort%u childSlot%u AddressDevice FAIL cc=%u\n",
          hub_port,speed,hub_slot,g_tt_hub_slot,g_tt_port,child_slot,frf_r85_cmd_last_cc);
        (void)command_submit(XHCI_CMD_DISABLE_SLOT, 0U, child_slot << 24, 1000U, NULL);
        /* A disabled xHC slot must not retain a stale DCBAA DMA pointer. */
        g_dcbaa[child_slot]=0u;
        host_cache_clean(&g_dcbaa[child_slot],sizeof(g_dcbaa[child_slot]));
        return 0;
    }
    if(port_index<4U)frf_r85_address_cc[port_index]=frf_r85_cmd_last_cc;
    frf_r85_address_ok |= 1u << hub_port;
    /* USB 2.0 §9.2.6.3: after SET_ADDRESS the device may be unresponsive
     * for 2ms. Windows' hub stack gives >=10ms stabilisation, so we use
     * 15ms; the previous driver issued GET_DESCRIPTOR immediately.
     * The xHC issues SET_ADDRESS as part of Address Device. */
    hub_delay_ms(15u);
    frf_r86_address_recovery_ms=15u;
    /* All existing EP0 helpers use g_ep0_ring/g_slot_id.  Switch that small
     * software ring descriptor to the child ring; the root ring/TRBs remain intact. */
    g_ep0_ring = g_child_ep0_ring;
    return 1;
}

static void hub_disable_current_child(void)
{
    const uint32_t slot = g_slot_id;
    if (slot && slot != g_root_hub_slot) {
        (void)command_submit(XHCI_CMD_DISABLE_SLOT, 0U, slot << 24, 1000U, NULL);
        g_dcbaa[slot] = 0U;
        host_cache_clean(&g_dcbaa[slot], sizeof(g_dcbaa[slot]));
    }
}

static void hub_copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0U;
    if (!dst || !cap) return;
    if (src) while (src[i] && i + 1U < cap) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static void r86_note_descriptor_fail(uint32_t phase,uint32_t got)
{
    if(!frf_r86_track_port || frf_r86_track_port>4u)return;
    unsigned i=frf_r86_track_port-1u;
    frf_r86_desc_phase[i]=phase;
    frf_r86_desc_fail_cc[i]=frf_r84_hub_cc;
    frf_r86_desc_fail_reason[i]=frf_r84_hub_td_reason;
    frf_r86_desc_fail_slot[i]=frf_r86_last_evt_slot;
    frf_r86_desc_fail_ep[i]=frf_r86_last_evt_ep;
    frf_r86_desc_got[i]=got;
    kprintf("[R86-EP0] port%u descriptor phase=%02x failed cc=%u reason=%u "
            "slot=%u ep=%u got=%u request=%08x\n",
      frf_r86_track_port,phase,frf_r84_hub_cc,frf_r84_hub_td_reason,
      frf_r86_last_evt_slot,frf_r86_last_evt_ep,got,frf_r86_last_ctl_request);
}
static uint32_t g_r91_last_report_descriptor_length;
static int hub_parse_child(struct FRFXHCIHubDeviceInfo *d,
                           uint32_t *bin_ep, uint32_t *bout_ep,
                           uint32_t *bin_mps, uint32_t *bout_mps,
                           uint32_t *config_value,
                           uint32_t *hid_ep, uint32_t *hid_mps,
                           uint32_t *hid_interval, uint32_t *hid_iface)
{
    uint32_t got = 0U;
    frf_r86_track_port=d->hub_port;
    if(d->hub_port<=4u)frf_r86_desc_phase[d->hub_port-1u]=0x11u;
    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if (!get_descriptor(USB_DT_DEVICE, 0U, 0U, g_ctrl_buffer, 8U, &got) || got < 8U) {
        r86_note_descriptor_fail(0xE1u,got);
        return 0;
    }
    if(d->hub_port<=4u) {
        frf_r86_desc_phase[d->hub_port-1u]=0x12u;
        frf_r86_desc_got[d->hub_port-1u]=got;
    }
    uint32_t mps = g_ctrl_buffer[7];
    if (g_port_speed >= 4U && mps < 16U) mps = 1U << mps;
    if (!evaluate_ep0_mps(mps)) {
        r86_note_descriptor_fail(0xE2u,got);
        return 0;
    }
    if(d->hub_port<=4u)frf_r86_desc_phase[d->hub_port-1u]=0x13u;
    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    got=0u;
    if (!get_descriptor(USB_DT_DEVICE, 0U, 0U, g_ctrl_buffer, 18U, &got) || got < 18U) {
        r86_note_descriptor_fail(0xE3u,got);
        return 0;
    }
    if(d->hub_port<=4u) {
        frf_r86_desc_phase[d->hub_port-1u]=0x14u;
        frf_r86_desc_got[d->hub_port-1u]=got;
    }
    d->vid = (uint32_t)g_ctrl_buffer[8] | ((uint32_t)g_ctrl_buffer[9] << 8);
    d->pid = (uint32_t)g_ctrl_buffer[10] | ((uint32_t)g_ctrl_buffer[11] << 8);
    frf_r94_last_seen_vidpid=((d->vid&0xffffu)<<16)|(d->pid&0xffffu);
    if(d->hub_port>=1u && d->hub_port<=4u)
        frf_r94_port_vidpid[d->hub_port-1u]=frf_r94_last_seen_vidpid;
    d->device_class = g_ctrl_buffer[4];
    d->device_subclass = g_ctrl_buffer[5];
    d->device_protocol = g_ctrl_buffer[6];
    const uint8_t im = g_ctrl_buffer[14], ip = g_ctrl_buffer[15], is = g_ctrl_buffer[16];
    uint16_t lang = 0x0409U;
    char mfr[64], prod[96], serial[64];
    mfr[0]=prod[0]=serial[0]=0;
    frf_zero_bytes(g_ctrl_buffer, 16U);
    if (get_descriptor(USB_DT_STRING,0U,0U,g_ctrl_buffer,16U,&got) && got >= 4U)
        lang=(uint16_t)g_ctrl_buffer[2] | ((uint16_t)g_ctrl_buffer[3] << 8);
    usb_string_ascii(im,lang,mfr,sizeof(mfr));
    usb_string_ascii(ip,lang,prod,sizeof(prod));
    usb_string_ascii(is,lang,serial,sizeof(serial));
    hub_copy_text(d->manufacturer,sizeof(d->manufacturer),mfr);
    hub_copy_text(d->product,sizeof(d->product),prod);
    hub_copy_text(d->serial,sizeof(d->serial),serial);

    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if(d->hub_port<=4u)frf_r86_desc_phase[d->hub_port-1u]=0x21u;
    got=0u;
    if (!get_descriptor(USB_DT_CONFIG,0U,0U,g_ctrl_buffer,9U,&got) || got < 9U) {
        r86_note_descriptor_fail(0xE4u,got);
        return 0;
    }
    uint32_t total=(uint32_t)g_ctrl_buffer[2] | ((uint32_t)g_ctrl_buffer[3] << 8);
    if (total < 9U || total > sizeof(g_ctrl_buffer)) {
        r86_note_descriptor_fail(0xE5u,got);
        return 0;
    }
    *config_value = g_ctrl_buffer[5];
    frf_zero_bytes(g_ctrl_buffer, sizeof(g_ctrl_buffer));
    if(d->hub_port<=4u)frf_r86_desc_phase[d->hub_port-1u]=0x22u;
    got=0u;
    if (!get_descriptor(USB_DT_CONFIG,0U,0U,g_ctrl_buffer,(uint16_t)total,&got) || got < 9U) {
        r86_note_descriptor_fail(0xE6u,got);
        return 0;
    }
    if(d->hub_port<=4u)frf_r86_desc_phase[d->hub_port-1u]=0x23u;

    uint32_t cur_class=0U, cur_sub=0U, cur_proto=0U;
    uint32_t cur_iface=0xffffffffu, mouse_iface=0xffffffffu;
    g_r96_hid_out_ep=g_r96_hid_out_mps=g_r96_hid_out_interval=0u;
    /* R91: HID descriptor's 0x22 report length belongs to SELECTED
     * interface.  Do not request 512B blindly, especially on FS/LS hubs. */
    uint32_t selected_report_length=0u;
    *hid_ep=*hid_mps=*hid_interval=0U;
    *hid_iface=0xffffffffu;
    *bin_ep=*bout_ep=*bin_mps=*bout_mps=0U;
    for (uint32_t off=0U; off+2U<=got; ) {
        uint32_t len=g_ctrl_buffer[off], type=g_ctrl_buffer[off+1U];
        if (len<2U || off+len>got) break;
        if (type==USB_DT_INTERFACE && len>=9U) {
            cur_iface=g_ctrl_buffer[off+2U];
            cur_class=g_ctrl_buffer[off+5U]; cur_sub=g_ctrl_buffer[off+6U]; cur_proto=g_ctrl_buffer[off+7U];
            if (cur_class==USB_CLASS_HID &&
                (cur_sub==USB_HID_SUBCLASS_BOOT || cur_sub==0u) &&
                (cur_proto==USB_HID_PROTOCOL_KEYBOARD ||
                 cur_proto==USB_HID_PROTOCOL_MOUSE ||
                 cur_proto==0u) &&
                mouse_iface==0xffffffffu) {
                mouse_iface=cur_iface;
                *hid_iface=cur_iface;
                /* Real HID interface may not be the device's first
                 * interface (wireless receivers often are composite). */
                d->interface_class=USB_CLASS_HID;
                d->interface_subclass=cur_sub;
                d->interface_protocol=cur_proto;
            }
            if (!d->interface_class) {
                d->interface_class=cur_class; d->interface_subclass=cur_sub; d->interface_protocol=cur_proto;
            }
            if (cur_class==USB_CLASS_MASS_STORAGE && cur_sub==USB_MSC_SUBCLASS_SCSI && cur_proto==USB_MSC_PROTOCOL_BOT)
                d->mass_storage=1U;
            if (cur_class==USB_CLASS_HUB) d->is_hub=1U;
        } else if (type==0x21u && len>=9u &&
                   cur_iface==mouse_iface && cur_class==USB_CLASS_HID) {
            /* HID descriptor: [bNumDescriptors]=5, triples begin at 6:
             * descriptor type (0x22), wDescriptorLength LE16. */
            uint32_t ndesc=g_ctrl_buffer[off+5u];
            for(uint32_t i=0u;i<ndesc && 6u+3u*i+2u<len;i++) {
                uint32_t idx=off+6u+3u*i;
                if(g_ctrl_buffer[idx]==0x22u) {
                    selected_report_length=(uint32_t)g_ctrl_buffer[idx+1u]|
                                           ((uint32_t)g_ctrl_buffer[idx+2u]<<8);
                    break;
                }
            }
        } else if (type==USB_DT_ENDPOINT && len>=7U &&
                   cur_iface==mouse_iface && cur_class==USB_CLASS_HID) {
            uint32_t addr=g_ctrl_buffer[off+2U],attr=g_ctrl_buffer[off+3U];
            uint32_t ep=addr&0x0fu;
            uint32_t mps=((uint32_t)g_ctrl_buffer[off+4U] |
                         ((uint32_t)g_ctrl_buffer[off+5U]<<8)) & 0x7ffu;
            if ((attr&3u)==3u && ep && mps) {
                if(addr&0x80u) {
                    *hid_ep=ep;*hid_mps=mps;*hid_interval=g_ctrl_buffer[off+6U];
                } else {
                    g_r96_hid_out_ep=ep;g_r96_hid_out_mps=mps;
                    g_r96_hid_out_interval=g_ctrl_buffer[off+6U];
                }
            }
        } else if (type==USB_DT_ENDPOINT && len>=7U && d->mass_storage &&
                   cur_class==USB_CLASS_MASS_STORAGE && cur_sub==USB_MSC_SUBCLASS_SCSI && cur_proto==USB_MSC_PROTOCOL_BOT) {
            uint32_t addr=g_ctrl_buffer[off+2U], attr=g_ctrl_buffer[off+3U];
            uint32_t ep=addr & 0x0FU;
            uint32_t epm=(uint32_t)g_ctrl_buffer[off+4U] | ((uint32_t)g_ctrl_buffer[off+5U] << 8);
            if ((attr & 3U)==2U && ep) {
                if (addr & 0x80U) { *bin_ep=ep; *bin_mps=epm; }
                else { *bout_ep=ep; *bout_mps=epm; }
            }
        }
        off += len;
    }
    if (d->device_class==USB_CLASS_HUB) d->is_hub=1U;
    g_r91_last_report_descriptor_length=selected_report_length;
    if(d->hub_port>=1u && d->hub_port<=4u)
        frf_r94_port_classproto[d->hub_port-1u]=
            ((d->interface_class&0xffu)<<24)|
            ((d->interface_subclass&0xffu)<<16)|
            ((d->interface_protocol&0xffu)<<8)|
            (d->device_class&0xffu);
    return 1;
}

/* R97 Huasheng 214b:7260: keep individual hub slots/rings live and
 * enumerate up to TWO external high-speed hubs chained after the Pi4
 * internal VL805 USB2 hub. No USB peripheral relies on a Linux driver.
 * The root hub is depth0; external #1 is depth1, external #2 depth2.
 * Direct child route=hub route|(port<<(4*depth)). */
static void r97_select_hub(struct R97HubState *h)
{
    g_slot_id=h->slot;
    g_port_speed=h->speed;
    g_ep0_mps=h->ep0_mps;
    g_ep0_ring=h->ep0;
    g_route_string=h->route;
    g_root_hub_port=h->root_port;
    g_slot_hub=1u;
    g_slot_num_ports=h->ports;
    g_tt_hub_slot=g_tt_port=0u;
    g_tt_think_time=h->speed==3u?h->tt_think:0u;
}
static int r97_configure_hub(struct R97HubState *h,uint32_t cfg,
                             uint32_t vid,uint32_t pid,
                             uint32_t route,uint32_t depth)
{
    if(g_r97_hub_used>=R97_MAX_EXTERNAL_HUBS || depth>2u ||
       g_r97_next_bank>=R97_CHILD_CONTEXT_BANKS)
        return 0;
    if(!frf_xhci_host_control(0x00u,USB_REQ_SET_CONFIGURATION,
                         (uint16_t)cfg,0u,NULL,0u,0,NULL))
        return 0;
    uint32_t got=0u;
    frf_zero_bytes(g_ctrl_buffer,sizeof(g_ctrl_buffer));
    if(!frf_xhci_host_control(0xA0u,USB_REQ_GET_DESCRIPTOR,
           (uint16_t)(USB_DT_HUB<<8),0u,g_ctrl_buffer,9u,1,&got)
           ||got<7u) return 0;
    uint32_t ports=g_ctrl_buffer[2];
    if(!ports || ports>15u)return 0;
    uint32_t tt=(((uint32_t)g_ctrl_buffer[3]|
                  ((uint32_t)g_ctrl_buffer[4]<<8))>>5)&3u;
    uint32_t power_ms=(uint32_t)g_ctrl_buffer[5]*2u+20u;
    if(power_ms<200u)power_ms=200u;
    /* Keep the child hub's existing EP0; configure its slot to mark
       HUB=1, number of ports and TT Think Time. New hub slot context
       retains the full route and physical root-port from Address Device. */
    g_slot_hub=1u;
    g_slot_num_ports=ports;
    g_tt_hub_slot=g_tt_port=0u;
    g_tt_think_time=g_port_speed==3u?tt:0u;
    host_zero(g_input_context,sizeof(g_input_context));
    uint32_t *icc=input_ctx(0u);
    context_write32(icc,1u,1u<<0);
    fill_slot_context(1u);
    host_cache_clean(g_input_context,sizeof(g_input_context));
    if(!command_submit(XHCI_CMD_CONFIG_EP,host_dma(g_input_context),
                       g_slot_id<<24,1500u,NULL)) return 0;
    h->slot=g_slot_id;h->speed=g_port_speed;h->root_port=g_root_hub_port;
    h->route=route;h->depth=depth;h->ports=ports;h->tt_think=tt;
    h->power_ms=power_ms;h->vidpid=(vid<<16)|(pid&0xffffu);
    h->bank=g_native_bank;
    h->ep0=g_ep0_ring;h->ep0_mps=g_ep0_mps;h->live=1u;
    ++g_r97_hub_used;
    ++g_r97_next_bank; /* Hubs keep their DMA context permanently. */
    frf_r97_hub_count=g_r97_hub_used;
    frf_r97_ext_hub_slot=h->slot;
    frf_r97_ext_hub_ports=ports;
    frf_r97_ext_hub_vidpid=h->vidpid;
    frf_r97_ext_level=depth;
    frf_r97_ext_tt_slot=h->slot;
    frf_r97_ext_stage=3u;
    kprintf("[R97-HUB] LIVE depth%u route=%05x slot%u ports%u TTT%u VID:PID=%04x:%04x\n",
            depth,route,h->slot,ports,tt,vid,pid);
    return 1;
}
static int r97_scan_hub(struct R97HubState *hub)
{
    if(!hub || !hub->live || hub->depth>2u)return 0;
    r97_select_hub(hub);
    frf_r97_ext_stage=4u;
    for(uint32_t port=1u;port<=hub->ports;port++) {
        if(!hub_port_feature(port,USB_HUB_FEAT_PORT_POWER,1)) {
            frf_r97_ext_error=0xe1u;
            kprintf("[R97-HUB] depth%u port%u power failed\n",hub->depth,port);
        }
        hub->ep0=g_ep0_ring;
    }
    hub_delay_ms(hub->power_ms);
    for(uint32_t port=1u;port<=hub->ports;port++) {
        if(g_native_count>=FRF_NATIVE_HID_MAX ||
           g_r97_next_bank>=R97_CHILD_CONTEXT_BANKS ||
           g_hub_device_count>=FRFXHCI_HUB_MAX_DEVICES) break;
        frf_r97_ext_last_port=port;
        r97_select_hub(hub);
        uint16_t st=0u,ch=0u;
        int okay=hub_port_status(port,&st,&ch);
        hub->ep0=g_ep0_ring;
        if(!okay) {
            frf_r97_ext_error=0xe2u;
            continue;
        }
        if(!(st&USB_HUB_PORT_CONNECTION))continue;
        frf_r97_ext_connected_mask|=1u<<port;
        ++frf_r97_ext_child_count;
        frf_r97_ext_stage=5u;
        g_native_bank=g_r97_next_bank;
        uint32_t speed=0u;
        int addressed=0;
        for(uint32_t tries=0u;tries<3u;tries++) {
            if(tries)hub_delay_ms(tries==1u?140u:300u);
            r97_select_hub(hub);
            if(!hub_reset_downstream_port(port,&speed)) {
                hub->ep0=g_ep0_ring;
                frf_r97_ext_error=0xe3u;
                continue;
            }
            hub->ep0=g_ep0_ring;
            if(!address_child_device(port,speed,hub->slot,hub->speed,
                         hub->tt_think,hub->route,hub->depth)) {
                frf_r97_ext_last_cc=frf_r85_cmd_last_cc;
                frf_r97_ext_error=0xe4u;
                continue;
            }
            addressed=1;
            break;
        }
        if(!addressed)continue;
        struct FRFXHCIHubDeviceInfo *d=&g_hub_devices[g_hub_device_count];
        frf_zero_bytes(d,sizeof(*d));
        d->valid=1u;
        d->hub_port=port;
        d->slot_id=g_slot_id;
        d->speed_id=speed;
        uint32_t ie=0u,oe=0u,im=0u,om=0u,cfg=0u;
        uint32_t he=0u,hm=0u,hi=0u,hiface=0xffffffffu;
        if(!hub_parse_child(d,&ie,&oe,&im,&om,&cfg,
                             &he,&hm,&hi,&hiface)) {
            frf_r97_ext_error=0xe5u;
            hub_disable_current_child();
            ++g_hub_device_count;
            continue;
        }
        ++g_hub_device_count;
        const uint32_t child_route=hub->route|
                                      ((port&15u)<<(4u*hub->depth));
        kprintf("[R97-HUB] depth%u port%u route=%05x slot%u speed%u VID:PID=%04x:%04x class%u/%u/%u\n",
           hub->depth,port,child_route,d->slot_id,speed,d->vid,d->pid,
           d->interface_class,d->interface_subclass,d->interface_protocol);
        if(d->is_hub) {
            if(hub->depth>=2u || g_r97_hub_used>=R97_MAX_EXTERNAL_HUBS) {
                ++frf_r97_ext_unsupported;
                hub_disable_current_child();
                continue;
            }
            struct R97HubState *next=&g_r97_external[g_r97_hub_used];
            if(!r97_configure_hub(next,cfg,d->vid,d->pid,
                                child_route,hub->depth+1u)) {
                frf_r97_ext_error=0xe6u;
                hub_disable_current_child();
                continue;
            }
            (void)r97_scan_hub(next);
            /* Re-activate this hub's EP0 before proceeding to next port. */
            r97_select_hub(hub);
            continue;
        }
        int usable=he && hm && hiface!=0xffffffffu &&
                    d->interface_class==USB_CLASS_HID &&
                    (d->interface_protocol==USB_HID_PROTOCOL_KEYBOARD ||
                     d->interface_protocol==USB_HID_PROTOCOL_MOUSE ||
                     d->interface_protocol==0u);
        if(usable && g_native_count<FRF_NATIVE_HID_MAX) {
            if(configure_interrupt_in(he,hm,hi) &&
               frf_xhci_host_control(0x00u,USB_REQ_SET_CONFIGURATION,
                                  (uint16_t)cfg,0u,NULL,0u,0,NULL)) {
                if(d->interface_protocol==USB_HID_PROTOCOL_KEYBOARD ||
                   d->interface_protocol==USB_HID_PROTOCOL_MOUSE)
                    (void)frf_xhci_host_control(0x21u,USB_REQ_SET_PROTOCOL,
                                      0u,(uint16_t)hiface,NULL,0u,0,NULL);
                struct NativeHID *h=&g_native_hid[g_native_count];
                h->slot=g_slot_id;
                h->protocol=d->interface_protocol;
                h->interface=hiface;
                h->ep=he;h->dci=g_hid_in_dci;h->mps=hm;
                h->vid=d->vid;h->pid=d->pid;
                h->report_desc_length=g_r91_last_report_descriptor_length;
                h->out_ep=g_r96_hid_out_ep;
                h->out_dci=g_hid_out_dci;
                h->out_mps=g_r96_hid_out_mps;
                if(g_hid_out_dci)h->out_ring=g_hid_out_ring;
                h->ring=g_hid_in_ring;h->ep0=g_ep0_ring;
                h->ring_bank=g_native_bank;h->ring_epoch=0u;
                h->pending=0u;h->pending_length=0u;
                h->failures=0u;h->valid=1u;
                ++g_native_count;
                ++g_r97_next_bank;
                frf_r83_hid_count=g_native_count;
                frf_r89_native_count=g_native_count;
                frf_r97_ext_live_mask|=1u<<port;
                frf_r97_ext_stage=7u;
                kprintf("[R97-HUB] live HID%u depth%u port%u slot%u protocol%u\n",
                    g_native_count,hub->depth,port,h->slot,h->protocol);
                continue;
            }
            frf_r97_ext_error=0xe7u;
        } else {
            ++frf_r97_ext_unsupported;
        }
        hub_disable_current_child();
        r97_select_hub(hub);
    }
    r97_select_hub(hub);
    if(!frf_r97_ext_error)frf_r97_ext_stage=8u;
    return 1;
}

/* Every USB2.0 socket on Pi4B is behind the VL805 internal 4-port hub.
 * Unlike the root hub, downstream status needs a class GET_STATUS control TD. */
volatile uint32_t frf_r57_hub_ports,frf_r57_hub_connected_mask;
volatile uint32_t frf_r57_hub_st_ok,frf_r57_hub_st_fail;
static int hub_scan_children(void)
{
    frf_r99_root_monitor_port=0u;
    g_r99_root_valid=0u;
    frf_r99_root_status=frf_r99_root_change=0u;
    frf_r99_root_poll_ok=frf_r99_root_poll_fail=0u;
    frf_r99_root_link_lost=frf_r99_root_link_up=0u;
    frf_r99_live_link=0u;
    frf_r99_last_hid_error_cc=frf_r99_hid_error_count=0u;
    frf_r99_ring_swaps=frf_r99_stale_events=0u;
    frf_r99_hid_recovered_total=frf_r99_hid_last_age_seconds=0u;
    frf_r99_hid_active_pending=0u;
    frf_r84_hub_power_ok=frf_r84_hub_power_fail=0u;
    frf_r84_hub_status_ok=frf_r84_hub_status_fail=0u;
    frf_r84_hub_cc=frf_r84_hub_evt_kind=frf_r84_hub_td_reason=0u;
    for(uint32_t i=0u;i<4u;i++)frf_r84_hub_last_status[i]=frf_r84_hub_last_change[i]=0u;
    for(uint32_t i=0u;i<4u;i++)frf_r84_snapshot[i]=0u;
    frf_r84_hub_scan_step=1u;
    frf_r86_track_port=frf_r86_address_recovery_ms=0u;
    frf_r86_last_evt_slot=frf_r86_last_evt_ep=0u;
    frf_r86_last_ctl_request=frf_r86_last_ctl_length=0u;
    for(unsigned i=0;i<4u;i++){
        frf_r86_desc_phase[i]=frf_r86_desc_fail_cc[i]=0u;
        frf_r86_desc_fail_reason[i]=frf_r86_desc_got[i]=0u;
        frf_r86_desc_fail_slot[i]=frf_r86_desc_fail_ep[i]=0u;
    }
    frf_r85_child_last_error=frf_r85_address_ok=0u;
    frf_r85_cmd_last_type=frf_r85_cmd_last_cc=0u;
    for(unsigned i=0;i<4u;i++) {
        frf_r85_enable_cc[i]=frf_r85_address_cc[i]=0u;
        frf_r85_speed[i]=frf_r85_slot[i]=0u;
    }
    for(uint32_t i=0u;i<8u;i++) {
        frf_r83_downstream_status[i]=0u;
        frf_r83_downstream_step[i]=0u;
    }
    frf_r57_hub_ports=0u;frf_r57_hub_connected_mask=0u;
    frf_r57_hub_st_ok=0u;frf_r57_hub_st_fail=0u;
    frf_r89_native_seen_mask=frf_r89_native_live_mask=0u;
    frf_r89_native_fail_mask=frf_r89_native_candidate_mask=0u;
    frf_r89_native_count=0u;
    frf_r94_usb_connected_mask=frf_r94_usb_live_mask=0u;
    frf_r94_usb_pending_mask=frf_r94_hotplug_stage=0u;
    frf_r94_last_seen_vidpid=frf_r94_last_unhandled_vidpid=0u;
    frf_r94_last_unhandled_classproto=0u;
    frf_r94_hub_poll_errors=0u;
    frf_r94_hotplug_connects=frf_r94_hotplug_disconnects=frf_r94_hotplug_failures=0u;
    frf_r95_addr_rescued_mask=frf_r95_addr_failed_mask=0u;
    g_r97_next_bank=g_r97_hub_used=0u;
    frf_r98_root_power_cycles=frf_r98_root_hs_recovery_ms=0u;
    for(unsigned z=0u;z<4u;z++) {
        frf_r98_root_addr_cc[z]=frf_r98_root_enable_cc[z]=0u;
        frf_r98_root_speed[z]=frf_r98_root_slot[z]=0u;
        frf_r98_root_attempts[z]=frf_r98_root_stage[z]=0u;
        frf_r98_root_last_status[z]=0u;
    }
    frf_zero_bytes(g_r97_external,sizeof(g_r97_external));
    frf_r97_ext_hub_slot=frf_r97_ext_hub_ports=0u;
    frf_r97_ext_connected_mask=frf_r97_ext_live_mask=0u;
    frf_r97_ext_error=frf_r97_ext_stage=0u;
    frf_r97_ext_child_count=frf_r97_ext_tt_slot=0u;
    frf_r97_ext_last_cc=frf_r97_ext_last_port=0u;
    frf_r97_ext_hub_vidpid=frf_r97_hub_count=0u;
    frf_r97_ext_level=frf_r97_ext_unsupported=0u;
    for(unsigned k=0u;k<4u;k++) {
        frf_r94_port_vidpid[k]=frf_r94_port_classproto[k]=0u;
        frf_r95_addr_attempts[k]=frf_r95_addr_cc[k]=0u;
        frf_r95_enable_cc[k]=frf_r95_reset_ok[k]=0u;
        frf_r95_port_speed[k]=frf_r95_last_slot[k]=0u;
    }
    for(unsigned i=0u;i<4u;i++) {
        frf_r89_native_protocol[i]=0xffu;
        frf_r89_native_slot[i]=0u;
        frf_r90_port_reset_result[i]=0u;
        frf_r90_port_address_cc[i]=0u;
        frf_r90_port_enable_cc[i]=0u;
        frf_r90_port_speed[i]=0u;
        frf_r90_sequence_step[i]=0u;
    }
    /* Root identity has already been parsed by the proven V0.4 path. */
    if (g_host.device_class != USB_CLASS_HUB && g_host.interface_class != USB_CLASS_HUB)
        return 1;

    const struct FRFXHCIHostInfo root_info = g_host;
    g_root_hub_slot = g_slot_id;
    g_root_hub_speed = g_port_speed;
    struct XHCIRing root_ep0_ring = g_ep0_ring;
    const uint32_t root_ep0_mps = g_ep0_mps;

    if (g_host.configuration_value &&
        !frf_xhci_host_control(0x00U, USB_REQ_SET_CONFIGURATION,
                               (uint16_t)g_host.configuration_value, 0U, NULL, 0U, 0, NULL))
        return 0;

    uint32_t got=0U;
    frf_zero_bytes(g_ctrl_buffer,sizeof(g_ctrl_buffer));
    if (!frf_xhci_host_control(0xA0U, USB_REQ_GET_DESCRIPTOR,
                               (uint16_t)(USB_DT_HUB << 8), 0U,
                               g_ctrl_buffer, 9U, 1, &got) || got < 7U)
        return 0;
    g_hub_num_ports = g_ctrl_buffer[2];
    frf_r57_hub_ports = g_hub_num_ports;
    if (!g_hub_num_ports) return 1;
    if (g_hub_num_ports > 15U) g_hub_num_ports=15U;
    const uint16_t hub_chars=(uint16_t)g_ctrl_buffer[3] | ((uint16_t)g_ctrl_buffer[4] << 8);
    g_hub_tt_think_time=(hub_chars >> 5) & 3U;
    kprintf("[FRFXHCI-HOST] HUB descriptor ports=%u chars=%04x ttt=%u\n",
            g_hub_num_ports, hub_chars, g_hub_tt_think_time);
    if (!hub_mark_root_slot(g_hub_num_ports)) {
        frf_r84_hub_scan_step=0xe2u;
        return 0;
    }
    frf_r84_hub_scan_step=2u;

    /* Save bPwrOn2PwrGood before subsequent class requests share g_ctrl_buffer. */
    uint32_t pwr_ms=(uint32_t)g_ctrl_buffer[5]*2U + 20U;
    /* Power all downstream ports while the root hub EP0 is still active. */
    for (uint32_t port=1U; port<=g_hub_num_ports; ++port) {
        if(hub_port_feature(port,USB_HUB_FEAT_PORT_POWER,1))
            frf_r84_hub_power_ok|=(1u<<port);
        else
            frf_r84_hub_power_fail|=(1u<<port);
    }
    frf_r84_hub_scan_step=3u;
    if (pwr_ms < 200U) pwr_ms=200U; /* bounded cold-attach USB2 power settle */
    hub_delay_ms(pwr_ms);

    uint32_t port_speed[16];
    uint32_t port_connected[16];
    uint32_t port_status_failed[16];
    frf_zero_bytes(port_speed,sizeof(port_speed));
    frf_zero_bytes(port_connected,sizeof(port_connected));
    frf_zero_bytes(port_status_failed,sizeof(port_status_failed));
    for (uint32_t port=1U; port<=g_hub_num_ports; ++port) {
        uint16_t st=0U,ch=0U;
        int status_ok=hub_port_status(port,&st,&ch);
        /* A cold Pi4 hub may report before the port power/line settling.
           Retry the control query without resetting the controller or EP0. */
        for (unsigned attempt=0u;!status_ok && attempt<2u;attempt++) {
            hub_delay_ms(40u);
            status_ok=hub_port_status(port,&st,&ch);
        }
        if (status_ok) {
            ++frf_r57_hub_st_ok;
            if (st & USB_HUB_PORT_CONNECTION) {
                frf_r57_hub_connected_mask |= 1u << port;
                frf_r89_native_seen_mask |= (1u << port);
            }
            port_connected[port]=(st & USB_HUB_PORT_CONNECTION) ? 1U : 0U;
            if(port<=4u)frf_r98_root_last_status[port-1u]=st;
            if(port<=8u) {
                frf_r83_downstream_status[port-1u]=st|((uint32_t)ch<<16);
                frf_r83_downstream_step[port-1u]=1u;
            }
            kprintf("[FRFXHCI-HOST] HUB port=%u status=%04x change=%04x connected=%u\n",
                    port, st, ch, port_connected[port]);
            /* R90: DO NOT RESET connected ports here.  Resetting all
             * attached ports before Address Device leaves multiple newly
             * reset devices responding to USB address 0.  Perform the reset
             * per port, immediately before that port's Address Device
             * command, below (Linux hub_port_init uses address0_mutex). */
            if(port_connected[port]) {
                if(port<=4u)frf_r90_sequence_step[port-1u]=1u;
                kprintf("[R90-USB] HUB port%u connected; defer reset until Address Device\n",port);
            }
        } else {
            ++frf_r57_hub_st_fail;
            port_status_failed[port]=1U;
            kprintf("[FRFXHCI-HOST] HUB port=%u GET_STATUS FAILED\n", port);
        }
    }
    /* Snapshot now contains pre-reset connected/status state only.
       The child loop below serializes reset with address assignment. */
    /* Freeze coherent post-power state before reusing DMA/rings on children. */
    frf_r84_hub_scan_step=4u;
    frf_r84_snapshot[0]=0x84000000u | ((g_hub_num_ports&0xffu)<<16) |
                          (frf_r57_hub_connected_mask&0xffffu);
    frf_r84_snapshot[1]=((frf_r84_hub_power_ok&0xffu)<<24)|
                          ((frf_r84_hub_power_fail&0xffu)<<16)|
                          ((frf_r84_hub_status_ok&0xffu)<<8)|
                          (frf_r84_hub_status_fail&0xffu);
    frf_r84_snapshot[2]=((frf_r57_hub_st_ok&0xffffu)<<16)|
                          (frf_r57_hub_st_fail&0xffffu);
    frf_r84_snapshot[3]=((frf_r84_hub_last_status[0]&0xffffu)<<16)|
                          (frf_r84_hub_last_status[1]&0xffffu);
    kprintf("[R84-HUB] summary ports=%u mask=%04x powerOK=%04x powerBad=%04x statusOK=%u statusBad=%u lastCC=%u reason=%u step=%u\n",
      g_hub_num_ports,frf_r57_hub_connected_mask,
      frf_r84_hub_power_ok,frf_r84_hub_power_fail,
      frf_r84_hub_status_ok,frf_r84_hub_status_fail,
      frf_r84_hub_cc,frf_r84_hub_td_reason,frf_r84_hub_scan_step);
    /* Save the root EP0 software dequeue/enqueue state only after all class
     * controls above have completed. */
    root_ep0_ring = g_ep0_ring;

    hub_zero_devices();
    g_hid_ready=0u;
    frf_usb2_hub_mouse_port=0u;
    frf_usb2_hub_mouse_ep=0u;
    frf_usb2_hub_mouse_slot=0u;
    frf_usb2_hub_mouse_status=0u;
    uint32_t boot_port=0U;
    /* R89: ONE enumeration pass.  R80-R88 addressed/disconnected every
     * downstream child in discovery, then RESET/ADDRESSed it again to make
     * it live.  That second pass perturbs other connected FS/LS devices
     * and reuses the same xHCI slot/context DMA space on failed probes.
     *
     * Here we address and parse each port exactly once.  A successful HID
     * slot and its unique DMA bank stay alive while subsequent ports are
     * enumerated.  Unwanted devices are disabled as before.
     *
     * IMPORTANT: g_native_bank is assigned BEFORE address_child_device
     * so each successful HID gets its own EP0 ring, device context,
     * interrupt-IN ring and report buffer, without a second reset.
     */
    for(uint32_t port=1U;port<=g_hub_num_ports &&
               g_hub_device_count<FRFXHCI_HUB_MAX_DEVICES; ++port) {
        if (!port_connected[port] && !port_status_failed[port])continue;
        struct FRFXHCIHubDeviceInfo *d=&g_hub_devices[g_hub_device_count];
        frf_zero_bytes(d,sizeof(*d));
        d->valid=1u;
        d->hub_port=port;
        d->speed_id=port_speed[port];
        if(port_status_failed[port]) {
            hub_copy_text(d->product,sizeof(d->product),"PORT STATUS FAILED");
            if(port<=4u)frf_r89_native_fail_mask|=1u<<port;
            ++g_hub_device_count;
            continue;
        }
        if(g_native_count>=FRF_NATIVE_HID_MAX ||
           g_r97_next_bank>=R97_CHILD_CONTEXT_BANKS) {
            hub_copy_text(d->product,sizeof(d->product),"NATIVE USB SLOTS FULL");
            ++g_hub_device_count;
            continue;
        }
        g_native_bank=g_r97_next_bank;
        /* Preserve the working R90 serial hub-port reset/address path.
         * R95 retries ONLY this one physical port if it fails, with a
         * bounded recovery pause and freshly initialised child slot.
         * The keyboard or mouse already registered on another port
         * is not reset or re-addressed. Never attempt simultaneous
         * downstream resets (USB address zero must be serialized). */
        int port_reset_ok=0,port_address_ok=0;
        for(unsigned attempt=0u;attempt<5u;attempt++) {
            if(attempt) {
                static const uint32_t rest_ms[5]={0u,250u,650u,950u,1200u};
                hub_delay_ms(rest_ms[attempt]);
            }
            /* The preceding attempt may leave g_slot_id on the failed
             * child. Hub class requests MUST use the root hub EP0. */
            g_slot_id=g_root_hub_slot;
            g_port_speed=g_root_hub_speed;
            g_ep0_mps=root_ep0_mps;
            g_ep0_ring=root_ep0_ring;
            g_route_string=0u;
            g_root_hub_port=g_selected_root_port;
            g_slot_hub=1u;g_slot_num_ports=g_hub_num_ports;
            g_tt_hub_slot=g_tt_port=g_tt_think_time=0u;
            if(port<=4u) {
                frf_r90_sequence_step[port-1u]=2u;
                frf_r95_addr_attempts[port-1u]=attempt+1u;
                frf_r98_root_attempts[port-1u]=attempt+1u;
                frf_r98_root_stage[port-1u]=2u;
            }
            /* Once only: power cycle the failing high-speed port, not
               any other live keyboard/mouse/gamepad port. */
            if(attempt==2u && port<=4u &&
               frf_r98_root_speed[port-1u]==3u) {
                if(hub_port_feature(port,USB_HUB_FEAT_PORT_POWER,0)) {
                    hub_delay_ms(200u);
                    if(hub_port_feature(port,USB_HUB_FEAT_PORT_POWER,1)) {
                        ++frf_r98_root_power_cycles;
                        hub_delay_ms(700u);
                    }
                    root_ep0_ring=g_ep0_ring;
                }
            }
            frf_r98_root_hs_recovery_ms=(attempt>0u)?120u:40u;
            if(!hub_reset_downstream_port(port,&port_speed[port])) {
                root_ep0_ring=g_ep0_ring;
                if(port<=4u) {
                    frf_r90_port_reset_result[port-1u]=0xe1u;
                    frf_r98_root_stage[port-1u]=0xe1u;
                }
                kprintf("[R98-USB] hub port%u reset fail attempt%u\n",
                        port,attempt+1u);
                continue;
            }
            port_reset_ok=1;
            root_ep0_ring=g_ep0_ring;
            if(port<=8u)frf_r83_downstream_step[port-1u]=2u;
            if(port<=4u) {
                frf_r90_port_reset_result[port-1u]=1u;
                frf_r90_port_speed[port-1u]=port_speed[port];
                frf_r90_sequence_step[port-1u]=3u;
                frf_r95_reset_ok[port-1u]=1u;
                frf_r95_port_speed[port-1u]=port_speed[port];
                frf_r98_root_speed[port-1u]=port_speed[port];
                frf_r98_root_stage[port-1u]=3u;
            }
            frf_r98_root_hs_recovery_ms=0u;
            if(!address_child_device(port,port_speed[port],
                g_root_hub_slot,g_root_hub_speed,g_hub_tt_think_time,0u,0u)) {
                if(port<=4u) {
                    frf_r95_addr_cc[port-1u]=frf_r85_address_cc[port-1u];
                    frf_r95_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
                    frf_r95_last_slot[port-1u]=frf_r85_slot[port-1u];
                    frf_r90_port_address_cc[port-1u]=frf_r85_address_cc[port-1u];
                    frf_r90_port_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
                    frf_r90_sequence_step[port-1u]=0xe3u;
                    frf_r98_root_addr_cc[port-1u]=frf_r85_address_cc[port-1u];
                    frf_r98_root_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
                    frf_r98_root_slot[port-1u]=frf_r85_slot[port-1u];
                    frf_r98_root_stage[port-1u]=0xe3u;
                }
                kprintf("[R98-USB] hub port%u Address Device fail attempt%u "
                        "speed%u enableCC%u addressCC%u slot%u\n",
                        port,attempt+1u,port_speed[port],
                        frf_r85_enable_cc[port<=4u?port-1u:0u],
                        frf_r85_address_cc[port<=4u?port-1u:0u],
                        frf_r85_slot[port<=4u?port-1u:0u]);
                continue;
            }
            port_address_ok=1;
            if(port<=4u) {
                frf_r95_addr_cc[port-1u]=frf_r85_address_cc[port-1u];
                frf_r95_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
                frf_r95_last_slot[port-1u]=frf_r85_slot[port-1u];
                frf_r98_root_addr_cc[port-1u]=frf_r85_address_cc[port-1u];
                frf_r98_root_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
                frf_r98_root_slot[port-1u]=frf_r85_slot[port-1u];
                frf_r98_root_stage[port-1u]=4u;
                if(attempt>0u)frf_r95_addr_rescued_mask|=1u<<port;
            }
            break;
        }
        frf_r98_root_hs_recovery_ms=0u;
        if(!port_reset_ok || !port_address_ok) {
            hub_copy_text(d->product,sizeof(d->product),
                port_reset_ok?"CONNECTED - ADDRESS FAILED":
                              "CONNECTED - RESET FAILED");
            if(port<=4u) {
                frf_r89_native_fail_mask|=1u<<port;
                frf_r95_addr_failed_mask|=1u<<port;
                frf_r90_sequence_step[port-1u]=port_reset_ok?0xe3u:0xe1u;
                frf_r83_downstream_step[port-1u]=port_reset_ok?0xe3u:0xe2u;
            }
            ++g_hub_device_count;
            continue;
        }
        d->slot_id=g_slot_id;
        if(port<=4u) {
            frf_r90_port_enable_cc[port-1u]=frf_r85_enable_cc[port-1u];
            frf_r90_port_address_cc[port-1u]=frf_r85_address_cc[port-1u];
            frf_r90_sequence_step[port-1u]=4u;
        }
        if(port<=8u)frf_r83_downstream_step[port-1u]=3u;
        uint32_t ie=0u,oe=0u,im=0u,om=0u,cfg=0u;
        uint32_t he=0u,hm=0u,hi=0u,hiface=0xffffffffu;
        g_r91_last_report_descriptor_length=0u;
        if(!hub_parse_child(d,&ie,&oe,&im,&om,&cfg,
                            &he,&hm,&hi,&hiface)) {
            hub_copy_text(d->product,sizeof(d->product),"CONNECTED - DESCRIPTOR FAILED");
            if(port<=4u) {
                frf_r90_sequence_step[port-1u]=0xe4u;
                frf_r89_native_fail_mask|=1u<<port;
            }
            if(port<=8u)frf_r83_downstream_step[port-1u]=0xe4u;
            hub_disable_current_child();
            ++g_hub_device_count;
            continue;
        }
        kprintf("[R89-USB] scanned port%u slot%u speed%u vid=%04x pid=%04x protocol=%u HID-ep=%u\n",
            port,d->slot_id,d->speed_id,d->vid,d->pid,
            d->interface_protocol,he);
        if(!boot_port && d->mass_storage && ie && oe && im && om)
            boot_port=port;
        if(port<=4u) {
            frf_r89_native_protocol[port-1u]=d->interface_protocol;
            frf_r89_native_slot[port-1u]=g_slot_id;
        }
        if(d->is_hub) {
            /* Monitor this physical INTERNAL hub port after initial
               registration; port power/link loss is distinct from a
               hung child HID endpoint. */
            frf_r99_root_monitor_port=port;
            /* The Huasheng 214b:7260 is physically two chained USB2 hubs.
             * Keep this first external slot addressed; enumerate its
             * children and the second hub recursively before resuming
             * root VL805 port discovery. */
            struct R97HubState *next=&g_r97_external[g_r97_hub_used];
            const uint32_t route=port&15u;
            if(g_r97_hub_used>=R97_MAX_EXTERNAL_HUBS ||
               !r97_configure_hub(next,cfg,d->vid,d->pid,route,1u)) {
                frf_r97_ext_error=0xe8u;
                hub_disable_current_child();
            } else {
                ++g_hub_device_count;
                (void)r97_scan_hub(next);
                g_slot_id=g_root_hub_slot;
                g_port_speed=g_root_hub_speed;
                g_ep0_mps=root_ep0_mps;
                g_ep0_ring=root_ep0_ring;
                g_route_string=0u;
                g_root_hub_port=g_selected_root_port;
                g_slot_hub=1u;g_slot_num_ports=g_hub_num_ports;
                g_tt_hub_slot=g_tt_port=g_tt_think_time=0u;
                continue;
            }
            ++g_hub_device_count;
            continue;
        }
        int usable_hid=he && hm && hiface!=0xffffffffu &&
            d->interface_class==USB_CLASS_HID &&
            (d->interface_protocol==USB_HID_PROTOCOL_KEYBOARD ||
             d->interface_protocol==USB_HID_PROTOCOL_MOUSE ||
             d->interface_protocol==0u);
        if(!usable_hid) {
            frf_r94_last_unhandled_vidpid=
                ((d->vid&0xffffu)<<16)|(d->pid&0xffffu);
            frf_r94_last_unhandled_classproto=
                ((d->interface_class&0xffu)<<24)|
                ((d->interface_subclass&0xffu)<<16)|
                ((d->interface_protocol&0xffu)<<8)|
                 (d->device_class&0xffu);
            kprintf("[R94-USB] port%u UNHANDLED VID:PID=%04x:%04x CLASS=%02x/%02x/%02x ep%u\n",
                    port,d->vid,d->pid,d->interface_class,
                    d->interface_subclass,d->interface_protocol,he);
        }
        if(usable_hid) {
            frf_r89_native_candidate_mask|=1u<<port;
            if(port<=8u)frf_r83_downstream_step[port-1u]=4u;
            if(configure_interrupt_in(he,hm,hi) &&
               frf_xhci_host_control(0x00u,USB_REQ_SET_CONFIGURATION,
                                   (uint16_t)cfg,0u,NULL,0u,0,NULL)) {
                if(d->interface_protocol==USB_HID_PROTOCOL_KEYBOARD ||
                   d->interface_protocol==USB_HID_PROTOCOL_MOUSE) {
                    (void)frf_xhci_host_control(0x21u,USB_REQ_SET_PROTOCOL,0u,
                                          (uint16_t)hiface,NULL,0u,0,NULL);
                }
                struct NativeHID *h=&g_native_hid[g_native_count];
                h->slot=g_slot_id;
                h->protocol=d->interface_protocol;
                h->interface=hiface;
                h->ep=he;h->dci=g_hid_in_dci;h->mps=hm;
                h->vid=d->vid;h->pid=d->pid;
                h->report_desc_length=g_r91_last_report_descriptor_length;
                h->out_ep=g_r96_hid_out_ep;
                h->out_dci=g_hid_out_dci;
                h->out_mps=g_r96_hid_out_mps;
                if(g_hid_out_dci)h->out_ring=g_hid_out_ring;
                h->ring=g_hid_in_ring;h->ep0=g_ep0_ring;
                h->ring_bank=g_native_bank;h->ring_epoch=0u;
                h->pending=0u;h->pending_length=0u;h->failures=0u;h->valid=1u;
                ++g_native_count;
                ++g_r97_next_bank; /* this DMA context is permanently live */
                frf_r83_hid_count=g_native_count;
                frf_r89_native_count=g_native_count;
                if(port<=4u) {
                    frf_r89_native_live_mask|=1u<<port;
                    frf_r90_sequence_step[port-1u]=5u;
                }
                if(port<=8u)frf_r83_downstream_step[port-1u]=5u;
                kprintf("[R89-USB] LIVE #%u port%u slot%u proto%u iface%u ep%u ringBank%u\n",
                  g_native_count,port,h->slot,h->protocol,hiface,he,g_native_bank);
                ++g_hub_device_count;
                /* No disable, no second port reset or re-address. */
                continue;
            }
            if(port<=8u)frf_r83_downstream_step[port-1u]=0xe5u;
            if(port<=4u) {
                frf_r90_sequence_step[port-1u]=0xe5u;
                frf_r89_native_fail_mask|=1u<<port;
            }
            kprintf("[R89-USB] port%u HID endpoint/config rejected\n",port);
        }
        hub_disable_current_child();
        ++g_hub_device_count;
    }
    frf_r94_usb_connected_mask=frf_r57_hub_connected_mask;
    frf_r94_usb_live_mask=frf_r89_native_live_mask;
    /* Leave root-hub EP0 selected for any future class requests. Per-HID
     * control transfers explicitly swap their saved per-slot ring. */
    g_slot_id=g_root_hub_slot;
    g_port_speed=g_root_hub_speed;
    g_ep0_mps=root_ep0_mps;
    g_ep0_ring=root_ep0_ring;
    g_r99_root_ep0_ring=root_ep0_ring;
    g_r99_root_ep0_mps=root_ep0_mps;
    g_r99_root_valid=frf_r99_root_monitor_port!=0u;
    g_r99_root_next_check=0u;
    g_route_string=0u;g_slot_hub=1u;g_slot_num_ports=g_hub_num_ports;
    g_tt_hub_slot=g_tt_port=g_tt_think_time=0u;
    if(g_native_count) {
        g_host=root_info;
        g_host.state=FRFXHCI_HOST_DEVICE;
        g_host.interface_class=USB_CLASS_HID;
        g_host.interface_protocol=g_native_hid[0].protocol;
        g_host.interface_number=g_native_hid[0].interface;
        g_host.slot_id=g_native_hid[0].slot;
        g_hid_ready=1u;
        frf_usb2_hub_mouse_status=2u;
        frf_usb_host_phase=0x62u;
        kprintf("[R80-HID] multi-device ARM USB active: %u endpoints\n",
                g_native_count);
        return 1;
    }

    if (!boot_port) {
        /* No mass storage or supported boot-mouse child. */
        g_slot_id=g_root_hub_slot; g_port_speed=g_root_hub_speed; g_ep0_mps=root_ep0_mps; g_ep0_ring=root_ep0_ring;
        g_route_string=0U; g_root_hub_port=g_selected_root_port; g_slot_hub=1U; g_slot_num_ports=g_hub_num_ports;
        g_tt_hub_slot=g_tt_port=g_tt_think_time=0U;
        g_host=root_info; g_host.state=FRFXHCI_HOST_DEVICE;
        kprintf("[FRFXHCI-HOST] ENUM PASS devices=%u hub=1 ports=%u boot-slot=0 root-portsc=%08x\n",
                g_hub_device_count+1U,g_hub_num_ports,host_read32(g_op+XHCI_OP_PORTSC1));
        return 1;
    }

    /* Re-enter root hub EP0 only to reset the chosen boot child back to address 0. */
    g_slot_id=g_root_hub_slot; g_port_speed=g_root_hub_speed; g_ep0_mps=root_ep0_mps; g_ep0_ring=root_ep0_ring;
    g_route_string=0U; g_root_hub_port=g_selected_root_port; g_slot_hub=1U; g_slot_num_ports=g_hub_num_ports;
    g_tt_hub_slot=g_tt_port=g_tt_think_time=0U;
    uint32_t bspeed=0U;
    if (!hub_reset_downstream_port(boot_port,&bspeed)) return 0;
    if (!address_child_device(boot_port,bspeed,g_root_hub_slot,g_root_hub_speed,g_hub_tt_think_time,0u,0u)) return 0;
    struct FRFXHCIHubDeviceInfo bootdev; frf_zero_bytes(&bootdev,sizeof(bootdev));
    bootdev.valid=1U; bootdev.hub_port=boot_port; bootdev.slot_id=g_slot_id; bootdev.speed_id=bspeed;
    uint32_t ie=0U,oe=0U,im=0U,om=0U,cfg=0U;
    uint32_t he=0u,hm=0u,hi=0u,hiface=0xffffffffu;
    if (!hub_parse_child(&bootdev,&ie,&oe,&im,&om,&cfg,&he,&hm,&hi,&hiface) || !bootdev.mass_storage || !ie || !oe || !im || !om)
        return 0;
    if (!configure_bulk_endpoints(ie,oe,im,om)) return 0;
    if (!frf_xhci_host_control(0x00U,USB_REQ_SET_CONFIGURATION,(uint16_t)cfg,0U,NULL,0U,0,NULL)) return 0;
    const uint32_t boot_slot=g_slot_id;
    const uint32_t bin_dci=g_bulk_in_dci, bout_dci=g_bulk_out_dci;

    /* Preserve root-hub identity for the UI, but advertise the active child BOT transport. */
    g_host=root_info;
    g_host.msc_ready=1U;
    g_host.state=FRFXHCI_HOST_MASS_STORAGE;
    g_host.slot_id=boot_slot;
    g_host.bulk_in_ep=ie; g_host.bulk_out_ep=oe;
    g_host.bulk_in_mps=im; g_host.bulk_out_mps=om;
    g_bulk_in_dci=bin_dci; g_bulk_out_dci=bout_dci;
    for (uint32_t i=0U;i<g_hub_device_count;++i)
        if (g_hub_devices[i].hub_port==boot_port) g_hub_devices[i].slot_id=boot_slot;
    kprintf("[FRFXHCI-HOST] ENUM PASS devices=%u hub=1 ports=%u boot-slot=%u root-portsc=%08x\n",
            g_hub_device_count+1U,g_hub_num_ports,boot_slot,host_read32(g_op+XHCI_OP_PORTSC1));
    return 1;
}

uint32_t frf_xhci_host_device_count(void)
{
    return g_hub_device_count;
}

uint32_t frf_xhci_host_hub_port_count(void)
{
    return g_hub_num_ports;
}

const volatile struct FRFXHCIHubDeviceInfo *frf_xhci_host_device(uint32_t index)
{
    if (index >= g_hub_device_count) return NULL;
    return &g_hub_devices[index];
}

static int controller_take_ownership(void)
{
    if (host_cpu() != 1U) {
        g_host.error = FRFXHCI_HE_NOT_CPU1;
        return 0;
    }

    if (frf_xhci_probe_run() != FRFXHCI_PROBE_OK) {
        g_host.error = FRFXHCI_HE_PROBE;
        return 0;
    }
    const volatile struct FRFXHCIProbeInfo *p = frf_xhci_probe_info();
    if (!p || p->access_mode != 3U || !p->virt_base || p->reg_bytes < 0x1000U) {
        g_host.error = FRFXHCI_HE_PROBE;
        return 0;
    }

    g_mmio = (uintptr_t)p->virt_base;
    g_reg_bytes = p->reg_bytes;
    g_cap_length = p->capbase & 0xFFU;
    g_op = g_mmio + g_cap_length;
    g_rt = g_mmio + p->rtsoff;
    g_db = g_mmio + p->dboff;
    g_context_size = (p->hccparams1 & (1U << 2)) ? 64U : 32U;
    g_host.state = FRFXHCI_HOST_PROBED;
    g_host.port_connected = (p->portsc1 & XHCI_PORT_CCS) ? 1U : 0U;
    g_host.speed_id = (p->portsc1 & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;

    kprintf("[FRFXHCI-HOST] takeover before PORTSC=%08x\n",
            host_read32(g_op + XHCI_OP_PORTSC1));

    frf_usb_host_phase=1u; /* before HALT */
    uint32_t cmd = host_read32(g_op + XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RUN;
    host_write32(g_op + XHCI_OP_USBCMD, cmd);
    if (!host_wait_mask(g_op + XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100U)) {
        g_host.error = FRFXHCI_HE_HALT_TIMEOUT;
        return 0;
    }

    frf_usb_host_phase=2u; /* before host RESET */
    host_write32(g_op + XHCI_OP_USBCMD, cmd | XHCI_CMD_HCRST);
    if (!host_wait_mask(g_op + XHCI_OP_USBCMD, XHCI_CMD_HCRST, 0U, 500U)) {
        g_host.error = FRFXHCI_HE_RESET_TIMEOUT;
        return 0;
    }
    if (!host_wait_mask(g_op + XHCI_OP_USBSTS, XHCI_STS_CNR, 0U, 500U)) {
        g_host.error = FRFXHCI_HE_CNR_TIMEOUT;
        return 0;
    }

    frf_r55_first_hse=0u;
    frf_r55_dma_bad=0u;
    for (unsigned i=0;i<12u;i++) frf_r55_sts[i]=0xffffffffu;
    frf_usb_r54_sts_after_reset = host_read32(g_op + XHCI_OP_USBSTS);
    r55_sample_sts(0,g_op);
    /* Clear stale HSE/PCD *immediately after HCRST* to see which
     * subsequent register write first re-asserts an error. */
    host_write32(g_op + XHCI_OP_USBSTS, (1U<<2)|(1U<<3)|(1U<<4));
    r55_sample_sts(1,g_op);
    if (!(host_read32(g_op + XHCI_OP_PAGESIZE) & 1U)) {
        g_host.error = FRFXHCI_HE_PAGE_SIZE;
        return 0;
    }

    ring_init(&g_cmd_ring, g_cmd_trbs, XHCI_RING_TRBS);
    host_zero(g_event_trbs, sizeof(g_event_trbs));
    host_zero(g_dcbaa, sizeof(g_dcbaa));
    host_zero(&g_erst, sizeof(g_erst));

    frf_usb_host_phase=3u; /* before cap HCSPARAMS2 fetch */
    const uint32_t hcs2 = host_read32(g_mmio + XHCI_CAP_HCSPARAMS2);
    const uint32_t scratch = scratchpad_count(hcs2);
    frf_usb_hcsparams2=hcs2;
    frf_usb_scratchpads_needed=scratch;
    frf_usb_host_phase=4u; /* scratchpad requirements decoded */
    kprintf("[VL805:R42] HCSPARAMS2=%08x scratch=%u capacity=%u\n",
             hcs2,scratch,(unsigned)XHCI_MAX_SCRATCHPADS);
    if (scratch > XHCI_MAX_SCRATCHPADS) {
        g_host.error = FRFXHCI_HE_SCRATCHPAD;
        return 0;
    }
    frf_usb_scratchpads_ready=0u;
    if (scratch) {
        frf_usb_host_phase=5u; /* populating scratchpad DCBAA */
        host_zero(g_scratchpad_ptrs, sizeof(g_scratchpad_ptrs));
        for (uint32_t i = 0; i < scratch; ++i) {
            host_zero(g_scratchpads[i], sizeof(g_scratchpads[i]));
            const uint64_t a = host_dma(g_scratchpads[i]);
            g_scratchpad_ptrs[i] = LE64(a);
        }
        host_cache_clean(g_scratchpad_ptrs, scratch * sizeof(uint64_t));
        g_dcbaa[0] = LE64(host_dma(g_scratchpad_ptrs));
        frf_usb_scratchpads_ready=scratch;
    }
    host_cache_clean(g_dcbaa, sizeof(g_dcbaa));

    const uint64_t event_dma = host_dma(g_event_trbs);
    g_erst.addr_lo = LE32((uint32_t)event_dma);
    g_erst.addr_hi = LE32((uint32_t)(event_dma >> 32));
    g_erst.size = LE32(XHCI_EVENT_TRBS);
    g_erst.reserved = 0;
    host_cache_clean(&g_erst, sizeof(g_erst));

    g_event_dequeue = 0;
    g_event_cycle = 1;
    frf_usb_host_phase=6u; /* computing DMA rings */
    const uint64_t cmd_dma = host_dma(g_cmd_trbs);
    const uint64_t dcbaa_dma = host_dma(g_dcbaa);
    const uint64_t erst_dma = host_dma(&g_erst);
    frf_r55_phys_cmd=mmu_virt2phys((uintptr_t)g_cmd_trbs);
    frf_r55_phys_evt=mmu_virt2phys((uintptr_t)g_event_trbs);
    frf_r55_phys_dcbaa=mmu_virt2phys((uintptr_t)g_dcbaa);
    frf_r55_phys_erst=mmu_virt2phys((uintptr_t)&g_erst);
    frf_r55_bus_cmd=cmd_dma; frf_r55_bus_evt=event_dma;
    frf_r55_bus_dcbaa=dcbaa_dma; frf_r55_bus_erst=erst_dma;
    frf_xhci_dma_cmd=(uint32_t)cmd_dma;
    frf_xhci_dma_event=(uint32_t)event_dma;
    frf_xhci_dma_dcbaa=(uint32_t)dcbaa_dma;
    frf_xhci_dma_erst=(uint32_t)erst_dma;
    kprintf("[VL805-DMA] CMD=%08x EVT=%08x DCBAA=%08x ERST=%08x\n",
            frf_xhci_dma_cmd,frf_xhci_dma_event,frf_xhci_dma_dcbaa,frf_xhci_dma_erst);
    /* For a 64-bit capable VL805 only, reject impossible/unreachable
     * ring pointers before any register write. This covers scratchpads too. */
    if (frf_r55_dma_bad || (cmd_dma >> 48) || (event_dma >> 48) ||
        (dcbaa_dma >> 48) || (erst_dma >> 48) ||
        host_dma(g_scratchpad_ptrs)==UINT64_MAX ||
        host_dma(g_scratchpads[0])==UINT64_MAX ||
        host_dma(g_scratchpads[scratch?scratch-1u:0u])==UINT64_MAX ||
        host_dma(g_input_context)==UINT64_MAX) {
        g_host.error = FRFXHCI_HE_DMA_ADDRESS;
        return 0;
    }

    uint32_t slots = p->max_slots;
    if (slots > XHCI_MAX_SLOTS_USED) slots = XHCI_MAX_SLOTS_USED;
    host_write32(g_op + XHCI_OP_CONFIG, slots);
    r55_sample_sts(2,g_op);
    host_write64(g_op + XHCI_OP_DCBAAP, dcbaa_dma);
    r55_sample_sts(3,g_op);
    host_write64(g_op + XHCI_OP_CRCR, cmd_dma | 1ULL);
    r55_sample_sts(4,g_op);
    frf_usb_cmd_crcr_after_write=host_read32(g_op+XHCI_OP_CRCR);

    const uintptr_t ir0 = g_rt + XHCI_RT_IR0;
    host_write32(ir0 + XHCI_IR_IMAN, 0U);
    host_write32(ir0 + XHCI_IR_IMOD, 0U);
    r55_sample_sts(5,g_op);
    host_write32(ir0 + XHCI_IR_ERSTSZ, 1U);
    r55_sample_sts(6,g_op);
    host_write64(ir0 + XHCI_IR_ERSTBA, erst_dma);
    r55_sample_sts(7,g_op);
    host_write64(ir0 + XHCI_IR_ERDP, event_dma | XHCI_ERDP_EHB);
    r55_sample_sts(8,g_op);
    frf_usb_cmd_erst_after_write=host_read32(ir0+XHCI_IR_ERSTBA);
    frf_usb_cmd_erdp_after_write=host_read32(ir0+XHCI_IR_ERDP);

    /* R45: capture the actual operational-register state, not an inferred
     * phase. These are all read-only snapshots around the existing RUN write. */
    frf_usb_run_hcs1 = host_read32(g_mmio + 0x04U);
    frf_usb_run_hcc1 = host_read32(g_mmio + 0x10U);
    frf_usb_run_pagesize = host_read32(g_op + XHCI_OP_PAGESIZE);
    frf_usb_run_config = host_read32(g_op + XHCI_OP_CONFIG);
    frf_usb_run_crcr_lo = host_read32(g_op + XHCI_OP_CRCR);
    frf_usb_run_crcr_hi = host_read32(g_op + XHCI_OP_CRCR + 4U);
    frf_usb_run_dcbaa_lo = host_read32(g_op + XHCI_OP_DCBAAP);
    frf_usb_run_dcbaa_hi = host_read32(g_op + XHCI_OP_DCBAAP + 4U);
    /* R54: capture and acknowledge any *sticky* host error already set
     * before RUN. USBSTS[2,3,4] are RW1C. If HSE reasserts, the controller
     * or PCIe/DMA initialization remains broken; the next diagnostic shows it. */
    frf_usb_r54_sts_before_clear = host_read32(g_op + XHCI_OP_USBSTS);
    host_write32(g_op + XHCI_OP_USBSTS, (1U<<2) | (1U<<3) | (1U<<4));
    frf_usb_r54_sts_after_clear = host_read32(g_op + XHCI_OP_USBSTS);
    frf_usb_run_cmd_before = host_read32(g_op + XHCI_OP_USBCMD);
    frf_usb_run_sts_before = host_read32(g_op + XHCI_OP_USBSTS);
    r55_sample_sts(9,g_op);
    frf_usb_host_phase=7u; /* RUN write pending */
    cmd = frf_usb_run_cmd_before;
    frf_usb_run_cmd_written = cmd | XHCI_CMD_RUN;
    host_write32(g_op + XHCI_OP_USBCMD, frf_usb_run_cmd_written);
    frf_usb_run_cmd_after = host_read32(g_op + XHCI_OP_USBCMD);
    frf_usb_run_sts_after = host_read32(g_op + XHCI_OP_USBSTS);
    frf_usb_r54_sts_after_run = frf_usb_run_sts_after;
    r55_sample_sts(10,g_op);
    if (!host_wait_mask(g_op + XHCI_OP_USBSTS, XHCI_STS_HCH, 0U, 100U)) {
        frf_usb_run_cmd_after = host_read32(g_op + XHCI_OP_USBCMD);
        frf_usb_run_sts_timeout = host_read32(g_op + XHCI_OP_USBSTS);
        r55_sample_sts(11,g_op);
        frf_usb_host_phase=0x71u; /* RUN timeout: raw registers captured */
        g_host.error = FRFXHCI_HE_RUN_TIMEOUT;
        kprintf("[R45-RUN] timeout CMD before=%08x write=%08x after=%08x STS before=%08x after=%08x timeout=%08x CRCR=%08x:%08x DCBAAP=%08x:%08x CONFIG=%08x PAGE=%08x HCS1=%08x HCC=%08x\\n",
                frf_usb_run_cmd_before,frf_usb_run_cmd_written,frf_usb_run_cmd_after,
                frf_usb_run_sts_before,frf_usb_run_sts_after,frf_usb_run_sts_timeout,
                frf_usb_run_crcr_hi,frf_usb_run_crcr_lo,
                frf_usb_run_dcbaa_hi,frf_usb_run_dcbaa_lo,
                frf_usb_run_config,frf_usb_run_pagesize,
                frf_usb_run_hcs1,frf_usb_run_hcc1);
        return 0;
    }

    kprintf("[FRFXHCI-HOST] takeover after-run PORTSC=%08x\n",
            host_read32(g_op + XHCI_OP_PORTSC1));

    frf_usb_host_phase=8u; /* controller running */
    g_host.owned = 1U;
    g_host.state = FRFXHCI_HOST_OWNED;
    frf_services_usb_backend_state(FRFS_USB_BACKEND_READY);
    return 1;
}

static int host_scan_cpu1(void)
{
    g_native_count=0u;
    frf_r92_defer_rd=frf_r92_defer_wr=0u;
    frf_r92_deferred_events=frf_r92_deferred_overflow=0u;
    frf_r83_hid_count=0u;
    g_native_bank=0u;
    frf_r82_unknown_events=0u;
    for (unsigned i=0;i<FRF_NATIVE_HID_MAX;i++) {
        frf_r82_interval[i]=frf_r82_speed[i]=0u;
        frf_r82_arm[i]=frf_r82_completed[i]=frf_r82_failed[i]=0u;
        frf_r82_last_cc[i]=frf_r82_last_bytes[i]=frf_r82_last_word[i]=0u;
        frf_r88_hid_td_size[i]=frf_r88_hid_gap_last_ms[i]=frf_r88_hid_gap_max_ms[i]=0u;
        frf_r88_hid_last_cnt[i]=0u;
        frf_r92_recovery_state[i]=frf_r92_recovery_attempts[i]=0u;
        frf_r92_last_cc[i]=frf_r92_recovery_ok[i]=0u;
        frf_r92_silent_timeout[i]=0u;
        g_native_hid[i].valid=0u;
        g_native_hid[i].pending=0u;
        g_native_hid[i].pending_length=0u;
        g_native_hid[i].pending_since_cnt=0u;
        g_native_hid[i].retry_after_cnt=0u;
        g_native_hid[i].recover_pending=0u;
        g_native_hid[i].recover_count=0u;
        g_native_hid[i].ring_bank=0u;
        g_native_hid[i].ring_epoch=0u;
    }
    frf_sdlog_xhci_stage = 10u;
    frf_sdlog_xhci_error = 0u;
    const uint32_t generation = g_host.generation + 1U;
    host_clear_info(generation);
    frf_usb_boot_clear();
    g_slot_id = 0;
    g_bulk_in_dci = g_bulk_out_dci = 0;
    hub_zero_devices();
    g_route_string = 0U; g_root_hub_port = 1U; g_selected_root_port=1U;
    g_slot_hub = g_slot_num_ports = 0U;
    g_tt_hub_slot = g_tt_port = g_tt_think_time = 0U;

    if (!controller_take_ownership()) {
        frf_sdlog_xhci_stage = 0x80000011u;
        frf_sdlog_xhci_error = g_host.error;
        g_host.state = FRFXHCI_HOST_ERROR;
        frf_services_usb_backend_state(FRFS_USB_BACKEND_ERROR);
        return 0;
    }
    frf_sdlog_xhci_stage = 20u;
    if (!port_reset()) {
        frf_sdlog_xhci_stage = 0x80000021u;
        frf_sdlog_xhci_error = g_host.error;
        if (g_host.error == FRFXHCI_HE_NO_DEVICE) {
            g_host.state = FRFXHCI_HOST_OWNED;
            dsb();
            kprintf("[FRFXHCI-HOST] controller owned; root port has no device\n");
            return 1;
        }
        g_host.state = FRFXHCI_HOST_ERROR;
        frf_services_usb_backend_state(FRFS_USB_BACKEND_ERROR);
        return 0;
    }
    frf_sdlog_xhci_stage = 30u;
    frf_usb_host_phase=0x20u;
    if (!address_device()) {
        frf_sdlog_xhci_stage = 0x80000031u;
        frf_sdlog_xhci_error = g_host.error;
        g_host.state = FRFXHCI_HOST_ERROR;
        frf_services_usb_backend_state(FRFS_USB_BACKEND_ERROR);
        return 0;
    }
    frf_sdlog_xhci_stage = 40u;
    frf_usb_host_phase=0x30u;
    if (!parse_and_configure()) {
        frf_sdlog_xhci_stage = 0x80000041u;
        frf_sdlog_xhci_error = g_host.error;
        g_host.state = FRFXHCI_HOST_ERROR;
        frf_services_usb_backend_state(FRFS_USB_BACKEND_ERROR);
        return 0;
    }
    frf_sdlog_xhci_stage = 50u;
    frf_usb_host_phase=0x40u;
    if ((g_host.device_class == USB_CLASS_HUB || g_host.interface_class == USB_CLASS_HUB) && !hub_scan_children()) {
        g_host.state = FRFXHCI_HOST_ERROR;
        frf_services_usb_backend_state(FRFS_USB_BACKEND_ERROR);
        return 0;
    }


    /* A non-storage HID/etc. device is still a successful enumeration. */
    if (g_host.msc_ready)
        (void)frf_usb_boot_cpu1_scan();

    frf_usb_host_phase=0x50u;
    dsb();
    kprintf("[FRFXHCI-HOST] ENUM PASS slot=%u speed=%u vid=%04x pid=%04x class=%u/%u/%u msc=%u\n",
            g_host.slot_id, g_host.speed_id, g_host.vid, g_host.pid,
            g_host.interface_class, g_host.interface_subclass,
            g_host.interface_protocol, g_host.msc_ready);
    return 1;
}

static int bulk_transfer(struct XHCIRing *ring, uint32_t dci, void *data,
                         uint32_t length, int direction_in, uint32_t *actual)
{
    if (host_cpu() != 1U || !g_slot_id || !dci || !length || !data)
        return 0;
    if (length > 0x10000U)
        return 0;

    uint64_t trb_dma = 0;
    struct XHCITrb *t = ring_next(ring, &trb_dma);
    const uint64_t dma = host_dma(data);
    t->parameter_lo = LE32((uint32_t)dma);
    t->parameter_hi = LE32((uint32_t)(dma >> 32));
    t->status = LE32(length);
    t->control = LE32(XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP |
                      (ring->cycle ? XHCI_TRB_CYCLE : 0U));
    if (direction_in)
        host_cache_invalidate(data, length);
    else
        host_cache_clean(data, length);
    host_cache_clean(t, sizeof(*t));
    host_write32(g_db + (uintptr_t)g_slot_id * 4U, dci);

    uint32_t got = length;
    if (!event_wait_transfer_trb(trb_dma, 0U, 3000U, length, &got)) {
        kprintf("[FRFXHCI-HOST] bulk dci=%u TD completion failed\n", dci);
        return 0;
    }
    if (direction_in)
        host_cache_invalidate(data, length);
    if (actual) *actual = got;
    return 1;
}

int frf_xhci_host_bulk_in(void *data, uint32_t length, uint32_t *actual)
{
    if (!g_host.msc_ready)
        return 0;
    return bulk_transfer(&g_bulk_in_ring, g_bulk_in_dci, data, length, 1, actual);
}

int frf_xhci_host_bulk_out(const void *data, uint32_t length, uint32_t *actual)
{
    if (!g_host.msc_ready)
        return 0;
    return bulk_transfer(&g_bulk_out_ring, g_bulk_out_dci, (void *)(uintptr_t)data,
                         length, 0, actual);
}

static void request_post(uint32_t command, uint32_t index, void *dst, uint32_t capacity)
{
    g_request.command = command;
    g_request.file_index = index;
    g_request.dst = (uintptr_t)dst;
    g_request.capacity = capacity;
    g_request.result = 0;
    g_request.sequence++;
    dsb();
    g_request.state = HOST_REQ_READY;
    dsb();
    __asm__ volatile("sev" ::: "memory");
}

static int request_wait(uint32_t timeout_ms)
{
    const uint64_t deadline = host_deadline_ms(timeout_ms);
    int over_deadline_logged = 0;

    for (;;) {
        const uint32_t s = __atomic_load_n(&g_request.state, __ATOMIC_ACQUIRE);
        if (s == HOST_REQ_DONE)
            return g_request.result ? 1 : 0;
        if (s == HOST_REQ_ERROR)
            return 0;

        if (host_timed_out(deadline)) {
            if (s == HOST_REQ_READY) {
                /* Safe cancellation is possible only before CPU1 claims the
                 * request.  CPU1 uses the matching READY->RUNNING CAS below,
                 * so this cannot race into a use-after-free of a load buffer. */
                uint32_t expected = HOST_REQ_READY;
                if (__atomic_compare_exchange_n(&g_request.state, &expected,
                                                HOST_REQ_ERROR, 0,
                                                __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
                    g_request.result = 0U;
                    g_host.error = FRFXHCI_HE_REQUEST_TIMEOUT;
                    return 0;
                }
                continue;
            }

            /* Once CPU1 is RUNNING, the destination buffer is owned by that
             * request.  Every xHCI/BOT/FAT loop below is itself bounded, so
             * finish synchronously rather than returning while CPU1 may still
             * write into memory that CPU0 could otherwise free/reuse. */
            if (s == HOST_REQ_RUNNING && !over_deadline_logged) {
                kprintf("[FRFXHCI-HOST] request exceeded UI deadline; draining safely\n");
                over_deadline_logged = 1;
            }
        }
        __asm__ volatile("yield");
    }
}

int frf_xhci_host_request_scan(uint32_t timeout_ms)
{
    if (g_request.state == HOST_REQ_READY || g_request.state == HOST_REQ_RUNNING)
        return 0;
    request_post(HOST_REQ_SCAN, 0U, NULL, 0U);
    return request_wait(timeout_ms);
}

int frf_xhci_host_request_load_file(uint32_t index, void *dst,
                                    uint32_t capacity, uint32_t timeout_ms)
{
    if (!dst || !capacity)
        return 0;
    if (g_request.state == HOST_REQ_READY || g_request.state == HOST_REQ_RUNNING)
        return 0;
    request_post(HOST_REQ_LOAD_FILE, index, dst, capacity);
    return request_wait(timeout_ms);
}

int frf_xhci_host_cpu1_work(void)
{
    if (host_cpu() != 1U)
        return 0;

    uint32_t expected = HOST_REQ_READY;
    if (!__atomic_compare_exchange_n(&g_request.state, &expected,
                                     HOST_REQ_RUNNING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;

    const uint32_t command = g_request.command;
    const uint32_t index = g_request.file_index;
    void *dst = (void *)(uintptr_t)g_request.dst;
    const uint32_t capacity = g_request.capacity;
    dsb();

    int ok = 0;
    if (command == HOST_REQ_SCAN) {
        ok = host_scan_cpu1();
    } else if (command == HOST_REQ_LOAD_FILE) {
        ok = frf_usb_boot_cpu1_load(index, dst, capacity);
    }

    g_request.result = ok ? 1U : 0U;
    g_request.state = ok ? HOST_REQ_DONE : HOST_REQ_ERROR;
    dsb();
    __asm__ volatile("sev" ::: "memory");
    return 1;
}
