#ifndef FRF_USB_BOOT_H
#define FRF_USB_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRF_USB_BOOT_MAX_FILES       4096U
#define FRF_USB_BOOT_NAME_BYTES      32U

#define FRF_USB_BOOT_FILE_ROM        (1U << 0)
#define FRF_USB_BOOT_FILE_HDF        (1U << 1)
#define FRF_USB_BOOT_FILE_ADF        (1U << 2)
#define FRF_USB_BOOT_FILE_BOOTABLE   (1U << 3)

struct FRFUSBBootFile {
    char name[FRF_USB_BOOT_NAME_BYTES];
    uint32_t size;
    uint32_t first_cluster;
    uint32_t flags;
};

struct FRFUSBBootInfo {
    uint32_t generation;
    uint32_t ready;
    uint32_t error;
    uint32_t block_size;
    uint32_t block_count_lo;
    uint32_t block_count_hi;
    uint32_t capacity_mib;
    uint32_t fat32;
    uint32_t partition_lba;
    uint32_t file_count;
    uint32_t scanning_amiga_dir;
    struct FRFUSBBootFile files[FRF_USB_BOOT_MAX_FILES];
};

const volatile struct FRFUSBBootInfo *frf_usb_boot_info(void);

/* CPU1-only routines. The xHCI host invokes these inside its request worker. */
int frf_usb_boot_cpu1_scan(void);
int frf_usb_boot_cpu1_load(uint32_t index, void *dst, uint32_t capacity);
void frf_usb_boot_clear(void);

#ifdef __cplusplus
}
#endif


/* FRF_EMU68_USB_RDB_BOOT_V0_5_5: CPU1-only random read exports. */
int frf_usb_boot_cpu1_read_physical_blocks(uint64_t lba, uint16_t count, void *dst);
int frf_usb_boot_cpu1_read_hdf_blocks(uint32_t index, uint64_t file_lba, uint16_t count, void *dst);

#ifndef FRF_USB_BOOT_FILE_KICKSTART
#define FRF_USB_BOOT_FILE_KICKSTART 0x40000000U /* /KICKROM or /DEVS/KICKST* */
#endif


/* FRF_USB_FILE_BROWSER_V0_5_50
 * Read-only FAT browser catalogue.  Rebuilt only by explicit USB scan.
 */
#ifndef FRF_USB_BROWSER_MAX_ENTRIES
#define FRF_USB_BROWSER_MAX_ENTRIES 4096U
#endif
#define FRF_USB_BROWSER_NAME_BYTES 128U
#define FRF_USB_BROWSER_INDEX_NONE 0xffffffffU
#define FRF_USB_BROWSER_DIRECTORY  (1U << 0)
#define FRF_USB_BROWSER_ROM        (1U << 1)
#define FRF_USB_BROWSER_ADF        (1U << 2)
#define FRF_USB_BROWSER_HDF        (1U << 3)
#define FRF_USB_BROWSER_KICKSTART  (1U << 4)
#define FRF_USB_BROWSER_CFG        (1U << 5)

struct FRFUSBBrowserEntry {
    char name[FRF_USB_BROWSER_NAME_BYTES];
    uint32_t size;
    uint32_t first_cluster;
    uint32_t parent;
    uint32_t flags;
    uint32_t legacy_index;
};

uint32_t frf_usb_browser_count(void);
uint32_t frf_usb_browser_generation(void);
uint32_t frf_usb_browser_truncated(void);
const volatile struct FRFUSBBrowserEntry *frf_usb_browser_entry(uint32_t index);


#ifndef FRF_USB_BOOT_FILE_CFG
#define FRF_USB_BOOT_FILE_CFG 0x20000000U
#endif
#endif
