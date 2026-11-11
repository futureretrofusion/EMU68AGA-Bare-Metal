/* FRF native HID gamepad descriptor -> hardware-shaped Amiga joystick.
 * No Poseidon/AmigaOS dependencies. One HID report ID selected per device.
 * Defensive bounds throughout; software tests exercise multiple layouts.
 */
#include <stdint.h>
#include "frf_native_usb_input.h"
enum { HID_PAGE_DESKTOP=1, HID_PAGE_BUTTON=9, HID_USAGE_JOYSTICK=4,
       HID_USAGE_GAMEPAD=5, HID_USAGE_X=0x30, HID_USAGE_Y=0x31,
       HID_USAGE_Z=0x32,HID_USAGE_RX=0x33,HID_USAGE_RY=0x34,
       HID_USAGE_RZ=0x35,HID_USAGE_HAT=0x39 };
struct Field { unsigned bit,size; int32_t lo,hi; unsigned valid; };
struct Pad {
  unsigned valid,id,has_id;
  unsigned button_offset,button_count,button_bits;
  unsigned shanwan,switch_pro;
  struct Field x,y,extra_x,extra_y,hat;
};
static struct Pad pads[4];
static uint32_t pad_vid[4],pad_pid[4];

void frf_native_usb_pad_set_identity_for(unsigned index,uint32_t vid,uint32_t pid)
{
  if(index>=4u)return;
  pad_vid[index]=vid;pad_pid[index]=pid;
  pads[index].valid=0u;
}
int frf_native_usb_pad_enable_switchpro_for(unsigned index)
{
  if(index>=4u || pad_vid[index]!=0x057eu || pad_pid[index]!=0x2009u)
    return 0;
  pads[index]=(struct Pad){0};
  pads[index].valid=1u;
  pads[index].switch_pro=1u;
  /* The 057e:2009 Pro Controller has several report IDs and a
     proprietary HID layout. Standard descriptor parsing deliberately
     rejects it; a per-VID:PID decoder is needed. */
  return 1;
}
int frf_native_usb_pad_enable_shanwan_for(unsigned index)
{
  if(index>=4u || pad_vid[index]!=0x2563u || pad_pid[index]!=0x0576u)return 0;
  pads[index]=(struct Pad){0};
  pads[index].valid=1u;
  pads[index].shanwan=1u;
  pads[index].has_id=1u;
  pads[index].id=1u;
  return 1;
}

static int32_t sign_extend(uint32_t v,unsigned bits)
{
  if(!bits || bits>32u) return 0;
  if(bits==32u) return (int32_t)v;
  uint32_t mask=(1u<<bits)-1u,sign=1u<<(bits-1u);
  v &= mask;
  return (int32_t)((v^sign)-sign);
}
static uint32_t bits_get(const uint8_t *bytes,unsigned len,unsigned off,unsigned size)
{
  if(!bytes || size>32u || !size || (uint64_t)off+size>(uint64_t)len*8u)
    return 0;
  uint32_t v=0;
  for(unsigned i=0;i<size;i++)
    if(bytes[(off+i)>>3u]&(1u<<((off+i)&7u))) v|=1u<<i;
  return v;
}
static int32_t field_get(const struct Field *f,const uint8_t *bytes,unsigned len)
{
  uint32_t v=bits_get(bytes,len,f->bit,f->size);
  return f->lo<0 ? sign_extend(v,f->size):(int32_t)v;
}
static int scale_axis(const struct Field *f,const uint8_t *bytes,unsigned len)
{
  if(!f->valid || f->hi<=f->lo) return 0;
  int32_t value=field_get(f,bytes,len);
  /* Arithmetic must remain 64-bit before subtraction: HID logical ranges
   * can legitimately span INT32_MIN..INT32_MAX. */
  int64_t range=(int64_t)f->hi-(int64_t)f->lo;
  int64_t x=((int64_t)value-(int64_t)f->lo)*254/range-127;
  if(x>127)x=127;
  if(x<-127)x=-127;
  return (int)x;
}
int frf_native_usb_pad_set_descriptor_for(unsigned index,const uint8_t *b,unsigned len)
{
  struct Pad out={0};
  if(index>=4u || !b || len<8u || len>2048u) return 0;
  /* Pi3 compatibility uses VID:PID-selected 10B Report ID1 layout.
     Report ID5 is a different HID mouse and must never be mapped to JOY1. */
  if(pad_vid[index]==0x2563u && pad_pid[index]==0x0576u)
    return frf_native_usb_pad_enable_shanwan_for(index);
  uint16_t offsets[256]={0};
  unsigned page=0,rsize=0,rcount=0,rid=0;
  int32_t lo=0,hi=255;
  unsigned usages[32]={0},u_count=0,umin=0,umax=0;
  unsigned depth=0,active_level=0,selected=0;
  for(unsigned at=0;at<len;) {
    unsigned prefix=b[at++];
    if(prefix==0xfeu) {
      if(at+2u>len) return 0;
      unsigned n=b[at];at+=2u;
      if(n>len-at)return 0;
      at+=n;continue;
    }
    unsigned n=prefix&3u; if(n==3u)n=4u;
    if(n>len-at)return 0;
    uint32_t v=0;
    for(unsigned i=0;i<n;i++)v|=(uint32_t)b[at+i]<<(8u*i);
    at+=n;
    unsigned type=(prefix>>2u)&3u,tag=prefix>>4u;
    if(type==1u) {
      switch(tag){
      case 0:page=v;break;
      case 1:lo=sign_extend(v,n*8u);break;
      case 2:hi=lo<0?sign_extend(v,n*8u):(int32_t)v;break;
      case 7:rsize=v;break;
      case 8:rid=v&255u;break;
      case 9:rcount=v;break;
      default:break;
      }
    } else if(type==2u) {
      switch(tag) {
      case 0:if(u_count<32u)usages[u_count++]=v;break;
      case 1:umin=v;break;
      case 2:umax=v;break;
      default:break;
      }
    } else if(type==0u) {
      if(tag==10u) {
        ++depth;
        unsigned usage=u_count?usages[0]:umin;
        if(!active_level && page==HID_PAGE_DESKTOP &&
           (usage==HID_USAGE_JOYSTICK || usage==HID_USAGE_GAMEPAD) &&
           v==1u) active_level=depth;
      } else if(tag==12u) {
        if(active_level==depth)active_level=0;
        if(depth) --depth;
      } else if(tag==8u) {
        if(!rsize || rsize>32u || rcount>128u ||
           (uint64_t)rsize*rcount>512u)return 0;
        unsigned base=offsets[rid];
        if(rid && !base)base=8u;
        uint64_t end=(uint64_t)base+(uint64_t)rsize*rcount;
        if(end>512u)return 0;
        offsets[rid]=(uint16_t)end;
        if(active_level && (v&1u)==0u) {
          if(!selected) {out.id=rid;out.has_id=rid!=0u;selected=1;}
          if(out.id==rid && (v&2u)!=0u) {
            if(page==HID_PAGE_BUTTON && !out.button_count &&
               rcount && rsize<=8u && rsize*rcount<=32u) {
              out.button_offset=base;
              out.button_count=rcount;
              out.button_bits=rsize;
            }
            if(page==HID_PAGE_DESKTOP) {
              for(unsigned i=0;i<rcount;i++) {
                unsigned usage=i<u_count?usages[i]:
                               (umin && umin+i<=umax?umin+i:0u);
                struct Field *field=0;
                if(usage==HID_USAGE_X)field=&out.x;
                if(usage==HID_USAGE_Y)field=&out.y;
                if(usage==HID_USAGE_HAT)field=&out.hat;
                if(usage>=HID_USAGE_Z && usage<=HID_USAGE_RZ) {
                  if(!out.extra_x.valid)field=&out.extra_x;
                  else if(!out.extra_y.valid)field=&out.extra_y;
                }
                if(field && !field->valid) {
                  field->bit=base+i*rsize;field->size=rsize;
                  field->lo=lo;field->hi=hi;field->valid=1u;
                }
              }
            }
          }
        }
      }
      /* HID local usages are scoped to a single main item. */
      u_count=umin=umax=0;
    }
  }
  if(!selected || (!out.x.valid && !out.y.valid && !out.hat.valid &&
                    !out.extra_x.valid && !out.extra_y.valid) ||
     out.button_bits>8u) return 0;
  out.valid=1u;
  pads[index]=out;
  return 1;
}
int frf_native_usb_pad_set_descriptor(const uint8_t *b,unsigned len)
{
  return frf_native_usb_pad_set_descriptor_for(0u,b,len);
}
int frf_native_usb_pad_report_for(unsigned index,const uint8_t *b,unsigned len)
{
  if(index>=4u || !pads[index].valid || !b || !len || len>64u) return 0;
  const struct Pad *pad=&pads[index];
  if(pad->switch_pro) {
    /* Nintendo USB 057e:2009, reports 0x30 and 0x21 share the same
       first 13 bytes: [0] report, [1] timer, [2] battery,
       [3] right buttons, [4] shared, [5] left (D-pad),
       [6..8] L stick packed 12-bit X,Y, [9..11] R stick.
       On 0x3f the controller uses the simpler input-only layout. */
    if((b[0]==0x30u || b[0]==0x21u) && len>=12u) {
      uint32_t lx=(uint32_t)b[6]|(((uint32_t)b[7]&15u)<<8u);
      uint32_t ly=((uint32_t)b[7]>>4u)|((uint32_t)b[8]<<4u);
      uint32_t rx=(uint32_t)b[9]|(((uint32_t)b[10]&15u)<<8u);
      uint32_t ry=((uint32_t)b[10]>>4u)|((uint32_t)b[11]<<4u);
      int dx=((b[5]&0x04u)?127:0)-((b[5]&0x08u)?127:0);
      int dy=((b[5]&0x01u)?127:0)-((b[5]&0x02u)?127:0);
      unsigned buttons=((b[3]&(0x08u|0x01u))?1u:0u)|
                       ((b[3]&0x04u)?2u:0u)|
                       ((b[3]&0x02u)?4u:0u);
      /* Forward sticks 0/1 and 2/3, plus digital D-pad. The same
         20 percent dead zone and Pi3 secondary-button mapping apply. */
      frf_native_usb_pad_update_axes(((int)lx-2048)/16,
                                    ((int)ly-2048)/16,
                                    dx?dx:((int)rx-2048)/16,
                                    dy?dy:((int)ry-2048)/16,
                                    buttons,8);
      return 1;
    }
    if(b[0]==0x3fu && len>=12u) {
      int hat=b[3]&15u;
      if(hat>7)hat=8;
      int32_t lx=(int32_t)((uint32_t)b[4]|((uint32_t)b[5]<<8u));
      int32_t ly=(int32_t)((uint32_t)b[6]|((uint32_t)b[7]<<8u));
      unsigned buttons=((b[1]&0x08u)?1u:0u)|
                       ((b[1]&0x04u)?2u:0u)|
                       ((b[1]&0x02u)?4u:0u);
      frf_native_usb_pad_update_axes((int)((lx-32768)/256),
                                    (int)((ly-32768)/256),
                                    0,0,buttons,hat);
      return 1;
    }
    return 0; /* 0x81 ACK and other control IDs are not input. */
  }
  if(pad->has_id && b[0]!=pad->id) return 0;
  if(pad->shanwan) {
    /* Proven Pi3 ShanWan 2563:0576:
       report 1, 10 bytes: 15 button bits in b1/b2, hat low nibble b3,
       axes 0..3 in b4..b7. Other IDs (5 mouse, 2 output) are ignored. */
    if(len!=10u || b[0]!=1u)return 0;
    unsigned buttons=(unsigned)b[1]|(((unsigned)b[2]&0x7fu)<<8);
    int hat=(b[3]&0x0fu);
    if(hat>7)hat=8;
    frf_native_usb_pad_update_axes((int)b[4]-128,(int)b[5]-128,
                                   (int)b[6]-128,(int)b[7]-128,
                                   buttons,hat);
    return 1;
  }
  const struct Field *fields[5]={&pad->x,&pad->y,&pad->hat,
                                  &pad->extra_x,&pad->extra_y};
  for(unsigned i=0;i<5u;i++)
    if(fields[i]->valid &&
       ((uint64_t)fields[i]->bit+fields[i]->size>(uint64_t)len*8u))
      return 0;
  unsigned buttons=0;
  if(pad->button_count) {
    unsigned size=pad->button_count*pad->button_bits;
    if(pad->button_offset+size>len*8u)return 0;
    buttons=bits_get(b,len,pad->button_offset,size);
  }
  int x=scale_axis(&pad->x,b,len),y=scale_axis(&pad->y,b,len);
  int hat=8;
  if(pad->hat.valid) {
    int hv=field_get(&pad->hat,b,len);
    hat=hv>=pad->hat.lo && hv<=pad->hat.hi ? hv-pad->hat.lo:8;
    if(hat>7)hat=8;
  }
  int dx=scale_axis(&pad->extra_x,b,len),dy=scale_axis(&pad->extra_y,b,len);
  frf_native_usb_pad_update_axes(x,y,dx,dy,buttons,hat);
  return 1;
}

int frf_native_usb_pad_report(const uint8_t *b,unsigned len)
{
  return frf_native_usb_pad_report_for(0u,b,len);
}
