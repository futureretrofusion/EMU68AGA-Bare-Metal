/* SPDX-License-Identifier: GPL-2.0-or-later
 * R22: Pi3 CPU1 read-only USB input telemetry on existing 64KB Zorro-II
 * mailbox, in unused space at offset 0x3400. No USB transport changes.
 * ARM64 BIG-ENDIAN and Amiga 68k agree on aligned 32-bit words.
 * Versioned magic is written LAST; stable sequence prevents torn reads.
 */
#ifndef FRF_R22_INPUT_DIAG_H
#define FRF_R22_INPUT_DIAG_H
#include <stdint.h>
#define FRF_R22_DIAG_OFFSET 0x3400u
#define FRF_R22_DIAG_MAGIC 0x50414432u /* "PAD2" */
#define FRF_R22_DIAG_VERSION 1u
struct frf_r22_diag {
  volatile uint32_t magic,version,sequence;
  volatile uint32_t tick_ms,host_ticks,pad_count;
  volatile uint32_t receiver_addr,receiver_hid_rx,receiver_len;
  volatile uint32_t receiver_word0,receiver_word1;
  volatile uint32_t hid_rx_total,compat_rx,compat_valid,compat_rejected;
  volatile uint32_t last_joy_fire,host_joy_writes;
  volatile uint32_t guest_joy_reads,guest_cia_reads;
  volatile uint32_t msc_present,msc_blocks;
};
#endif
