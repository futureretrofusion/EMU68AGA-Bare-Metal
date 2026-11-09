/* SPDX-License-Identifier: GPL-2.0-or-later
 * UA700CAMD - bridges EMU68AGA native TinyUSB Edirol MIDI into Amiga CAMD.
 * Works with virtual FRUM v1 board (manufacturer 0x6d73, product 0x69).
 * Uses the kernel's independent UAM1 mailbox at board + 0x4000.
 * Runs as a normal AmigaDOS process; Ctrl-C to terminate.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <libraries/configvars.h>
#include <midi/camd.h>
#include <midi/mididefs.h>
#include <inline/camd.h>
#include <utility/tagitem.h>
#include <stdio.h>
#include <stdint.h>
struct Library *CamdBase;
#define MAGIC 0x55414d31UL
#define BOARD_OFF 0x4000u
#define ENTRIES 128u
static unsigned msglen(unsigned cin) {
    switch(cin & 15u) {
      case 0x2: case 0x6: case 0xC: case 0xD:return 2;
      case 0x3: case 0x4: case 0x7: case 0x8: case 0x9:
      case 0xA: case 0xB: case 0xE:return 3;
      case 0x5: case 0xF:return 1;
      default:return 0;
    }
}
int main(int argc,char **argv)
{
    (void)argc;(void)argv;
    struct Library *exp=OpenLibrary("expansion.library",36);
    if(!exp){puts("UA700CAMD: expansion.library missing");return 20;}
    struct ExpansionBase *ExpansionBase=(struct ExpansionBase*)exp;
    struct ConfigDev *cfg=FindConfigDev(NULL,0x6d73,0x69);
    if(!cfg || !cfg->cd_BoardAddr){
        puts("UA700CAMD: FRUM USB virtual board not found");
        CloseLibrary(exp);return 20;
    }
    volatile uint32_t *m=(volatile uint32_t*)((volatile uint8_t*)cfg->cd_BoardAddr+BOARD_OFF);
    if(m[0]!=MAGIC || m[1]!=1u){
        printf("UA700CAMD: kernel MIDI mailbox not found (%08lx); install matching kernel\\n",(unsigned long)m[0]);
        CloseLibrary(exp);return 20;
    }
    CamdBase=OpenLibrary("camd.library",1);
    if(!CamdBase){puts("UA700CAMD: camd.library missing");CloseLibrary(exp);return 20;}
    struct TagItem nt[]={
        {MIDI_Name,(ULONG)"EMU68AGA UA-700"},
        {MIDI_ClientType,CCType_ToneGenerator},
        {TAG_DONE,0}
    };
    struct MidiNode *node=CreateMidiA(nt);
    if(!node){puts("UA700CAMD: CreateMidi failed");goto end;}
    /* Matches the cluster preferred by existing AmigaDX7 and Poly800RGB. */
    struct TagItem lt[]={
        {MLINK_Location,(ULONG)"poseidonusb.in.0"},
        {MLINK_Name,(ULONG)"EMU68AGA UA-700 MIDI IN"},
        {MLINK_Comment,(ULONG)"EMU68AGA native Edirol USB MIDI"},
        {TAG_DONE,0}
    };
    struct MidiLink *link=AddMidiLinkA(node,MLTYPE_Sender,lt);
    if(!link){puts("UA700CAMD: cannot create sender link (check cluster collision)");DeleteMidi(node);goto end;}
    puts("UA700CAMD: MIDI IN running on poseidonusb.in.0. Ctrl-C exits.");
    unsigned long count=0,discard=0,control=0,last_dev=0xffffffffu;
    uint32_t last_prod=0;
    for(;;){
        if(SetSignal(0,0)&SIGBREAKF_CTRL_C)break;
        uint32_t dev=m[4];
        if(dev!=last_dev){printf("UA700CAMD: USB device address %lu\\n",(unsigned long)dev);last_dev=dev;}
        uint32_t prod=m[2],cons=m[3],num=0;
        /* Don't accept stale packets on launch; catch up instead. */
        if(prod-cons>ENTRIES){m[3]=prod;discard++;continue;}
        while(cons!=prod && num++<ENTRIES){
            uint32_t data=m[16u+(cons&(ENTRIES-1u))];
            uint8_t cable=(uint8_t)(data>>28);
            uint8_t cin=(uint8_t)(data>>24)&15u;
            uint8_t bytes[3]={(uint8_t)(data>>16),(uint8_t)(data>>8),(uint8_t)data};
            unsigned n=msglen(cin);
            /* Some Roland advanced implementations use alternate packet
             * framing. Reject implausible data instead of forging notes. */
            if(cable==1u){control++;cons++;m[3]=cons;continue;}
            if(cable!=0u){discard++;cons++;m[3]=cons;continue;}
            if(n && bytes[0]>=0x80u && bytes[0]<0xf8u){
                ParseMidi(link,bytes,n);count++;
                if(count<12u || (count%64u)==0u)
                    printf("MIDI %lu: %02x %02x %02x\\n",count,(unsigned)bytes[0],(unsigned)bytes[1],(unsigned)bytes[2]);
            }else discard++;
            cons++;
            m[3]=cons;
        }
        if(prod==last_prod)Delay(1);
        last_prod=prod;
    }
    printf("UA700CAMD: MIDI port events=%lu control=%lu discarded=%lu errors=%lu\n",count,control,discard,(unsigned long)m[5]);
    RemoveMidiLink(link);
    DeleteMidi(node);
end:
    CloseLibrary(CamdBase);
    CloseLibrary(exp);
    return 0;
}
