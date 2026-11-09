/* FRFADF: load AmigaDOS ADF files into standalone EMU68AGA's
 * virtual DF0/DF1 drives or its four OSD swap slots.
 * Original Future Retro Fusion glue to the AGA-Storm FRF $ADxx protocol.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

#define FRF_ADF_CONTROL  (*(volatile UWORD *)0xDFF1F2)
#define FRF_ADF_SIZEFLAG (*(volatile UWORD *)0xDFF1F0)
#define FRF_ADF_POINTER_HI (*(volatile UWORD *)0xDFF1EC)
#define FRF_ADF_POINTER_LO (*(volatile UWORD *)0xDFF1EE)
#define ADF_DD 901120L
#define ADF_HD 1802240L

static int command(const char *kind,int *code)
{
    if(!kind || !stricmp(kind,"SLOT1")){*code=0xC1;return 1;}
    if(!stricmp(kind,"SLOT2")){*code=0xC2;return 1;}
    if(!stricmp(kind,"SLOT3")){*code=0xC3;return 1;}
    if(!stricmp(kind,"SLOT4")){*code=0xC4;return 1;}
    if(!stricmp(kind,"DF0")){*code=0xF0;return 1;}
    if(!stricmp(kind,"DF1")){*code=0xF1;return 1;}
    return 0;
}
static int supported(void)
{
    return (FRF_ADF_CONTROL & 0xFF00u)==0xAD00u;
}
int main(int argc,char **argv)
{
    BPTR fh=0;
    APTR data=0;
    LONG len=0,bytes=0;
    int op;
    UWORD result;
    ULONG addr;
    if(argc==3 && !stricmp(argv[1],"EJECT")) {
        if(!stricmp(argv[2],"DF0"))op=0xE0;
        else if(!stricmp(argv[2],"DF1"))op=0xE1;
        else {Printf((CONST_STRPTR)"Usage: FRFADF EJECT DF0|DF1\n");return 10;}
        if(!supported()){Printf((CONST_STRPTR)"FRF virtual floppy interface unavailable\n");return 20;}
        FRF_ADF_CONTROL=(UWORD)(0xAD00u|(UWORD)op);
        result=FRF_ADF_CONTROL;
        Printf((CONST_STRPTR)"FRFADF eject: status $%04lx\n",(ULONG)result);
        return result==0xAD82u ? 0 : 20;
    }
    if(argc<2 || argc>3 || !command(argc==3 ? argv[2] : 0,&op)) {
        Printf((CONST_STRPTR)"FRFADF file.adf [SLOT1|SLOT2|SLOT3|SLOT4|DF0|DF1]\n");
        Printf((CONST_STRPTR)"FRFADF EJECT DF0|DF1\n");
        return 10;
    }
    if(!supported()){Printf((CONST_STRPTR)"FRF virtual floppy interface unavailable\n");return 20;}
    fh=Open((STRPTR)argv[1],MODE_OLDFILE);
    if(!fh){Printf((CONST_STRPTR)"Cannot open ADF: %s\n",argv[1]);return 20;}
    if(Seek(fh,0L,OFFSET_END)==-1L ||
       (len=Seek(fh,0L,OFFSET_BEGINNING))==-1L ||
       (len!=ADF_DD && len!=ADF_HD)) {
        Printf((CONST_STRPTR)"ADF must be DD 901120 or HD 1802240 bytes\n");
        Close(fh);return 20;
    }
    data=AllocMem((ULONG)len,MEMF_PUBLIC|MEMF_FAST);
    if(!data){Printf((CONST_STRPTR)"Not enough Fast RAM for ADF\n");Close(fh);return 20;}
    bytes=Read(fh,data,len);
    Close(fh);fh=0;
    if(bytes!=len) {
        Printf((CONST_STRPTR)"Incomplete ADF read: %ld of %ld bytes\n",bytes,len);
        FreeMem(data,(ULONG)len);return 20;
    }
    addr=(ULONG)data;
    if(addr<0x200000UL) {
        Printf((CONST_STRPTR)"FRFADF needs identity-mapped Fast RAM above 2 MB\n");
        FreeMem(data,(ULONG)len);return 20;
    }
    FRF_ADF_POINTER_HI=(UWORD)(addr>>16);
    FRF_ADF_POINTER_LO=(UWORD)(addr&0xffffu);
    FRF_ADF_SIZEFLAG=(UWORD)(((ULONG)len>>10)|0x1000u);
    FRF_ADF_CONTROL=(UWORD)(0xAD00u|(UWORD)op);
    result=FRF_ADF_CONTROL;
    /* The bare-metal chipset copies the entire image synchronously before
     * returning, so guest-side Fast RAM may now be released. */
    FreeMem(data,(ULONG)len);
    Printf((CONST_STRPTR)"FRFADF %s -> %s (%ld bytes), status $%04lx\n",
           argv[1],argc==3?argv[2]:"SLOT1",len,(ULONG)result);
    return (result==0xAD83u || result==0xAD81u) ? 0 : 20;
}
