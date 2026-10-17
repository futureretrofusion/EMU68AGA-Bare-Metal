/* SPDX-License-Identifier: GPL-2.0-or-later
 * FRF USB0 read/write storage worker (v0.10.45). Called only on CPU1 from frf_usb_main.
 * Never call USPi from the emulated CPU0 or from an Amiga interrupt.
 */
#include <stdint.h>
#include <uspi/types.h>
#include <uspi.h>
#include <support.h>
#include "frf_usb_mass.h"
extern volatile struct frf_um_shared *frf_um_host;
/* CPU1 only: incremented by the USB interface class factory during boot. */
volatile unsigned frf_um_msc_interface_seen;

static uint32_t boot_capacity;
static int storage_count;
static int tested;
static uint32_t reported_capacity;

/* Call once, after USPiInitialize(), while CPU1 owns the USB stack. */
void frf_um_init(void)
{
    storage_count=USPiMassStorageDeviceAvailable();
    boot_capacity=storage_count>0 ? USPiMassStorageDeviceGetCapacity(0):0;
    tested=1;
    kprintf("[USBMASS] boot devices=%d USB0 capacity=%u x 512 bytes (%u MiB), RW TEST\n",
            storage_count, boot_capacity,boot_capacity/2048u);
}

/* Only the CPU1 main loop may touch this function. */
void frf_um_service(void)
{
    volatile struct frf_um_shared *s=
        __atomic_load_n(&frf_um_host,__ATOMIC_ACQUIRE);
    if (!tested || !s || s->magic!=FRF_UM_MAGIC)
        return;

    /* Non-breaking v1 protocol extension in reserved words, read only by
     * usbmass-probe. Does not claim the device is mountable. */
    s->reserved[0]=frf_um_msc_interface_seen;
    s->reserved[1]=0x4D534350u; /* MSCP */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    if (!reported_capacity) {
        s->capacity=boot_capacity;
        s->present=(storage_count>0 && boot_capacity>0) ? 1u : 0u;
        s->media_change=1u;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        reported_capacity=1;
    }
    if (__atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=FRF_UM_REQ)
        return;

    const uint32_t seq=s->sequence;
    const uint32_t cmd=s->command;
    const uint32_t lba=s->lba;
    const uint32_t sectors=s->sectors;
    __atomic_store_n(&s->state,FRF_UM_BUSY,__ATOMIC_RELEASE);

    int rc=-1;
    if (s->present && (cmd==FRF_UM_READ || cmd==FRF_UM_WRITE) && sectors>0
        && sectors<=FRF_UM_MAX_SECTORS
        && lba<boot_capacity
        && sectors<=boot_capacity-lba) {
        /* USPi requires multiples of 512 and 512-byte aligned offset.
         * Page data starts at 64, so the buffer is at least 16-byte aligned.
         * All bus access remains exclusively on CPU1. */
        const unsigned bytes=sectors*FRF_UM_SECTOR_SIZE;
        if(cmd==FRF_UM_WRITE) {
            /* Write payload was staged by guest before publishing REQ. */
            rc=USPiMassStorageDeviceWrite(
                (unsigned long long)lba*FRF_UM_SECTOR_SIZE,
                (const void*)s->data,bytes,0);
        } else {
            rc=USPiMassStorageDeviceRead(
                (unsigned long long)lba*FRF_UM_SECTOR_SIZE,
                (void*)s->data,bytes,0);
        }
    }
    s->result=(uint32_t)rc;
    s->completed=seq;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->state,
       (rc==(int)(sectors*FRF_UM_SECTOR_SIZE)) ?
        FRF_UM_DONE:FRF_UM_ERROR,__ATOMIC_RELEASE);
}
