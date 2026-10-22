/* SPDX-License-Identifier: GPL-2.0-only
 * Descriptor-driven USB HID -> Amiga port 2 gamepad translation.
 * Only claims actual Generic Desktop Joystick/Game Pad APPLICATION
 * collections, not arbitrary keyboard/mouse/vendor HID packets.
 * All HID items decoded from USB byte order, never compiler bitfields.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "frf_r19_hid_descriptor.h"
enum { AXIS_X=1,AXIS_Y=2,HAT=3,BUTTON=4,DP_UP=5,DP_DOWN=6,DP_RIGHT=7,DP_LEFT=8,AXIS_DX=9,AXIS_DY=10 };
struct hid_state {
  uint32_t usage_page,report_size,report_count,report_id;
  int32_t logical_min,logical_max;
};
static int32_t sign_extend(uint32_t v,unsigned bits) {
  if(bits==0 || bits>=32) return (int32_t)v;
  uint32_t mask=(1u<<bits)-1u;
  v &= mask;
  return (int32_t)((v^(1u<<(bits-1)))-(1u<<(bits-1)));
}
static uint32_t uint_le(const uint8_t *s,unsigned n) {
  uint32_t v=0;
  for(unsigned i=0;i<n;i++) v|=(uint32_t)s[i] << (8u*i);
  return v;
}
struct local_state {
  uint32_t usages[32];
  uint32_t first,last;
  uint8_t n,range;
};
static void local_reset(struct local_state *l) { memset(l,0,sizeof(*l)); }
static uint32_t local_usage(const struct local_state *l,unsigned i) {
  if(i<l->n) return l->usages[i];
  if(l->range && l->last>=l->first)
    return l->first + ((i<=(l->last-l->first))?i:(l->last-l->first));
  if(l->n) return l->usages[l->n-1];
  return 0u;
}
static int kind_for(uint32_t page,uint32_t usage) {
  if(page==1u) {
    if(usage==0x30u) return AXIS_X;
    if(usage==0x31u) return AXIS_Y;
    if(usage==0x32u) return AXIS_DX;
    if(usage==0x33u) return AXIS_DY;
    if(usage==0x39u) return HAT;
    if(usage==0x90u) return DP_UP;
    if(usage==0x91u) return DP_DOWN;
    if(usage==0x92u) return DP_RIGHT;
    if(usage==0x93u) return DP_LEFT;
  }
  if(page==9u && usage>0u && usage<=64u) return BUTTON;
  return 0;
}
static unsigned id_index(uint32_t report_id) {
  return report_id>=FRF_HID_REPORTS ? FRF_HID_REPORTS : report_id;
}
/* Returns positive only when a full gamepad collection has at least
 * x/y, hat, dpad or button with known bit ranges. Never guesses offsets.
 */
int frf_hid_gamepad_parse(struct frf_hid_map *out,const uint8_t *data,
                           uint16_t size) {
  if(!out) return 0;
  memset(out,0,sizeof(*out));
  if(!data || !size || size>2048u) return 0;
  struct hid_state g={0};
  struct hid_state global_stack[4];
  unsigned stack_depth=0;
  struct local_state l={0};
  uint16_t offsets[FRF_HID_REPORTS]={0};
  unsigned depth=0;
  unsigned gamepad_depth=0;
  uint32_t active_id=0;
  int any_gamepad=0;
  for(unsigned pos=0;pos<size;) {
    uint8_t code=data[pos++];
    if(code==0xfeu) { /* HID long item */
      if((unsigned)pos+2u>size) return 0;
      unsigned n=data[pos];pos+=2u;
      if(pos+n>size) return 0;
      pos+=n;continue;
    }
    unsigned bytes=code&3u;
    if(bytes==3u)bytes=4u;
    if(pos+bytes>size) return 0;
    uint32_t raw=uint_le(data+pos,bytes);
    pos+=bytes;
    unsigned type=(code>>2u)&3u,tag=(code>>4u)&15u;
    if(type==1u) { /* global */
      switch(tag) {
      case 0:g.usage_page=raw;break;
      case 1:g.logical_min=sign_extend(raw,bytes*8u);break;
      case 2:g.logical_max=g.logical_min<0?
                 sign_extend(raw,bytes*8u):(int32_t)raw;break;
      case 7:g.report_size=raw;break;
      case 8:g.report_id=raw; if(raw)out->has_ids=1;break;
      case 9:g.report_count=raw;break;
      case 10:if(stack_depth<4u)global_stack[stack_depth++]=g;break;
      case 11:if(stack_depth)g=global_stack[--stack_depth];break;
      default:break;
      }
    } else if(type==2u) { /* local */
      switch(tag) {
      case 0:if(l.n<32u)l.usages[l.n++]=raw;break;
      case 1:l.first=raw;l.range=1;break;
      case 2:l.last=raw;l.range=1;break;
      default:break;
      }
    } else if(type==0u) { /* main */
      if(tag==10u) { /* Collection */
        ++depth;
        uint32_t use=local_usage(&l,0);
        uint32_t page=g.usage_page;
        if(use>>16u) {page=use>>16u;use&=0xffffu;}
        if(!gamepad_depth && raw==1u && page==1u &&
           (use==4u || use==5u)) {
          gamepad_depth=depth;
          active_id=g.report_id;
          any_gamepad=1;
        }
      } else if(tag==12u) { /* End Collection */
        if(gamepad_depth && depth==gamepad_depth) gamepad_depth=0;
        if(depth) --depth;
      } else if(tag==8u) { /* Input */
        if(g.report_size>32u || g.report_count>1024u ||
           g.report_size*g.report_count>8192u) return 0;
        unsigned rid=id_index(g.report_id);
        if(rid>=FRF_HID_REPORTS) return 0;
        uint32_t offset=offsets[rid];
        uint32_t total=g.report_size*g.report_count;
        if(offset+total>8192u) return 0;
        const bool variable=(raw&2u)!=0u;
        const bool is_const=(raw&1u)!=0u;
        const bool relative=(raw&4u)!=0u;
        if(gamepad_depth && !is_const && !relative &&
           g.report_size>0u && g.report_size<=32u && variable) {
          for(unsigned i=0;i<g.report_count && out->count<FRF_HID_FIELDS;++i) {
            uint32_t usage=local_usage(&l,i),page=g.usage_page;
            if(usage>>16u){page=usage>>16u;usage&=0xffffu;}
            int kind=kind_for(page,usage);
            if(!kind)continue;
            struct frf_hid_field *f=&out->field[out->count++];
            f->bit=(uint16_t)(offset+i*g.report_size);
            f->width=(uint8_t)g.report_size;
            f->report_id=(uint8_t)g.report_id;
            f->kind=(uint8_t)kind;
            f->usage=(uint16_t)usage;
            f->min=g.logical_min;
            f->max=g.logical_max;
          }
        }
        offsets[rid]=(uint16_t)(offset+total);
        if(gamepad_depth) active_id=g.report_id;
      }
      local_reset(&l);
    }
  }
  for(unsigned i=0;i<FRF_HID_REPORTS;i++)
    out->report_bits[i]=offsets[i];
  if(!any_gamepad || !out->count) return 0;
  unsigned useful=0;
  for(unsigned i=0;i<out->count;++i) {
    int kind=out->field[i].kind;
    if(kind==AXIS_X || kind==AXIS_Y || kind==HAT ||
       kind==BUTTON || kind>=DP_UP) useful++;
  }
  out->valid=useful>0;
  out->first_id=(uint8_t)active_id;
  return out->valid;
}
/* HID uses independent report IDs. Preserve fields delivered in other IDs. */
unsigned frf_hid_gamepad_report_capabilities(const struct frf_hid_map *m,uint8_t report_id)
{
  if(!m || !m->valid)return 0;
  unsigned flags=0;
  for(unsigned i=0;i<m->count;++i) {
    const struct frf_hid_field *f=&m->field[i];
    if(m->has_ids && f->report_id!=report_id)continue;
    if(f->kind==BUTTON)flags|=2u;
    else if(f->kind==AXIS_X || f->kind==AXIS_Y || f->kind==HAT ||
            (f->kind>=DP_UP && f->kind<=AXIS_DY))flags|=1u;
  }
  return flags;
}
static uint32_t extract_bits(const uint8_t *p,uint16_t len,unsigned bit,
                             unsigned width) {
  if(width==0 || width>32 || bit+width>(unsigned)len*8u)return 0u;
  uint32_t v=0;
  for(unsigned i=0;i<width;++i)
    v|=(uint32_t)((p[(bit+i)>>3u]>>((bit+i)&7u))&1u)<<i;
  return v;
}
int frf_hid_gamepad_decode_buttons(const struct frf_hid_map*m,const uint8_t*d,uint16_t len,uint32_t*out,uint32_t*present){
if(!m||!m->valid||!d||!len||!out||!present)return 0;
unsigned id=m->has_ids?d[0]:0u;unsigned n=m->has_ids?len-1u:len;const uint8_t*p=m->has_ids?d+1:d;
uint32_t value=0,mask=0;
for(unsigned i=0;i<m->count;i++){const struct frf_hid_field*f=&m->field[i];
if(f->kind!=BUTTON||(m->has_ids&&f->report_id!=id)||!f->usage||f->usage>32u)continue;
if((unsigned)f->bit+f->width>n*8u)continue;
uint32_t b=1u<<(f->usage-1u);
mask|=b;
if(extract_bits(p,(uint16_t)n,f->bit,f->width))value|=b;
}
*out=value;*present=mask;return mask!=0;
}
static int scale_direction(int value,int min,int max) {
  if(max<=min) return 0;
  int midpoint=min+(max-min)/2;
  int dead=(max-min)/5;
  if(dead<1)dead=1;
  return value<midpoint-dead?-1:value>midpoint+dead?1:0;
}
int frf_hid_gamepad_decode(const struct frf_hid_map *m,
                            const uint8_t *data,uint16_t len,
                            uint16_t *joy,int *fire) {
  if(!m || !m->valid || !data || !joy || !fire) return 0;
  unsigned report_id=m->has_ids?(len?data[0]:0u):0u;
  if(m->has_ids && !report_id) return 0;
  unsigned payload_len=(len && m->has_ids)?(unsigned)len-1u:(unsigned)len;
  if(payload_len*8u<m->report_bits[report_id]) return 0;
  int x=0,y=0,dx=0,dy=0,x_valid=0,y_valid=0,hat=-1,hat_min=0;
  int up=0,down=0,left=0,right=0;
  int have_button=0,pressed=0,found=0;
  for(unsigned i=0;i<m->count;++i) {
    const struct frf_hid_field *f=&m->field[i];
    const uint8_t *p=data;
    unsigned n=len;
    if(f->report_id) {
      if(n==0 || p[0]!=f->report_id) continue;
      p++;n--;
    }
    if((unsigned)f->bit+f->width>n*8u) continue;
    uint32_t raw=extract_bits(p,(uint16_t)n,f->bit,f->width);
    int32_t v=(f->min<0)?sign_extend(raw,f->width):(int32_t)raw;
    ++found;
    if(f->kind==AXIS_X && !x_valid) {x=scale_direction(v,f->min,f->max);x_valid=1;}
    if(f->kind==AXIS_Y && !y_valid) {y=scale_direction(v,f->min,f->max);y_valid=1;}
    if(f->kind==HAT){hat=v;hat_min=f->min;}
    if(f->kind==AXIS_DX)dx=scale_direction(v,f->min,f->max);
    if(f->kind==AXIS_DY)dy=scale_direction(v,f->min,f->max);
    if(f->kind==BUTTON){have_button=1;if(v)pressed=1;}
    if(f->kind==DP_UP) up|=(v!=0);
    if(f->kind==DP_DOWN) down|=(v!=0);
    if(f->kind==DP_RIGHT) right|=(v!=0);
    if(f->kind==DP_LEFT) left|=(v!=0);
  }
  if(!found) return 0;
  if(hat>=0) {
    int h=hat-hat_min;
    if(h>=0 && h<8) {
      up|=(h==0 || h==1 || h==7);
      right|=(h==1 || h==2 || h==3);
      down|=(h==3 || h==4 || h==5);
      left|=(h==5 || h==6 || h==7);
    }
  }
  left|=dx<0;right|=dx>0;up|=dy<0;down|=dy>0;
  if(x_valid){left|=x<0;right|=x>0;}
  if(y_valid){up|=y<0;down|=y>0;}
  if(left && right)left=right=0;
  if(up && down)up=down=0;
  *joy=(uint16_t)((left?0x0200u:0u) |
      (((left!=0)^(up!=0))?0x0100u:0u) |
      (right?0x0002u:0u) |
      (((right!=0)^(down!=0))?0x0001u:0u));
  *fire=have_button && pressed;
  return 1;
}
