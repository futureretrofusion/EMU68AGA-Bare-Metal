#include <stdint.h>
#include "tusb.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
typedef struct { uint32_t normal; } local_be_t;
__attribute__((used,section(".frfbe"))) const local_be_t host_order = { .normal=0x12345678u };
__attribute__((used,section(".frfbe"))) const dwc2_hprt_t port_bits = { .conn_status=1u };
__attribute__((used,section(".frfbe"))) const dwc2_ghwcfg2_t phy_bits = { .hs_phy_type=2u };
__attribute__((used,section(".frfbe"))) const tusb_rhport_init_t host_config = {
 .role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH
};
uint32_t frf_be_probe(const volatile dwc2_regs_t *r)
{ return r->hprt; }
