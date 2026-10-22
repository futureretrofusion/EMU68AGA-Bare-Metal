/* R22 native AmigaOS 3.x USB -> joystick link monitor.
 * Shell executable: FindConfigDev 6D73:69; read only existing Zorro-II
 * 64KB board at +0x3400. No writes to hardware or mass-storage mailbox.
 */
#include <stdio.h>
#include <stdint.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <libraries/expansion.h>
#include "../src/frf_usb/frf_r22_input_diag.h"

struct ExpansionBase *ExpansionBase;

static uint32_t read32(volatile const uint32_t *p) {return *p;}
int main(void)
{
    printf("EMU68AGA R35 220D:330C ID01/02/03 pad monitor - Zorro-II board\n");
    ExpansionBase=(struct ExpansionBase *)OpenLibrary("expansion.library",37);
    if(!ExpansionBase){puts("expansion.library unavailable");return 10;}
    struct ConfigDev *cd=FindConfigDev(NULL,0x6d73,0x69);
    if(!cd) {
        puts("FRUM USB board 6D73:69 not found in expansion list.");
        puts("Use FRFJoyDiag to test guest JOY1DAT directly.");
        CloseLibrary((struct Library *)ExpansionBase);return 20;
    }
    volatile const struct frf_r22_diag *d=
        (volatile const struct frf_r22_diag *)
        ((volatile const uint8_t*)cd->cd_BoardAddr+FRF_R22_DIAG_OFFSET);
    printf("FRUM Zorro board at %08lX; diag at +%04X\n",
           (unsigned long)(uintptr_t)cd->cd_BoardAddr,FRF_R22_DIAG_OFFSET);
    uint32_t old_rx=0,old_map=0,old_joy=0xffffffffu,last_tick=0;
    for(unsigned i=0;i<90u;++i) {
        const uint32_t magic=read32(&d->magic);
        if(magic!=FRF_R22_DIAG_MAGIC) {
            if(i%5u==0u) printf("No live R22 PAD2 record yet (magic=%08lX)\n",(unsigned long)magic);
            Delay(10);
            continue;
        }
        const uint32_t seq1=read32(&d->sequence);
        if(seq1&1u){Delay(2);continue;}
        const uint32_t tick=read32(&d->tick_ms);
        const uint32_t host=read32(&d->host_ticks);
        const uint32_t pad=read32(&d->pad_count);
        const uint32_t device=read32(&d->receiver_addr);
        const uint32_t rx=read32(&d->receiver_hid_rx);
        const uint32_t len=read32(&d->receiver_len);
        const uint32_t raw0=read32(&d->receiver_word0);
        const uint32_t raw1=read32(&d->receiver_word1);
        const uint32_t total=read32(&d->hid_rx_total);
        const uint32_t map=read32(&d->compat_valid);
        const uint32_t matched=read32(&d->compat_rx);
        const uint32_t bad=read32(&d->compat_rejected);
        const uint32_t state=read32(&d->last_joy_fire);
        const uint32_t writes=read32(&d->host_joy_writes);
        const uint32_t guest=read32(&d->guest_joy_reads);
        const uint32_t cia=read32(&d->guest_cia_reads);
        const uint32_t kbd_ok=read32(&d->kbd_boot_ok);
        const uint32_t kbd_blocked=read32(&d->kbd_receiver_blocked);
        const uint32_t kbd_bad=read32(&d->kbd_bad_length);
        const uint32_t kbd_held=read32(&d->kbd_held);
        const uint32_t kbd_keys=read32(&d->kbd_transitions);
        const uint32_t kbd_q=read32(&d->kbd_cia_queue);
        const uint32_t kbd_drop=read32(&d->kbd_cia_drops);
        const uint32_t kbd_ack=read32(&d->kbd_cia_timeouts);
        uint32_t full[6]; for(unsigned k=0;k<6u;k++) full[k]=read32(&d->raw24[k]);
        uint32_t poll[4]; for(unsigned k=0;k<4u;k++)poll[k]=read32(&d->poll_stats[k]);
        uint32_t ctrl[4];for(unsigned k=0;k<4u;k++)ctrl[k]=read32(&d->ctrl_stats[k]);
        uint32_t ids[2]={0};uint32_t rawids[2][7]={{0}};
        if(read32(&d->version)>=6u){
            ids[0]=read32(&d->id01_count);ids[1]=read32(&d->id03_count);
            for(unsigned k=0;k<7u;k++){rawids[0][k]=read32(&d->id01_raw[k]);rawids[1][k]=read32(&d->id03_raw[k]);}
        }
        uint32_t id02=0,raw02[7]={0};
        if(read32(&d->version)>=7u){
           id02=read32(&d->id02_count);
           for(unsigned k=0;k<7u;k++)raw02[k]=read32(&d->id02_raw[k]);
        }
        const uint32_t seq2=read32(&d->sequence);
        if(seq1!=seq2 || (seq2&1u)){Delay(2);continue;}
        if(i==0) puts("Press Dyonder D-pad / stick / A: values update every ~0.5 sec");
        printf("t=%5lu host=%8lu PAD=%lu dev=%04lX dRX=%6lu(+%lu) len=%lu HID=%lu ",
            (unsigned long)tick,(unsigned long)host,(unsigned long)pad,
            (unsigned long)device,(unsigned long)rx,(unsigned long)(rx-old_rx),
            (unsigned long)len,(unsigned long)total);
        printf("OK=%6lu(+%lu) claimRX=%lu reject=%lu\n",
            (unsigned long)map,(unsigned long)(map-old_map),
            (unsigned long)matched,(unsigned long)bad);
        printf("    raw=%08lX %08lX JOY1=%04lX FIRE=%lu W=%lu GJOYRD=%lu CIARD=%lu\n",
            (unsigned long)raw0,(unsigned long)raw1,
            (unsigned long)(state&65535u),(unsigned long)((state>>16)&1u),
            (unsigned long)writes,(unsigned long)guest,(unsigned long)cia);
        printf("    R23 KBD boot=%lu blockedPad=%lu badLen=%lu keys=%lu held=%lu queue=%lu drops=%lu ackTO=%lu\n",
             (unsigned long)kbd_ok,(unsigned long)kbd_blocked,
             (unsigned long)kbd_bad,(unsigned long)kbd_keys,
             (unsigned long)kbd_held,(unsigned long)kbd_q,
             (unsigned long)kbd_drop,(unsigned long)kbd_ack);
        if(read32(&d->version)>=3u) printf("    R24 DATA %08lX %08lX %08lX %08lX %08lX %08lX\n",
            (unsigned long)full[0],(unsigned long)full[1],(unsigned long)full[2],
            (unsigned long)full[3],(unsigned long)full[4],(unsigned long)full[5]);
        if(read32(&d->version)>=4u)printf("R25 POLL notReady=%lu submitOK=%lu submitFail=%lu ticks=%lu\n",(unsigned long)poll[0],(unsigned long)poll[1],(unsigned long)poll[2],(unsigned long)poll[3]);
        if(read32(&d->version)>=5u)printf("R26 GET tries=%lu OK=%lu ERR=%lu pending=%lu\n",(unsigned long)ctrl[0],(unsigned long)ctrl[1],(unsigned long)ctrl[2],(unsigned long)ctrl[3]);
        if(read32(&d->version)>=6u){
            printf("R29 ID01=%lu ID03=%lu\n",(unsigned long)ids[0],(unsigned long)ids[1]);
            for(unsigned k=0;k<2u;k++)printf("ID0%u %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",k?3u:1u,(unsigned long)rawids[k][0],(unsigned long)rawids[k][1],(unsigned long)rawids[k][2],(unsigned long)rawids[k][3],(unsigned long)rawids[k][4],(unsigned long)rawids[k][5],(unsigned long)rawids[k][6]);
        }
        if(read32(&d->version)>=7u){
           printf("R33 ID02=%lu DATA %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
              (unsigned long)id02,(unsigned long)raw02[0],(unsigned long)raw02[1],(unsigned long)raw02[2],(unsigned long)raw02[3],(unsigned long)raw02[4],(unsigned long)raw02[5],(unsigned long)raw02[6]);
        }
        if(tick==last_tick && i>1u) puts("    WARNING: CPU1 telemetry clock has not advanced!");
        last_tick=tick;old_rx=rx;old_map=map;old_joy=state;
        Delay(25u);
    }
    (void)old_joy;
    CloseLibrary((struct Library *)ExpansionBase);
    puts("R35 test complete.");
    return 0;
}
