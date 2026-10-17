/* Deterministic SPSC test implementation.
 * Before hardware integration, memory barriers / cache operations must
 * be replaced or verified for Emu68 68k/ARM cross-core shared mappings.
 * This DOES NOT register an Amiga device or initialize WiFi hardware.
 */
#include <string.h>
#include "frf_net_bridge.h"
void frf_net_bridge_init(struct frf_net_shared *s, const uint8_t mac[6])
{
    memset(s,0,sizeof *s);
    s->magic=FRF_NET_MAGIC;
    s->abi_version=FRF_NET_ABI;
    s->bytes=(uint32_t)sizeof *s;
    s->mtu=1500;
    s->generation=1;
    if(mac)memcpy(s->mac,mac,6);
}
int frf_net_push(struct frf_net_ring *r,const void *frame,uint32_t len)
{
    if(!r||!frame||len<14||len>FRF_NET_MAX_ETHERNET_FRAME) return -2;
    uint32_t p=r->producer,c=r->consumer;
    if((uint32_t)(p-c)>=FRF_NET_SLOTS){r->rejected++;return -1;}
    struct frf_net_packet *packet=&r->slot[p&(FRF_NET_SLOTS-1u)];
    memcpy(packet->frame,frame,len);
    packet->length=len;
    /* Compiler release barrier; hardware cacheability must be audited */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->producer=p+1;
    return 0;
}
int frf_net_pop(struct frf_net_ring *r,void *dst,uint32_t cap,uint32_t *out_len)
{
    if(!r||!dst||!out_len)return -2;
    uint32_t c=r->consumer,p=r->producer;
    if(c==p)return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    struct frf_net_packet *packet=&r->slot[c&(FRF_NET_SLOTS-1u)];
    uint32_t n=packet->length;
    if(n<14||n>FRF_NET_MAX_ETHERNET_FRAME||cap<n)return -2;
    memcpy(dst,packet->frame,n);
    *out_len=n;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->consumer=c+1;
    return 1;
}
