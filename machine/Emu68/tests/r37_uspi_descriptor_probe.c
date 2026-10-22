#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "frf_r19_hid_descriptor.h"
int main(int argc,char**argv) {
 if(argc!=2)return 2;
 FILE*f=fopen(argv[1],"r");
 if(!f)return 3;
 uint8_t d[1024];unsigned n=0,u;
 while(n<1024 && fscanf(f,"%x",&u)==1)d[n++]=(uint8_t)u;
 fclose(f);struct frf_hid_map m={0};
 int ok=frf_hid_gamepad_parse(&m,d,(uint16_t)n);
 printf("descriptor bytes=%u mapped=%d fields=%u ID=%u report1bits=%u\n",n,ok,m.count,m.first_id,m.report_bits[1]);
 assert(ok && n==191u && m.count==19u && m.report_bits[1]==72u);
 uint8_t b[10]={1,0,0,8,128,128,128,128,0,0};
 uint16_t joy=0;int fire=0;uint32_t buttons=0,present=0;
 int good=frf_hid_gamepad_decode(&m,b,10,&joy,&fire);
 frf_hid_gamepad_decode_buttons(&m,b,10,&buttons,&present);
 assert(good && joy==0 && !fire && !buttons && present==0x7fffu);
 b[1]=1;
 good=frf_hid_gamepad_decode(&m,b,10,&joy,&fire);
 frf_hid_gamepad_decode_buttons(&m,b,10,&buttons,&present);
 assert(good && joy==0 && fire && buttons==1u);
 b[1]=0;b[3]=0;
 good=frf_hid_gamepad_decode(&m,b,10,&joy,&fire);
 assert(good && joy==0x0100u && !fire);
 puts("PASS USPi 191-byte descriptor: 15 independent buttons, hat, four axes, primary fire and D-pad");
 return 0;
}
