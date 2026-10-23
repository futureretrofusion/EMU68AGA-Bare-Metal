#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "common/tusb_common.h"
#include "class/hid/hid.h"

int main(void) {
    /* HID descriptor bytes with wReportLength = 191 (0x00BF) in USB little-endian. */
    uint8_t raw[9] = { 9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0xBF, 0x00 };
    const tusb_hid_descriptor_hid_t *h = (const tusb_hid_descriptor_hid_t *)raw;
    const uint16_t current = tu_unaligned_read16((const uint8_t*)h + offsetof(tusb_hid_descriptor_hid_t, wReportLength));
    const uint16_t fixed   = tu_le16toh(current);
    printf("raw=%02X %02X current=%u/0x%04X fixed=%u/0x%04X\n",
           raw[7],raw[8], current,current,fixed,fixed);
    if (current > CFG_TUH_ENUMERATION_BUFSIZE)
        puts("CURRENT_BRANCH: descriptor skipped as too large");
    else
        puts("CURRENT_BRANCH: descriptor fetched");
    if (fixed > CFG_TUH_ENUMERATION_BUFSIZE)
        puts("FIXED_BRANCH: descriptor skipped");
    else
        puts("FIXED_BRANCH: descriptor fetched");
    if(fixed!=191u)return 2;
    return 0;
}
