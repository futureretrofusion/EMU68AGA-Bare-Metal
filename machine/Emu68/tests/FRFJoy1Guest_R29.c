#include <stdio.h>
#include <stdint.h>
#include <proto/dos.h>
int main(void){
volatile uint16_t *joy=(void*)0xDFF00C;
volatile uint8_t *cia=(void*)0xBFE001;
uint16_t prev=0xffff;int pf=-1;
puts("Read JOY1DAT and CIAA while operating gamepad");
for(unsigned i=0;i<300;i++){
uint16_t v=*joy;uint8_t c=*cia;int f=!(c&0x80);
if(v!=prev || f!=pf || i%25==0)printf("n=%03u JOY=%04X CIA=%02X FIRE=%d\n",i,v,c,f);
prev=v;pf=f;Delay(3);}
return 0;}
