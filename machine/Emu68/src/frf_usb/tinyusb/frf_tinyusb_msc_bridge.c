/* SPDX-License-Identifier: GPL-2.0-only
 * EMU68AGA independent TinyUSB MSC <-> legacy FRUM 68k Zorro-II board.
 * CPU1 owns TinyUSB. Only one outstanding SCSI command, DMA staging
 * buffer is cache-line aligned and never aliases guest uncached memory.
 * Read-only until explicit R/W hardware verification.
 */
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "class/msc/msc_host.h"
#include "../frf_usb_mass.h"

extern volatile struct frf_um_shared *frf_um_host;
extern void kprintf(const char *fmt,...);

static uint8_t active_addr, physical_msc_count;
static uint32_t block_count, change_count, device_token;
static uint32_t request_seq, request_cmd, request_bytes;
static uint8_t in_flight;
static uint8_t media_changed;
static uint8_t pending_read_data[FRF_UM_MAX_SECTORS*FRF_UM_SECTOR_SIZE]
  __attribute__((aligned(64)));

uint32_t frf_tinyusb_msc_devices(void) { return physical_msc_count; }
uint32_t frf_tinyusb_msc_capacity(void) { return block_count; }

static volatile struct frf_um_shared *board(void)
{
    volatile struct frf_um_shared *s =
        __atomic_load_n(&frf_um_host,__ATOMIC_ACQUIRE);
    return s && s->magic==FRF_UM_MAGIC ? s : 0;
}

void frf_tinyusb_msc_mount(uint8_t addr)
{
    if (!addr || !tuh_msc_mounted(addr)) return;
    ++physical_msc_count;
    uint32_t sector_bytes=tuh_msc_get_block_size(addr,0);
    uint32_t blocks=tuh_msc_get_block_count(addr,0);
    kprintf("[TINYUSB-MSC] mounted daddr=%u blocks=%u sector_size=%u\\n",
            addr,blocks,sector_bytes);
    /* Legacy Amiga Zorro-II transport is defined for 512-byte sectors.
       We detect drives with other sector sizes, but don't corrupt them. */
    if(!active_addr && sector_bytes==FRF_UM_SECTOR_SIZE && blocks>0) {
        active_addr=addr;
        block_count=blocks;
        media_changed=1;
        ++change_count;
        ++device_token;
    }
}

void frf_tinyusb_msc_unmount(uint8_t addr)
{
    if(physical_msc_count) --physical_msc_count;
    if(addr!=active_addr) return;
    ++device_token; /* invalidate any outstanding completion */
    active_addr=0;
    block_count=0;
    media_changed=1;
    ++change_count;
    in_flight=0;
    volatile struct frf_um_shared *s=board();
    if(s) {
        s->present=0u;
        s->capacity=0u;
        s->media_change=change_count;
        if(s->state==FRF_UM_BUSY) {
            s->result=(uint32_t)-1;
            s->completed=request_seq;
            __atomic_store_n(&s->state,FRF_UM_ERROR,__ATOMIC_RELEASE);
        }
    }
}

static bool scsi_complete(uint8_t daddr, const tuh_msc_complete_data_t *done)
{
    if(!in_flight || !done || daddr!=active_addr ||
       done->user_arg!=(uintptr_t)device_token)
        return true;
    volatile struct frf_um_shared *s=board();
    const int good=(done->csw && done->csw->status==MSC_CSW_STATUS_PASSED);
    if(s && s->state==FRF_UM_BUSY && s->sequence==request_seq) {
        if(good && request_cmd==FRF_UM_READ) {
            for(uint32_t i=0;i<request_bytes;++i)
                s->data[i]=pending_read_data[i];
        }
        s->result=good ? request_bytes : (uint32_t)-1;
        s->completed=request_seq;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&s->state,good?FRF_UM_DONE:FRF_UM_ERROR,
                         __ATOMIC_RELEASE);
    }
    in_flight=0;
    return true;
}

/* Invoke each CPU1 TinyUSB task iteration after tuh_task_ext().
   The guest never calls TinyUSB or accesses CPU1-owned USB registers. */
void frf_tinyusb_msc_service(void)
{
    volatile struct frf_um_shared *s=board();
    if(!s) return;
    const uint8_t addr=active_addr;
    const uint8_t ready=addr && tuh_msc_mounted(addr) && block_count>0;
    s->reserved[0]=physical_msc_count;
    s->reserved[1]=0x4d534350u; /* MSCP legacy probe ABI */
    if(media_changed || s->present != (uint32_t)ready ||
       s->capacity!=(ready?block_count:0u)) {
        s->present=ready?1u:0u;
        s->capacity=ready?block_count:0u;
        s->sector_size=FRF_UM_SECTOR_SIZE;
        s->media_change=change_count;
        media_changed=0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
    if(in_flight || s->state!=FRF_UM_REQ) return;
    const uint32_t cmd=s->command, lba=s->lba, sectors=s->sectors;
    request_seq=s->sequence;
    request_cmd=cmd;
    request_bytes=sectors*FRF_UM_SECTOR_SIZE;
    __atomic_store_n(&s->state,FRF_UM_BUSY,__ATOMIC_RELEASE);
    if(!ready || sectors==0 || sectors>FRF_UM_MAX_SECTORS ||
       lba>=block_count || sectors>block_count-lba ||
       (cmd!=FRF_UM_READ && cmd!=FRF_UM_WRITE)) {
        s->result=(uint32_t)-1;
        s->completed=request_seq;
        __atomic_store_n(&s->state,FRF_UM_ERROR,__ATOMIC_RELEASE);
        return;
    }
#ifndef FRF_TINYUSB_MSC_WRITE
    if(cmd==FRF_UM_WRITE) {
        /* Preserve drive data until a real 512-byte SCSI write test passes. */
        s->result=(uint32_t)-2;
        s->completed=request_seq;
        __atomic_store_n(&s->state,FRF_UM_ERROR,__ATOMIC_RELEASE);
        return;
    }
#else
    if(cmd==FRF_UM_WRITE) {
        for(uint32_t i=0;i<request_bytes;++i)
            pending_read_data[i]=s->data[i];
    }
#endif
    if(!tuh_msc_ready(addr)) {
        /* Don't fail a valid guest request solely because another SCSI
           command is in progress; retry on next CPU1 task iteration. */
        __atomic_store_n(&s->state,FRF_UM_REQ,__ATOMIC_RELEASE);
        return;
    }
    in_flight=1;
    bool accepted;
#ifdef FRF_TINYUSB_MSC_WRITE
    if(cmd==FRF_UM_WRITE)
        accepted=tuh_msc_write10(addr,0,pending_read_data,lba,
                                (uint16_t)sectors,scsi_complete,device_token);
    else
#endif
        accepted=tuh_msc_read10(addr,0,pending_read_data,lba,
                               (uint16_t)sectors,scsi_complete,device_token);
    if(!accepted) {
        in_flight=0;
        s->result=(uint32_t)-1;
        s->completed=request_seq;
        __atomic_store_n(&s->state,FRF_UM_ERROR,__ATOMIC_RELEASE);
    }
}
