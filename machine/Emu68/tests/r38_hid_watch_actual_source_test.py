#!/usr/bin/env python3
"""Compile the REAL CPU1 HID polling functions with a mock USB host.
Asserts that an unclassified HID interface is polled even when no buttons
are pressed, recovers an idle input endpoint, and stops after disconnect.
"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
src=(root/"src/frf_usb/tinyusb/frf_tinyusb_host.c").read_text()
start=src.index("#define FRF_HID_WATCH_SLOTS 16u")
end=src.index("static volatile uint32_t frf_usb_reports",start)
actual=src[start:end]
header="""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
static uint32_t clock_ms;
static int ready[16][8],fail_submit[16][8],attempt[16][8];
uint32_t tusb_time_millis_api(void){return clock_ms;}
bool tuh_hid_receive_ready(uint8_t addr,uint8_t idx){
 return ready[addr][idx]!=0;
}
bool tuh_hid_receive_report(uint8_t addr,uint8_t idx){
 attempt[addr][idx]++;
 if(fail_submit[addr][idx]) return false;
 ready[addr][idx]=0;
 return true;
}
"""
test="""
int main(void){
 /* Unknown HID interface: PAD=0 must not disable interrupt polling. */
 frf_hid_watch_mount(4,0);
 assert(frf_hid_watch[0].addr==4);
 clock_ms=29; ready[4][0]=1; frf_hid_watch_retry();
 assert(attempt[4][0]==0);
 clock_ms=31; frf_hid_watch_retry();
 assert(attempt[4][0]==1);
 /* Busy endpoint: never double-submit */
 clock_ms=56; frf_hid_watch_retry();
 assert(attempt[4][0]==1);
 /* Failed USB submit: retry after 25 ms, without any fire button */
 clock_ms=81; ready[4][0]=1; fail_submit[4][0]=1;
 frf_hid_watch_retry();
 assert(attempt[4][0]==2 && frf_hid_retry_failed==1);
 clock_ms=106; fail_submit[4][0]=0; frf_hid_watch_retry();
 assert(attempt[4][0]==3 && frf_hid_retry_ok==2);
 /* Two HID interfaces on one composite device are independent. */
 frf_hid_watch_mount(4,2);
 ready[4][2]=1; clock_ms=140; frf_hid_watch_retry();
 assert(attempt[4][2]==1);
 /* Device removal must prevent ghost input and stop USB polling. */
 frf_hid_watch_unmount(4,0);
 ready[4][0]=1; clock_ms=200; frf_hid_watch_retry();
 assert(attempt[4][0]==3);
 frf_hid_watch_unmount(4,2);
 assert(frf_hid_retry_ok==3);
 puts("R38 REAL watchdog source PASS: unclaimed HID, idle retry, busy guard, failed submit, multi-interface, unmount");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="r38_hid_") as d:
    path=Path(d)/"watch.c"
    path.write_text(header+actual+test)
    compilers=[
        (["gcc"],[]),
        (["/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc"],["qemu-aarch64_be"])
    ]
    for cc,runner in compilers:
        exe=Path(d)/("watch_be" if runner else "watch_x86")
        cmd=cc+["-std=c17","-O2","-Wall","-Wextra","-Werror"]
        if runner:cmd+=["-static"]
        subprocess.run(cmd+["-o",str(exe),str(path)],check=True)
        subprocess.run(runner+[str(exe)],check=True)
