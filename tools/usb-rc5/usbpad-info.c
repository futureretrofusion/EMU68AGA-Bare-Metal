/* Read-only EMU68AGA USB gamepad telemetry from dedicated Z2 board.
   No device writes; works even when joystick is not recognized by games. */
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/expansion.h>
#include <libraries/configvars.h>
#include <stdio.h>
#include <stdint.h>
#include "../machine/Emu68/src/frf_usb/frf_pad_diag.h"
#define VENDOR 0x6d73u
#define PRODUCT 0x69u
int main(void) {
    struct Library *lib=OpenLibrary("expansion.library",36);
    if (!lib) { puts("Cannot open expansion.library");return 20; }
    struct ExpansionBase *ExpansionBase=(struct ExpansionBase*)lib;
    struct ConfigDev *cfg=FindConfigDev(0,VENDOR,PRODUCT);
    if (!cfg || !cfg->cd_BoardAddr) {
        puts("EMU68AGA Zorro-II USB storage board absent.");CloseLibrary(lib);return 10;
    }
    volatile struct frf_pad_diag *p=(volatile struct frf_pad_diag*)
      ((volatile uint8_t*)cfg->cd_BoardAddr+FRF_PAD_DIAG_OFF);
    if (p->magic!=FRF_PAD_DIAG_MAGIC) {
        puts("RC5 gamepad telemetry absent: not an RC5 kernel?");CloseLibrary(lib);return 5;
    }
    printf("Receivers=%lu  Reports OK=%lu Bad=%lu Fail=%lu\n",
        (unsigned long)p->receivers,(unsigned long)p->rx_ok,
        (unsigned long)p->rx_bad,(unsigned long)p->rx_fail);
    printf("Last USB report: length=%lu ID=%lu bytes=%08lx %08lx\n",
        (unsigned long)p->rx_len,(unsigned long)p->rx_id,
        (unsigned long)p->sample_a,(unsigned long)p->sample_b);
    printf("Decoded=%lu  Amiga pad callback=%lu\n",
        (unsigned long)p->reports_decoded,(unsigned long)p->updates);
    printf("buttons=%08lx hat=%ld axes=%08lx  JOY1DAT=%04lx fire=%lu\n",
        (unsigned long)p->pad_buttons,(long)p->pad_hat,
        (unsigned long)p->pad_axis01,
        (unsigned long)p->joy1dat,(unsigned long)p->fire);
    printf("USB storage: reads=%lu writes=%lu write_fail=%lu cmd=%lu LBA=%lu result=%ld\n",
        (unsigned long)p->storage_reads,(unsigned long)p->storage_writes,
        (unsigned long)p->storage_write_fail,(unsigned long)p->storage_last_cmd,
        (unsigned long)p->storage_last_lba,(long)p->storage_last_result);
    printf("Gamepad initial/retry submit failures=%lu recoveries=%lu\n",
           (unsigned long)p->pad_submit_deferred,
           (unsigned long)p->pad_submit_recovered);
    CloseLibrary(lib);return 0;
}
