#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "common/tusb_types.h"
#include "common/tusb_compiler.h"
int main(void){
  uint8_t raw[18]={18,1,0x00,0x02,0,0,0,64,0x7e,0x05,0x09,0x20,0,1,1,2,3,1};
  tusb_desc_device_t d; memcpy(&d,raw,sizeof d);
  const uint16_t vid=tu_le16toh(d.idVendor),pid=tu_le16toh(d.idProduct);
  printf("raw=%02X %02X %02X %02X direct=%04X:%04X host=%04X:%04X\n",
    raw[8],raw[9],raw[10],raw[11],d.idVendor,d.idProduct,vid,pid);
  if(vid!=0x057eu || pid!=0x2009u)return 1;
  return 0;
}