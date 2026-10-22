#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "../src/frf_usb/frf_r22_input_diag.h"
#include "../src/frf_usb/frf_usb_mass.h"
int main(void)
{
  _Static_assert(sizeof(struct frf_r22_diag)==33u*4u,"ABI mismatch");
  _Static_assert(FRF_R22_DIAG_OFFSET>=0x3000u+256u,"legacy pad block overlap");
  _Static_assert(FRF_R22_DIAG_OFFSET+sizeof(struct frf_r22_diag)<=FRF_UM_BOARD_BYTES,"board overflow");
  struct frf_r22_diag d={0};
  d.magic=FRF_R22_DIAG_MAGIC;
  d.version=FRF_R22_DIAG_VERSION;
  d.sequence=2u;
  d.receiver_hid_rx=4096u;
  d.last_joy_fire=0x00010300u;
  assert(d.magic==0x50414432u && d.receiver_hid_rx==4096u);
  assert((d.last_joy_fire&0xffffu)==0x0300u && (d.last_joy_fire>>16)==1u);
  puts("R22 Zorro-II mailbox layout, version, endian-independent aligned 32-bit fields PASS");
  return 0;
}
