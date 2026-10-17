/* FRF / EMU68AGA ARM<->Amiga Ethernet framing interface, ABI v1.
 * Original integration glue, not a hardware driver or complete network stack.
 * 68k/ARM in this project use big-endian native 32-bit words.
 * Shared memory must be mapped at stable addresses in BOTH address spaces
 * and have a deliberate coherent-cache policy before live use.
 */
#ifndef FRF_NET_BRIDGE_H
#define FRF_NET_BRIDGE_H
#include <stdint.h>
#define FRF_NET_MAGIC 0x4652464eu  /* FRFN */
#define FRF_NET_ABI 1u
#define FRF_NET_SLOTS 16u
#define FRF_NET_FRAME_BYTES 1536u
#define FRF_NET_MAX_ETHERNET_FRAME 1518u /* no FCS */
enum frf_net_status {
    FRF_NET_DOWN = 0,
    FRF_NET_PROBING = 1,
    FRF_NET_ASSOCIATING = 2,
    FRF_NET_LINK_UP = 3,
    FRF_NET_FAILED = 4
};
struct frf_net_packet {
    uint32_t length;
    uint32_t reserved;
    uint8_t frame[FRF_NET_FRAME_BYTES];
};
struct frf_net_ring {
    uint32_t producer; /* producer only */
    uint32_t consumer; /* consumer only */
    uint32_t rejected;
    uint32_t reserved;
    struct frf_net_packet slot[FRF_NET_SLOTS];
};
struct frf_net_shared {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t bytes;
    uint32_t generation;
    uint32_t link_status;
    uint32_t link_generation;
    uint32_t mtu;
    uint32_t flags;
    uint8_t mac[8];
    uint32_t reserved[6];
    struct frf_net_ring amiga_tx_arm_rx; /* Amiga SANA-II writes; ARM receives */
    struct frf_net_ring arm_tx_amiga_rx; /* ARM radio receives; Amiga reads */
};
typedef char frf_packet_abi_check[(sizeof(struct frf_net_packet)==1544)?1:-1];
typedef char frf_ring_power_two_check[((FRF_NET_SLOTS & (FRF_NET_SLOTS-1u))==0)?1:-1];
void frf_net_bridge_init(struct frf_net_shared *s, const uint8_t mac[6]);
int frf_net_push(struct frf_net_ring *r,const void *frame,uint32_t len);
int frf_net_pop(struct frf_net_ring *r,void *dst,uint32_t dst_capacity,uint32_t *out_len);
#endif
