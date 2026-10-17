/* SPDX-License-Identifier: GPL-2.0-or-later
 * FRF Emu68 standalone USB mass-storage Zorro-II shared transport.
 * Version 1. The 68k guest and big-endian ARM agree on 32-bit words.
 * USB is owned exclusively by CPU1; guest never touches DWC2 registers.
 */
#ifndef FRF_USB_MASS_H
#define FRF_USB_MASS_H
#include <stdint.h>

#define FRF_UM_MAGIC       0x4652554du  /* "FRUM" */
#define FRF_UM_VERSION     1u
#define FRF_UM_VENDOR      0x6d73u
#define FRF_UM_PRODUCT     0x69u
#define FRF_UM_BOARD_BYTES 65536u
#define FRF_UM_SECTOR_SIZE 512u
#define FRF_UM_MAX_SECTORS 16u

#define FRF_UM_IDLE   0u
#define FRF_UM_REQ    1u
#define FRF_UM_BUSY   2u
#define FRF_UM_DONE   3u
#define FRF_UM_ERROR  4u
#define FRF_UM_READ   1u
#define FRF_UM_WRITE  2u  /* 512-byte sector write; same mailbox framing */

struct frf_um_shared {
    volatile uint32_t magic;       /* 0x00 */
    volatile uint32_t version;     /* 0x04 */
    volatile uint32_t bytes;       /* 0x08 */
    volatile uint32_t state;       /* 0x0c */
    volatile uint32_t present;     /* 0x10 (0 absent, 1 media ready) */
    volatile uint32_t capacity;    /* 0x14, 512-byte sectors */
    volatile uint32_t sector_size; /* 0x18 */
    volatile uint32_t media_change;/* 0x1c, static during first boot */
    volatile uint32_t sequence;    /* 0x20, caller increments */
    volatile uint32_t command;     /* 0x24 */
    volatile uint32_t lba;         /* 0x28 */
    volatile uint32_t sectors;     /* 0x2c */
    volatile uint32_t result;      /* 0x30, bytes read or negative error */
    volatile uint32_t completed;   /* 0x34, echoed sequence */
    volatile uint32_t reserved[2]; /* 0x38 */
    uint8_t data[FRF_UM_MAX_SECTORS * FRF_UM_SECTOR_SIZE]; /* 0x40 */
};
typedef char frf_um_data_offset_check[
    (__builtin_offsetof(struct frf_um_shared,data)==64u) ? 1 : -1];
typedef char frf_um_board_size_check[
    (sizeof(struct frf_um_shared) <= FRF_UM_BOARD_BYTES) ? 1 : -1];
#endif
