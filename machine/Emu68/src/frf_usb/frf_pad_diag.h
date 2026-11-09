/* USB receiver/joystick telemetry in the unused half of the existing Z2 board.
 * No enlargement of struct frf_um_shared: compatible with installed usbmass.device.
 * Big-endian ARM and 68k agree on 32-bit field byte order.
 */
#ifndef FRF_PAD_DIAG_H
#define FRF_PAD_DIAG_H
#include <stdint.h>
#define FRF_PAD_DIAG_OFF 0x3000u
#define FRF_PAD_DIAG_MAGIC 0x50414435u /* PAD5 */
struct frf_pad_diag {
    uint32_t magic;
    uint32_t receivers;
    uint32_t rx_ok;
    uint32_t rx_bad;
    uint32_t rx_fail;
    uint32_t rx_len;
    uint32_t rx_id;
    uint32_t sample_a;
    uint32_t sample_b;
    uint32_t reports_decoded;
    uint32_t pad_buttons;
    uint32_t pad_hat;
    uint32_t pad_axis01;
    uint32_t joy1dat;
    uint32_t fire;
    uint32_t updates;
    uint32_t storage_reads;
    uint32_t storage_writes;
    uint32_t storage_write_fail;
    uint32_t storage_last_cmd;
    uint32_t storage_last_lba;
    uint32_t storage_last_result;
};
#endif
