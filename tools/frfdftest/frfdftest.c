/* FRFDFTEST: inspect Kickstart disk.resource and trackdisk.device.
 * Read-only. GPL-2.0-only.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>
#include <resources/disk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <string.h>

static void query(struct IOExtTD *io,UWORD cmd,const char *label)
{
    io->iotd_Req.io_Command=cmd;
    io->iotd_Req.io_Flags=0;
    io->iotd_Req.io_Data=NULL;
    io->iotd_Req.io_Length=0;
    io->iotd_Req.io_Offset=0;
    (void)DoIO((struct IORequest *)io);
    Printf((CONST_STRPTR)"  %s: error=%ld actual=%lu\n",
           (STRPTR)label,(LONG)io->iotd_Req.io_Error,
           (ULONG)io->iotd_Req.io_Actual);
}
int main(int argc,char **argv)
{
    struct DiscResource *dr;
    struct MsgPort *port;
    struct IOExtTD *io;
    LONG first=0,last=3;
    int do_read=0;
    if(argc>=2) {
        if(argv[1][0]<'0'||argv[1][0]>'3'||argv[1][1]!=0) {
            Printf((CONST_STRPTR)"Usage: FRFDFTEST [0|1|2|3] [READ]\n");
            return 10;
        }
        first=last=argv[1][0]-'0';
    }
    if(argc>2) {
        if(stricmp(argv[2],"READ")!=0) {
            Printf((CONST_STRPTR)"Usage: FRFDFTEST [0|1|2|3] [READ]\n");
            return 10;
        }
        do_read=1;
    }
    dr=(struct DiscResource *)OpenResource((CONST_STRPTR)DISKNAME);
    if(dr) {
        Printf((CONST_STRPTR)"disk.resource cached IDs (not a fresh probe):\n");
        for(int u=0;u<4;u++)
            Printf((CONST_STRPTR)"  DF%ld: $%08lx\n",
                   (LONG)u,(ULONG)dr->dr_UnitID[u]);
        Printf((CONST_STRPTR)"  Expected DD ID: 00000000; absent: FFFFFFFF\n");
    } else Printf((CONST_STRPTR)"disk.resource: NOT FOUND\n");
    port=CreateMsgPort();
    if(!port) {
        Printf((CONST_STRPTR)"CreateMsgPort failed\n"); return 20;
    }
    io=(struct IOExtTD *)CreateIORequest(port,sizeof(struct IOExtTD));
    if(!io) {
        Printf((CONST_STRPTR)"CreateIORequest failed\n");
        DeleteMsgPort(port);return 20;
    }
    for(LONG u=first;u<=last;u++) {
        LONG rc=OpenDevice((CONST_STRPTR)TD_NAME,(ULONG)u,
                           (struct IORequest *)io,0);
        Printf((CONST_STRPTR)"DF%ld: OpenDevice(trackdisk.device,%ld)=%ld\n",
               u,u,rc);
        if(rc!=0)continue;
        query(io,TD_GETDRIVETYPE,"TD_GETDRIVETYPE");
        query(io,TD_GETNUMTRACKS,"TD_GETNUMTRACKS");
        query(io,TD_CHANGESTATE,"TD_CHANGESTATE (0=inserted)");
        LONG present=io->iotd_Req.io_Error==0 &&
                     io->iotd_Req.io_Actual==0;
        query(io,TD_CHANGENUM,"TD_CHANGENUM");
        if(do_read && present) {
            UBYTE *boot=(UBYTE *)AllocMem(512,MEMF_CHIP|MEMF_CLEAR);
            if(boot) {
                io->iotd_Req.io_Command=CMD_READ;
                io->iotd_Req.io_Flags=0;
                io->iotd_Req.io_Data=boot;
                io->iotd_Req.io_Length=512;
                io->iotd_Req.io_Offset=0;
                (void)DoIO((struct IORequest *)io);
                Printf((CONST_STRPTR)"  CMD_READ sector0: error=%ld bytes=%lu\n",
                       (LONG)io->iotd_Req.io_Error,
                       (ULONG)io->iotd_Req.io_Actual);
                if(io->iotd_Req.io_Error==0)
                    Printf((CONST_STRPTR)"  Bootblock: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
                         (ULONG)boot[0],(ULONG)boot[1],(ULONG)boot[2],
                         (ULONG)boot[3],(ULONG)boot[4],(ULONG)boot[5],
                         (ULONG)boot[6],(ULONG)boot[7]);
                FreeMem(boot,512);
            } else Printf((CONST_STRPTR)"  MEMF_CHIP buffer unavailable\n");
        }
        CloseDevice((struct IORequest *)io);
    }
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return 0;
}
