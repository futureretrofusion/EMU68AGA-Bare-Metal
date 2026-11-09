/* USB0 read-only partition/driver probe, v0.10.50.
 * Never writes a sector or any Amiga volume. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static unsigned char sec[512] __attribute__((aligned(16)));
static uint32_t le32(const unsigned char *s)
{
    return (uint32_t)s[0]|((uint32_t)s[1]<<8)|((uint32_t)s[2]<<16)|((uint32_t)s[3]<<24);
}
static uint16_t le16(const unsigned char *s)
{
    return (uint16_t)((uint16_t)s[0]|((uint16_t)s[1]<<8));
}
static int fatpart(unsigned char type)
{
    switch(type) {
      case 1:case 4:case 6:case 11:case 12:case 14: return 1;
      default:return 0;
    }
}
int main(void)
{
    struct MsgPort *port=CreateMsgPort();
    if (!port) {puts("Cannot create reply port");return 20;}
    struct IOStdReq *io=(struct IOStdReq*)CreateIORequest(port,sizeof(struct IOExtTD));
    if (!io) {DeleteMsgPort(port);return 20;}
    LONG rc=OpenDevice("usbmass.device",0,(struct IORequest*)io,0);
    if (rc) {
        printf("OpenDevice(usbmass.device,0): FAILED error=%ld\n",(long)rc);
        puts("Check DEVS:usbmass.device v0.3 and matching Pi3 ARM kernel.");
        DeleteIORequest((struct IORequest*)io);DeleteMsgPort(port);return 10;
    }
    struct DriveGeometry g;
    memset(&g,0,sizeof(g));
    io->io_Command=TD_GETGEOMETRY;io->io_Data=&g;io->io_Length=sizeof g;
    io->io_Offset=0;io->io_Actual=0;
    DoIO((struct IORequest*)io);
    int geomerr=io->io_Error;
    unsigned long blocks=(unsigned long)g.dg_TotalSectors;
    printf("TD_GETGEOMETRY: error=%d sectors=%lu sectorBytes=%lu\n",
           geomerr,blocks,(unsigned long)g.dg_SectorSize);
    io->io_Command=TD_PROTSTATUS;io->io_Data=0;io->io_Length=0;io->io_Offset=0;
    DoIO((struct IORequest*)io);
    int prot=io->io_Error;
    unsigned long writeprot=(unsigned long)io->io_Actual;
    printf("TD_PROTSTATUS: error=%d write-protected=%lu (0=not protected)\n",
           prot,writeprot);
    io->io_Command=CMD_READ;io->io_Data=sec;io->io_Length=512;
    io->io_Offset=0;io->io_Actual=0;
    DoIO((struct IORequest*)io);
    int rderr=io->io_Error;
    unsigned long bytes=(unsigned long)io->io_Actual;
    printf("CMD_READ sector0: error=%d bytes=%lu\n",rderr,bytes);
    CloseDevice((struct IORequest*)io);
    DeleteIORequest((struct IORequest*)io);DeleteMsgPort(port);
    if(geomerr || rderr || bytes!=512 || blocks<1 ||
       g.dg_SectorSize!=512 || sec[510]!=0x55 || sec[511]!=0xaa) {
        puts("Cannot derive mount geometry. No output proposed.");
        return 5;
    }
    uint32_t start=0,len=0;
    unsigned type=0;
    if((sec[0]==0xeb || sec[0]==0xe9) && le16(sec+11)==512) {
        start=0;len=(uint32_t)blocks;
        puts("Found directly formatted FAT volume (superfloppy layout).");
    } else {
        for(unsigned i=0;i<4;i++) {
            const unsigned char *e=sec+446+i*16;
            if(e[4]==0xEE) {
                puts("Protective GPT partition detected; FAT95 legacy MBR mount not proposed.");
                return 10;
            }
            if(fatpart(e[4]) && !len) {
                type=e[4];
                start=le32(e+8);
                len=le32(e+12);
            }
        }
        if(!len) {
            puts("No FAT partition found in primary MBR entries.");
            return 5;
        }
    }
    if((uint64_t)start>=blocks || (uint64_t)len>blocks-start ||
       (uint64_t)start+len>0x7fffffffu || !len) {
        puts("Partition geometry outside usable capacity. STOP: do not mount RW.");
        return 5;
    }
    printf("FAT partition type=%02lx start LBA=%lu sector count=%lu\n",
           (unsigned long)type,(unsigned long)start,(unsigned long)len);
    printf("For FAT95 automatic MBR partition detection (FAT\\1 / DosType 0x46415401):\n");
    puts("  FileSystem     = L:fat95");
    puts("  Device         = usbmass.device");
    puts("  Unit           = 0");
    puts("  Flags          = 0");
    puts("  LowCyl         = 0");
    puts("  HighCyl        = 1");
    puts("  Surfaces       = 1");
    puts("  BlocksPerTrack = 1");
    puts("  BlockSize      = 512");
    puts("  Buffers        = 80");
    puts("  BufMemType     = 1");
    puts("  MaxTransfer    = 0x2000");
    puts("  Mask           = 0xFFFFFFFE");
    puts("  StackSize      = 8192");
    puts("  Priority       = 5");
    puts("  GlobVec        = -1");
    puts("  DosType        = 0x46415401");
    puts("  Activate       = 1");
    puts("  Mount          = 1");
    puts("READ-ONLY DIAGNOSTIC COMPLETE: nothing was written.");
    if(prot || writeprot)
        puts("WARNING: driver reports write protection; check active device version.");
    return 0;
}
