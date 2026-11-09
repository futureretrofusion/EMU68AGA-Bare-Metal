/* SPDX-License-Identifier: GPL-2.0-or-later
 * FRF usbmass.device 0.2, boot-present 512-byte USB disks read/write.
 * Loads as normal AmigaOS resident. Requires virtual Zorro II 6D73:0069
 * mapped by Emu68. USB controller is exclusively owned by ARM CPU1.
 */
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/expansion.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <libraries/configvars.h>
#include <stdint.h>
#include "frf_usb_mass.h"

struct USBMassDevice {
    struct Device dev;
    BPTR seg;
    volatile struct frf_um_shared *shared;
    ULONG users;
    ULONG busy;
    ULONG next_sequence;
};
static char name[]="usbmass.device";
static char id[]="usbmass.device 0.2 (17.10.2026) FRF RW test";
static char ver[]="\0$VER: usbmass.device 0.2 (17.10.2026)";
extern const UBYTE endOfCode;
void *initDev(struct USBMassDevice *d asm("d0"),BPTR seg asm("a0"),struct ExecBase *sys asm("a6"));
ULONG openDev(struct IOStdReq *io asm("a1"),ULONG num asm("d0"),ULONG flags asm("d1"),struct USBMassDevice *d asm("a6"));
ULONG closeDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6"));
BPTR expungeDev(struct USBMassDevice *d asm("a6"));
ULONG extDev(struct USBMassDevice *d asm("a6"));
void beginDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6"));
ULONG abortDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6"));

static const APTR funcs[]={
    (APTR)openDev,(APTR)closeDev,(APTR)expungeDev,(APTR)extDev,
    (APTR)beginDev,(APTR)abortDev,(APTR)-1
};
static const APTR initTab[]={
    (APTR)sizeof(struct USBMassDevice),(APTR)funcs,0,(APTR)initDev
};
static struct Resident const resident __attribute__((used))={
    RTC_MATCHWORD,(struct Resident*)&resident,(APTR)&endOfCode,
    RTF_AUTOINIT|RTF_AFTERDOS,0,NT_DEVICE,0,(APTR)name,(APTR)id,(APTR)initTab
};
/* Avoid a C runtime startup; Exec scans the resident structure. */
LONG __attribute__((used,no_reorder)) doNotExecute(void){return -1;}
/* EndSkip is emitted after every function, not before the entry points. */

static void finish(struct IOStdReq *io){
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg((struct Message*)io);
}
static void copy_to(void *dst,const void *src,ULONG bytes){
    UBYTE *d=(UBYTE*)dst;const UBYTE *s=(const UBYTE*)src;
    while(bytes--)*d++=*s++;
}
static BOOL live(struct USBMassDevice *d){
    volatile struct frf_um_shared *s=d->shared;
    return s && s->magic==FRF_UM_MAGIC &&
           s->version==FRF_UM_VERSION &&
           s->bytes==sizeof(struct frf_um_shared) &&
           s->sector_size==FRF_UM_SECTOR_SIZE &&
           s->present && s->capacity;
}
void *initDev(struct USBMassDevice *d asm("d0"), BPTR seg asm("a0"),struct ExecBase *sys asm("a6")){
    (void)sys;(void)ver;
    d->seg=seg;
    d->users=0;d->busy=0;d->shared=0;d->next_sequence=0;
    d->dev.dd_Library.lib_Revision=1;
    return d;
}
ULONG openDev(struct IOStdReq *io asm("a1"),ULONG unit asm("d0"),ULONG flags asm("d1"),struct USBMassDevice *d asm("a6")){
    (void)flags;
    io->io_Error=IOERR_OPENFAIL;
    if (unit!=0 || io->io_Message.mn_Length < sizeof(struct IOStdReq))return 0;
    if (!d->shared){
        struct Library *lib=OpenLibrary("expansion.library",36);
        if (!lib)return 0;
        struct ExpansionBase *ExpansionBase=(struct ExpansionBase*)lib;
        struct ConfigDev *cfg=FindConfigDev(NULL,FRF_UM_VENDOR,FRF_UM_PRODUCT);
        if(cfg && cfg->cd_BoardAddr)d->shared=(struct frf_um_shared*)cfg->cd_BoardAddr;
        CloseLibrary(lib);
    }
    if (!live(d))return 0;
    d->users++;
    io->io_Device=(struct Device*)d;
    io->io_Unit=(struct Unit*)d;
    io->io_Error=0;
    return 0;
}
ULONG closeDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6")){
    if(d->users)d->users--;
    io->io_Device=NULL;io->io_Unit=NULL;
    return 0;
}
BPTR expungeDev(struct USBMassDevice *d asm("a6")){(void)d;return 0;} /* pinned */
ULONG extDev(struct USBMassDevice *d asm("a6")){(void)d;return 0;}
ULONG abortDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6")){
    (void)io;(void)d;return IOERR_NOCMD;
}
static LONG transfer_disk(struct USBMassDevice *d,struct IOStdReq *io,int large,int writing){
    volatile struct frf_um_shared *s=d->shared;
    UBYTE *dest=(UBYTE*)io->io_Data;
    uint64_t offset=(uint64_t)io->io_Offset;
    ULONG remain=io->io_Length;
    if(large)offset |= ((uint64_t)io->io_Actual << 32);
    io->io_Actual=0;
    if (!dest || (offset & 511u) || (remain & 511u))return IOERR_BADADDRESS;
    if (!live(d))return TDERR_DiskChanged;
    if (offset/512u > s->capacity ||
        (uint64_t)(remain/512u) > s->capacity-offset/512u)
        return TDERR_BadSecPreamble;

    while(remain){
        ULONG sectors=remain/FRF_UM_SECTOR_SIZE;
        ULONG bytes,seq;
        if(sectors>FRF_UM_MAX_SECTORS)sectors=FRF_UM_MAX_SECTORS;
        bytes=sectors*FRF_UM_SECTOR_SIZE;
        /* No concurrent callers can share one mailbox. Forbid only while
         * taking the lock; NEVER hold it while waiting on CPU1. */
        Forbid();
        if(d->busy || s->state!=FRF_UM_IDLE){
            Permit();return TDERR_DriveInUse;
        }
        d->busy=1;
        Permit();
        /* Stage bytes before REQ; other Amiga requests observe d->busy.
         * Avoid copying 8 KiB while interrupts/task switching are forbidden. */
        if(writing)copy_to((void*)s->data,(const void*)dest,bytes);
        Forbid();
        seq=++d->next_sequence;
        if(!seq)seq=++d->next_sequence;
        s->command=writing?FRF_UM_WRITE:FRF_UM_READ;
        s->lba=(ULONG)(offset/512u);
        s->sectors=sectors;
        s->sequence=seq;
        __asm__ volatile("" ::: "memory");
        s->state=FRF_UM_REQ;
        Permit();

        /* CPU1 progresses independently. Bounded spin is an emergency
         * stop for absent firmware; this is NOT a wall-clock deadline.
         * Timeout leaves the mailbox owned by CPU1, avoiding buffer races. */
        ULONG limit=120000000u;
        BOOL finished=FALSE;
        while(limit--){
            ULONG state=s->state;
            if ((state==FRF_UM_DONE || state==FRF_UM_ERROR) &&
                 s->completed==seq) {
                finished=TRUE;
                break;
            }
        }
        if (!finished || s->completed!=seq){
            d->busy=0;
            return TDERR_NotSpecified;
        }
        LONG result=(LONG)s->result;
        if(s->state!=FRF_UM_DONE || result!=(LONG)bytes){
            s->state=FRF_UM_IDLE;
            d->busy=0;
            return TDERR_NotSpecified;
        }
        if(!writing)copy_to(dest,(const void*)s->data,bytes);
        __asm__ volatile("" ::: "memory");
        s->state=FRF_UM_IDLE;
        d->busy=0;
        dest+=bytes;remain-=bytes;offset+=bytes;
        io->io_Actual+=bytes;
    }
    return 0;
}
static const UWORD supported_commands[]={
    CMD_READ,TD_READ64,NSCMD_TD_READ64,
    CMD_WRITE,TD_WRITE64,NSCMD_TD_WRITE64,
    TD_GETGEOMETRY,TD_CHANGESTATE,TD_CHANGENUM,TD_PROTSTATUS,
    TD_GETDRIVETYPE,TD_MOTOR,TD_SEEK,CMD_CLEAR,CMD_UPDATE,
    NSCMD_DEVICEQUERY,0
};
void beginDev(struct IOStdReq *io asm("a1"),struct USBMassDevice *d asm("a6")){
    io->io_Error=0;
    switch(io->io_Command){
    case CMD_READ:
    case TD_READ64:
    case NSCMD_TD_READ64:
        io->io_Error=(BYTE)transfer_disk(d,io,io->io_Command!=CMD_READ,0);
        break;
    case CMD_WRITE:
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
        io->io_Error=(BYTE)transfer_disk(d,io,io->io_Command!=CMD_WRITE,1);
        break;
    case TD_FORMAT:
    case TD_FORMAT64:
        /* Formatting is deliberately outside the initial RW test. */
        io->io_Actual=0;
        io->io_Error=IOERR_NOCMD;
        break;
    case TD_CHANGESTATE:
        io->io_Actual=live(d)?0:1;
        break;
    case TD_PROTSTATUS:
        io->io_Actual=live(d)?0:1; /* removable media, writable when live */
        break;
    case TD_CHANGENUM:
        io->io_Actual=d->shared?d->shared->media_change:0;
        break;
    case TD_GETDRIVETYPE:
        io->io_Actual=DG_DIRECT_ACCESS;
        break;
    case TD_GETGEOMETRY: {
        struct DriveGeometry *g=(struct DriveGeometry*)io->io_Data;
        if(!g || io->io_Length<sizeof(*g)){
            io->io_Error=IOERR_BADLENGTH;break;
        }
        g->dg_SectorSize=512;
        g->dg_TotalSectors=live(d)?d->shared->capacity:0;
        g->dg_Heads=1;
        g->dg_TrackSectors=32;
        g->dg_CylSectors=32;
        g->dg_Cylinders=(g->dg_TotalSectors+31u)/32u;
        g->dg_BufMemType=MEMF_PUBLIC;
        g->dg_DeviceType=DG_DIRECT_ACCESS;
        g->dg_Flags=DGF_REMOVABLE;
        g->dg_Reserved=0;
        io->io_Actual=sizeof(*g);
        break;
    }
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *q=(struct NSDeviceQueryResult*)io->io_Data;
        if(!q || io->io_Length<sizeof(*q)){
            io->io_Error=IOERR_BADLENGTH;break;
        }
        q->nsdqr_DevQueryFormat=0;
        q->nsdqr_SizeAvailable=sizeof(*q);
        q->nsdqr_DeviceType=NSDEVTYPE_TRACKDISK;
        q->nsdqr_DeviceSubType=0;
        q->nsdqr_SupportedCommands=(APTR)supported_commands;
        io->io_Actual=sizeof(*q);
        break;
    }
    case CMD_RESET:
    case CMD_CLEAR:
    case CMD_UPDATE:
    case TD_SEEK:
    case TD_SEEK64:
    case TD_MOTOR:
        io->io_Actual=0;break;
    default:
        io->io_Error=IOERR_NOCMD;break;
    }
    finish(io);
}

__asm__(".section .text\n.globl _endOfCode\n_endOfCode:\n");
