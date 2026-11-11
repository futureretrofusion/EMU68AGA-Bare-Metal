/* FRF Pi4 VL805 PCIe xHCI probe/bring-up V0.1
 *
 * Replaces the incorrect /scb/xhci probe for the Pi4 external USB-A ports.
 * The external ports are owned by the VIA VL805 behind BCM2711 PCIe.
 *
 * Architecture follows the working Emu68 bcmpcie.library/xhci.device stack:
 *   BCM2711 root complex -> bus 1 dev 0 VL805 -> BAR0 -> xHCI
 * The existing FRF polling xHCI host still owns all xHCI ring/HID logic.
 */
#include <stdint.h>
#include "support.h"
#include "devicetree.h"
#include "support_rpi.h"
#include "frf_services.h"
#include "frf_xhci_probe.h"

#define PCIE_RGR1_SW_INIT_1             0x9210u
#define PCIE_SW_INIT                    (1u << 1)
#define PCIE_PERST                      (1u << 0)
#define PCIE_MISC_HARD_DEBUG            0x4204u
#define PCIE_SERDES_IDDQ                (1u << 27)
#define PCIE_MISC_CTRL                  0x4008u
#define PCIE_SCB_ACCESS_EN              (1u << 12)
#define PCIE_CFG_READ_UR                (1u << 13)
#define PCIE_RCB_MPS                    (1u << 10)
#define PCIE_RCB_64B                    (1u << 7)
#define PCIE_SCB0_SIZE_MASK             0xf8000000u
#define PCIE_RC_BAR1_LO                 0x402cu
#define PCIE_RC_BAR2_LO                 0x4034u
#define PCIE_RC_BAR2_HI                 0x4038u
#define PCIE_RC_BAR3_LO                 0x403cu
#define PCIE_RC_BAR_SIZE_MASK           0x1fu
#define PCIE_STATUS                     0x4068u
#define PCIE_STATUS_RC                  (1u << 7)
#define PCIE_STATUS_DL_ACTIVE           (1u << 5)
#define PCIE_STATUS_PHY_UP              (1u << 4)
#define PCIE_REVISION                   0x406cu
#define PCIE_WIN0_LO                    0x400cu
#define PCIE_WIN0_HI                    0x4010u
#define PCIE_WIN0_BASE_LIMIT            0x4070u
#define PCIE_WIN0_BASE_HI               0x4080u
#define PCIE_WIN0_LIMIT_HI              0x4084u
#define PCIE_EXT_CFG_INDEX              0x9000u
#define PCIE_EXT_CFG_DATA               0x8000u
#define PCIE_MSI_MASK_SET               0x4510u
#define PCIE_MSI_CLR                    0x4508u
#define PCIE_PRIV_ID_VAL3               0x043cu
#define PCIE_PRIV_LINK_CAP              0x04dcu
#define PCIE_VENDOR_SPEC1               0x0188u

#define PCI_VENDOR_DEVICE               0x00u
#define PCI_COMMAND                     0x04u
#define PCI_CLASS_REVISION              0x08u
#define PCI_BUS_NUMBERS                 0x18u
#define PCI_MEMORY_BASE_LIMIT          0x20u
#define PCI_BRIDGE_CONTROL             0x3eu
#define VL805_PCI_FW_VERSION           0x50u
#define PCI_BAR0                        0x10u
#define PCI_BAR1                        0x14u
#define PCI_COMMAND_MEMORY              (1u << 1)
#define PCI_COMMAND_MASTER              (1u << 2)

#define VIA_VENDOR                      0x1106u
#define VL805_DEVICE                    0x3483u
#define XHCI_CLASS                      0x0c0330u

#define XHCI_CAP_CAPBASE                0x00u
#define XHCI_CAP_HCSPARAMS1             0x04u
#define XHCI_CAP_HCCPARAMS1             0x10u
#define XHCI_CAP_DBOFF                  0x14u
#define XHCI_CAP_RTSOFF                 0x18u
#define XHCI_OP_PORTSC1                 0x400u
#define FRFXHCI_ACCESS_DEVICE32         3u

volatile uint32_t frf_pci_ro_status;
volatile uint32_t frf_pci_ro_root_id;
volatile uint32_t frf_pci_ro_busnums;
volatile uint32_t frf_pci_ro_ep_id;
volatile uint32_t frf_pci_ro_ep_bar0;
volatile uint32_t frf_pci_ro_probe_status;
/* R40: actual values that allow a bad PCIe decode to be distinguished from
 * controller firmware not being loaded. Written only by USB CPU1 probe. */
volatile uint32_t frf_usb_root_command;
volatile uint32_t frf_usb_bridge_window;
volatile uint32_t frf_usb_endpoint_command;
volatile uint32_t frf_usb_endpoint_bar;
volatile uint32_t frf_usb_fw_before;
volatile uint32_t frf_usb_fw_after;
volatile uint32_t frf_usb_reset_rc;
volatile uint32_t frf_usb_caps_reads;
volatile uint32_t frf_usb_caps_raw;
/* R46: inbound PCIe DMA window needed for VL805 to fetch rings in RAM.
 * Readbacks are from BCM2711 RC MMIO, not synthetic expected values. */
volatile uint32_t frf_usb_dma_rc_bar2_before,frf_usb_dma_rc_bar2_after;
volatile uint32_t frf_usb_dma_rc_bar2_hi_before,frf_usb_dma_rc_bar2_hi_after;
volatile uint32_t frf_usb_dma_misc_before,frf_usb_dma_misc_after;
volatile uint32_t frf_usb_dma_expected_bar2,frf_usb_dma_expected_scb;
volatile uint32_t frf_usb_dma_action;
/* R55: the actual PCI bus <-> CPU physical DMA window from firmware DT.
 * Matched against our programmed inbound RC BAR2, not inferred from MMU VA. */
volatile uint64_t frf_r55_dma_cpu_base;
volatile uint64_t frf_r55_dma_bus_base;
volatile uint64_t frf_r55_dma_window_size;




static volatile struct FRFXHCIProbeInfo g_xhci = {
    .state = FRFS_USB_BACKEND_OFFLINE,
    .status = FRFXHCI_PROBE_NOT_RUN
};

static inline uint64_t sys_us(void)
{
    volatile struct { uint32_t lo, hi; } *clk = (volatile void *)0xf2003004;
    uint32_t h1,h2,l;
    do { h1=LE32(clk->hi); l=LE32(clk->lo); h2=LE32(clk->hi); } while (h1!=h2);
    return ((uint64_t)h1 << 32) | l;
}

static void delay_us_local(uint32_t us)
{
    uint64_t end=sys_us()+us;
    while ((int64_t)(sys_us()-end)<0) __asm__ volatile("yield");
}

static uint64_t cells_be(const uint32_t *cells, uint32_t count)
{
    uint64_t v=0;
    while (count--) v=(v<<32) | (uint64_t)BE32(*cells++);
    return v;
}

static uint32_t mmio_read32(uintptr_t base, uint32_t off)
{
    volatile uint32_t *p=(volatile uint32_t *)(base+(uintptr_t)off);
    uint32_t v=LE32(*p);
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static void mmio_write32(uintptr_t base, uint32_t off, uint32_t v)
{
    volatile uint32_t *p=(volatile uint32_t *)(base+(uintptr_t)off);
    *p=LE32(v);
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint16_t mmio_read16_addr(uintptr_t a)
{
    uint16_t v=*(volatile uint16_t *)a;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v=__builtin_bswap16(v);
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static void mmio_write16_addr(uintptr_t a, uint16_t v)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v=__builtin_bswap16(v);
#endif
    *(volatile uint16_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* platform_init() rewrites the CPU side of /scb ranges to low DEVICE virtual. */
static int current_device_virt(of_node_t *node, uint64_t bus_addr, uintptr_t *virt_out)
{
    if (!node || !node->on_parent || !node->on_parent->on_parent || !virt_out) return 0;
    of_node_t *bus=node->on_parent, *parent=bus->on_parent;
    of_property_t *rp=dt_find_property(bus,"ranges");
    if (!rp || !rp->op_value) return 0;
    uint32_t ca=dt_get_property_value_u32(bus,"#address-cells",2,1);
    uint32_t pa=dt_get_property_value_u32(parent,"#address-cells",2,0);
    uint32_t sc=dt_get_property_value_u32(bus,"#size-cells",2,1);
    if (!ca || ca>2 || !pa || pa>2 || !sc || sc>2) return 0;
    uint32_t n=ca+pa+sc, bytes=n*4u, left=rp->op_length;
    const uint8_t *raw=(const uint8_t *)rp->op_value;
    while (left>=bytes) {
        const uint32_t *r=(const uint32_t *)(const void *)raw;
        uint64_t child=cells_be(r,ca), cpu=cells_be(r+ca,pa), span=cells_be(r+ca+pa,sc);
        if (span && bus_addr>=child && bus_addr-child<span) {
            uint64_t v=cpu+(bus_addr-child);
            if (v && v<=0xffffffffULL) { *virt_out=(uintptr_t)v; return 1; }
            return 0;
        }
        raw+=bytes; left-=bytes;
    }
    return 0;
}

struct pcie_window {
    uint64_t bus;
    uint64_t phys;
    uint64_t size;
    uintptr_t virt;
};

static int pcie_get_window(of_node_t *pcie, const char *name, struct pcie_window *w)
{
    of_property_t *rp=dt_find_property(pcie,name);
    if (!rp || !rp->op_value || !w) return 0;
    uint32_t ac=dt_get_property_value_u32(pcie,"#address-cells",3,1);
    uint32_t pa=dt_get_property_value_u32(pcie->on_parent,"#address-cells",2,0);
    uint32_t sc=dt_get_property_value_u32(pcie,"#size-cells",2,1);
    if (ac<2 || ac>3 || !pa || pa>2 || !sc || sc>2) return 0;
    uint32_t n=ac+pa+sc, bytes=n*4u, left=rp->op_length;
    const uint8_t *raw=(const uint8_t *)rp->op_value;
    while (left>=bytes) {
        const uint32_t *r=(const uint32_t *)(const void *)raw;
        uint32_t attr=BE32(r[0]);
        if ((attr & 0x03000000u)==0x02000000u && !(attr & 0x40000000u)) {
            w->bus=cells_be(r+1,ac-1);
            w->phys=cells_be(r+ac,pa);
            w->size=cells_be(r+ac+pa,sc);
            w->virt=0;
            if (!__builtin_strcmp(name,"ranges")) {
                of_property_t *vp=dt_find_property(pcie,"emu68,pci-mmio-virt");
                of_property_t *sp=dt_find_property(pcie,"emu68,pci-mmio-size");
                if (!vp || !vp->op_value || vp->op_length<4) return 0;
                w->virt=(uintptr_t)BE32(*(const uint32_t *)vp->op_value);
                if (sp && sp->op_value && sp->op_length>=4)
                    w->size=(uint64_t)BE32(*(const uint32_t *)sp->op_value);
            }
            return w->size!=0;
        }
        raw+=bytes; left-=bytes;
    }
    return 0;
}

static unsigned log2_floor64(uint64_t v)
{
    unsigned n=0;
    while (v>>1) { v>>=1; ++n; }
    return n;
}

static uint64_t round_pow2(uint64_t v)
{
    if (v<=1) return 1;
    --v;
    for (unsigned s=1;s<64;s<<=1) v|=v>>s;
    return v+1;
}

static uint32_t ibar_size_code(uint64_t size)
{
    unsigned l=log2_floor64(size);
    if (l>=12 && l<=15) return (uint32_t)((l-12)+0x1c);
    if (l>=16 && l<=37) return (uint32_t)(l-15);
    return 0;
}

static int link_up(uintptr_t rc)
{
    uint32_t s=mmio_read32(rc,PCIE_STATUS);
    return (s & (PCIE_STATUS_DL_ACTIVE|PCIE_STATUS_PHY_UP)) ==
           (PCIE_STATUS_DL_ACTIVE|PCIE_STATUS_PHY_UP);
}

static uintptr_t cfg_addr(uintptr_t rc, unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    if (bus==0) return rc+off;
    uint32_t idx=((bus & 0xffu)<<20)|((dev & 0x1fu)<<15)|((fn & 7u)<<12);
    mmio_write32(rc,PCIE_EXT_CFG_INDEX,idx);
    return rc+PCIE_EXT_CFG_DATA+off;
}

static uint32_t cfg_read32(uintptr_t rc,unsigned bus,unsigned dev,unsigned fn,unsigned off)
{
    uintptr_t a=cfg_addr(rc,bus,dev,fn,off);
    uint32_t v=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");
    return LE32(v);
}

static void cfg_write32(uintptr_t rc,unsigned bus,unsigned dev,unsigned fn,unsigned off,uint32_t v)
{
    uintptr_t a=cfg_addr(rc,bus,dev,fn,off);
    *(volatile uint32_t *)a=LE32(v);
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint16_t cfg_read16(uintptr_t rc,unsigned bus,unsigned dev,unsigned fn,unsigned off)
{
    return mmio_read16_addr(cfg_addr(rc,bus,dev,fn,off));
}

static void cfg_write16(uintptr_t rc,unsigned bus,unsigned dev,unsigned fn,unsigned off,uint16_t v)
{
    mmio_write16_addr(cfg_addr(rc,bus,dev,fn,off),v);
}

static void set_outbound(uintptr_t rc, const struct pcie_window *w)
{
    uint64_t phys_mb=w->phys>>20;
    uint64_t lim_mb=(w->phys+w->size-1)>>20;
    mmio_write32(rc,PCIE_WIN0_LO,(uint32_t)w->bus);
    mmio_write32(rc,PCIE_WIN0_HI,(uint32_t)(w->bus>>32));
    uint32_t x=mmio_read32(rc,PCIE_WIN0_BASE_LIMIT);
    x &= ~(0x0000fff0u|0xfff00000u);
    x |= ((uint32_t)phys_mb << 4) & 0x0000fff0u;
    x |= ((uint32_t)lim_mb << 20) & 0xfff00000u;
    mmio_write32(rc,PCIE_WIN0_BASE_LIMIT,x);
    x=mmio_read32(rc,PCIE_WIN0_BASE_HI);
    x=(x & ~0xffu) | (uint32_t)((phys_mb>>12)&0xffu);
    mmio_write32(rc,PCIE_WIN0_BASE_HI,x);
    x=mmio_read32(rc,PCIE_WIN0_LIMIT_HI);
    x=(x & ~0xffu) | (uint32_t)((lim_mb>>12)&0xffu);
    mmio_write32(rc,PCIE_WIN0_LIMIT_HI,x);
}

/* On Pi4 firmware may enumerate the PCIe bridge before our kernel starts.
 * A live PCIe link proves config/MMIO access, NOT necessarily that incoming
 * DMA transactions can target host RAM. Compare RC_BAR2/SCB mapping and
 * correct only if a mismatch is observed. Never assert PERST here.
 *
 * For Pi4 DT dma-ranges: PCI 0x00000000 => CPU 0x00000000, 3GiB.
 * BCM2711 RC_BAR2 must cover a power-of-two window, here 4GiB.
 * This is separate from the already validated 0xC0000000 BAR0 outbound.
 */
static int ensure_inbound_dma_on_live_link(uintptr_t rc,const struct pcie_window *dma)
{
    frf_usb_dma_action=0x10u; /* entered live-link DMA validation */
    if (!dma->size || dma->size > 0x100000000ULL) return 0;
    uint64_t inbound=round_pow2(dma->size);
    uint32_t code=ibar_size_code(inbound);
    uint64_t off=dma->bus-dma->phys;
    if (!code || (off & (inbound-1u))!=0u ||
        ((unsigned)log2_floor64(inbound))<15u) return 0;
    uint32_t exp_bar=(uint32_t)(off & ~0x1fu)|code;
    uint32_t exp_hi=(uint32_t)(off>>32);
    uint32_t exp_scb=((log2_floor64(inbound)-15u)<<27)&PCIE_SCB0_SIZE_MASK;
    frf_usb_dma_expected_bar2=exp_bar;
    frf_usb_dma_expected_scb=exp_scb;
    frf_usb_dma_rc_bar2_before=mmio_read32(rc,PCIE_RC_BAR2_LO);
    frf_usb_dma_rc_bar2_hi_before=mmio_read32(rc,PCIE_RC_BAR2_HI);
    frf_usb_dma_misc_before=mmio_read32(rc,PCIE_MISC_CTRL);
    int bar_bad=((frf_usb_dma_rc_bar2_before & ~0x1fu)!=(exp_bar&~0x1fu)) ||
                ((frf_usb_dma_rc_bar2_before & 0x1fu)!=code) ||
                frf_usb_dma_rc_bar2_hi_before!=exp_hi;
    uint32_t mm=frf_usb_dma_misc_before;
    int misc_bad=(mm & (PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR)) !=
                      (PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR) ||
                 (mm & PCIE_SCB0_SIZE_MASK)!=exp_scb;
    if (bar_bad) {
        frf_usb_dma_action|=1u;
        mmio_write32(rc,PCIE_RC_BAR2_LO,exp_bar);
        mmio_write32(rc,PCIE_RC_BAR2_HI,exp_hi);
    }
    if (misc_bad) {
        frf_usb_dma_action|=2u;
        mm |= PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR;
        mm &= ~PCIE_SCB0_SIZE_MASK;
        mm |= exp_scb;
        /* Pi4 MAX_BURST_SIZE 128B (bits21:20=0) consistent with Circle.
         * Do not change RCB mode or PCIe link/firmware in this candidate. */
        mmio_write32(rc,PCIE_MISC_CTRL,mm);
    }
    frf_usb_dma_rc_bar2_after=mmio_read32(rc,PCIE_RC_BAR2_LO);
    frf_usb_dma_rc_bar2_hi_after=mmio_read32(rc,PCIE_RC_BAR2_HI);
    frf_usb_dma_misc_after=mmio_read32(rc,PCIE_MISC_CTRL);
    kprintf("[R46-DMA] BAR2 %08x:%08x -> %08x:%08x expected=%08x:%08x MISC=%08x -> %08x scb=%08x action=%08x\\n",
            frf_usb_dma_rc_bar2_hi_before,frf_usb_dma_rc_bar2_before,
            frf_usb_dma_rc_bar2_hi_after,frf_usb_dma_rc_bar2_after,
            exp_hi,exp_bar,frf_usb_dma_misc_before,frf_usb_dma_misc_after,
            exp_scb,frf_usb_dma_action);
    if (frf_usb_dma_rc_bar2_after!=exp_bar ||
        frf_usb_dma_rc_bar2_hi_after!=exp_hi ||
        (frf_usb_dma_misc_after & PCIE_SCB0_SIZE_MASK)!=exp_scb ||
        (frf_usb_dma_misc_after&(PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR)) !=
                     (PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR)) {
        frf_usb_dma_action|=0x80000000u;
        return 0;
    }
    frf_usb_dma_action|=0x20u; /* validated */
    return 1;
}

static int root_prepare(uintptr_t rc, const struct pcie_window *mem,
                        const struct pcie_window *dma)
{
    if (!link_up(rc)) {
        uint32_t v=mmio_read32(rc,PCIE_RGR1_SW_INIT_1);
        mmio_write32(rc,PCIE_RGR1_SW_INIT_1,v|PCIE_SW_INIT|PCIE_PERST);
        delay_us_local(100);
        v=mmio_read32(rc,PCIE_RGR1_SW_INIT_1);
        mmio_write32(rc,PCIE_RGR1_SW_INIT_1,v & ~PCIE_SW_INIT);
        v=mmio_read32(rc,PCIE_MISC_HARD_DEBUG);
        mmio_write32(rc,PCIE_MISC_HARD_DEBUG,v & ~PCIE_SERDES_IDDQ);
        delay_us_local(100);

        uint64_t inbound=round_pow2(dma->size);
        uint32_t code=ibar_size_code(inbound);
        if (!code) return 0;
        uint64_t off=dma->bus-dma->phys;
        mmio_write32(rc,PCIE_RC_BAR2_LO,((uint32_t)off & ~0x1fu)|code);
        mmio_write32(rc,PCIE_RC_BAR2_HI,(uint32_t)(off>>32));
        v=mmio_read32(rc,PCIE_MISC_CTRL);
        v &= ~(0x00300000u|PCIE_SCB0_SIZE_MASK);
        v |= PCIE_SCB_ACCESS_EN|PCIE_CFG_READ_UR|PCIE_RCB_MPS|PCIE_RCB_64B;
        v |= ((log2_floor64(inbound)-15u)<<27) & PCIE_SCB0_SIZE_MASK;
        mmio_write32(rc,PCIE_MISC_CTRL,v);
        v=mmio_read32(rc,PCIE_RC_BAR1_LO);
        mmio_write32(rc,PCIE_RC_BAR1_LO,v & ~PCIE_RC_BAR_SIZE_MASK);
        v=mmio_read32(rc,PCIE_RC_BAR3_LO);
        mmio_write32(rc,PCIE_RC_BAR3_LO,v & ~PCIE_RC_BAR_SIZE_MASK);
        mmio_write32(rc,PCIE_MSI_MASK_SET,0xffffffffu);
        mmio_write32(rc,PCIE_MSI_CLR,0xffffffffu);
        set_outbound(rc,mem);

        v=mmio_read32(rc,PCIE_RGR1_SW_INIT_1);
        mmio_write32(rc,PCIE_RGR1_SW_INIT_1,v & ~PCIE_PERST);
        delay_us_local(100000);
        uint64_t end=sys_us()+100000;
        while (!link_up(rc) && (int64_t)(sys_us()-end)<0) {
            delay_us_local(5000);
        }
        if (!link_up(rc)) return 0;
    } else {
        /* R46: preserve the R40 working outbound map and root bridge
         * setup. Repair only missing inbound RC_BAR2/SCB mapping.
         * No reset, no firmware reload, no PCI config changes here. */
        if (!ensure_inbound_dma_on_live_link(rc,dma)) return 0;
        set_outbound(rc,mem);
    }

    uint32_t status=mmio_read32(rc,PCIE_STATUS);
    if (!(status & PCIE_STATUS_RC)) return 0;

    uint32_t v=mmio_read32(rc,PCIE_PRIV_ID_VAL3);
    mmio_write32(rc,PCIE_PRIV_ID_VAL3,(v & ~0x00ffffffu)|0x00060400u);
    v=mmio_read32(rc,PCIE_VENDOR_SPEC1);
    mmio_write32(rc,PCIE_VENDOR_SPEC1,v & ~0x0cu);
    v=mmio_read32(rc,PCIE_PRIV_LINK_CAP);
    mmio_write32(rc,PCIE_PRIV_LINK_CAP,v & ~0x0c00u);

    /* Fixed Pi4 topology: root bridge bus 0, VL805 downstream on bus 1. */
    cfg_write32(rc,0,0,0,PCI_BUS_NUMBERS,0x00010100u);

    /* A PCI-to-PCI bridge must forward this bus-1 MMIO range. The original
     * probe only assigned VL805 BAR0; it never configured the root's type-1
     * Memory Base/Limit nor its PCI_COMMAND_MEMORY. A responding config
     * header therefore did not prove that downstream MMIO was decodable.
     * The 64 MiB firmware DT non-prefetchable window is 1 MiB-aligned. */
    if ((mem->bus & 0xfffffu) || (mem->size & 0xfffffu) ||
        !mem->size || mem->bus+mem->size-1u>0xffffffffULL)
        return 0;
    uint32_t b=(uint32_t)((mem->bus>>16)&0xfff0u);
    uint32_t l=(uint32_t)(((mem->bus+mem->size-1u)>>16)&0xfff0u);
    uint32_t aperture=(l<<16)|b;
    cfg_write32(rc,0,0,0,PCI_MEMORY_BASE_LIMIT,aperture);
    uint16_t root_cmd=cfg_read16(rc,0,0,0,PCI_COMMAND);
    cfg_write16(rc,0,0,0,PCI_COMMAND,
                (uint16_t)(root_cmd|PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER));
    frf_usb_root_command=cfg_read16(rc,0,0,0,PCI_COMMAND);
    frf_usb_bridge_window=cfg_read32(rc,0,0,0,PCI_MEMORY_BASE_LIMIT);
    kprintf("[VL805:R40] root command=%04x bridge memory=%08x expected=%08x\n",
            frf_usb_root_command,frf_usb_bridge_window,aperture);
    if ((frf_usb_root_command & PCI_COMMAND_MEMORY)==0 ||
        (frf_usb_bridge_window & 0xfff0fff0u)!=(aperture & 0xfff0fff0u))
        return 0;
    return 1;
}

static int assign_vl805_bar(uintptr_t rc,const struct pcie_window *mem,
                            uintptr_t *virt_out,uint32_t *bytes_out,uint32_t *bus_out)
{
    uint32_t id=cfg_read32(rc,1,0,0,PCI_VENDOR_DEVICE);
    uint16_t vendor=(uint16_t)(id & 0xffffu), device=(uint16_t)(id>>16);
    uint32_t cls=cfg_read32(rc,1,0,0,PCI_CLASS_REVISION)>>8;
    kprintf("[VL805] PCI 01:00.0 id=%04x:%04x class=%06x\n",vendor,device,cls);
    if (vendor!=VIA_VENDOR || device!=VL805_DEVICE || cls!=XHCI_CLASS) return 0;

    uint16_t original_cmd=cfg_read16(rc,1,0,0,PCI_COMMAND);
    /* R55: temporary all-ones BAR sizing with MEM/BM decoding DISABLED.
     * Preserve firmware command flags and restore on all error paths. */
    cfg_write16(rc,1,0,0,PCI_COMMAND,
                (uint16_t)(original_cmd & ~(PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER)));
    uint32_t save0=cfg_read32(rc,1,0,0,PCI_BAR0);
    uint32_t save1=cfg_read32(rc,1,0,0,PCI_BAR1);
    cfg_write32(rc,1,0,0,PCI_BAR0,0xffffffffu);
    uint32_t mask0=cfg_read32(rc,1,0,0,PCI_BAR0);
    int is64=((save0>>1)&3u)==2u;
    uint64_t mask=(uint64_t)(mask0 & ~0x0fu);
    if (is64) {
        cfg_write32(rc,1,0,0,PCI_BAR1,0xffffffffu);
        uint32_t mask1=cfg_read32(rc,1,0,0,PCI_BAR1);
        mask |= (uint64_t)mask1<<32;
    }
    /* For a 32-bit BAR the complement must be performed in 32 bits.
     * The old 64-bit complement incorrectly invented a >4 GiB size. */
    uint64_t size=is64 ? ((~mask)+1u) :
                          (uint64_t)(uint32_t)(~(uint32_t)mask+1u);
    if (!size || size>mem->size || (size & (size-1u))) {
        cfg_write32(rc,1,0,0,PCI_BAR0,save0);
        if (is64) cfg_write32(rc,1,0,0,PCI_BAR1,save1);
        cfg_write16(rc,1,0,0,PCI_COMMAND,original_cmd);
        kprintf("[VL805] bad BAR0 size mask=%08x size=%08x\n",mask0,(uint32_t)size);
        return 0;
    }

    uint64_t bar=(mem->bus+size-1u)&~(size-1u);
    if (bar<mem->bus || bar+size>mem->bus+mem->size) {
        cfg_write32(rc,1,0,0,PCI_BAR0,save0);
        if (is64) cfg_write32(rc,1,0,0,PCI_BAR1,save1);
        cfg_write16(rc,1,0,0,PCI_COMMAND,original_cmd);
        return 0;
    }
    cfg_write32(rc,1,0,0,PCI_BAR0,(uint32_t)bar|(save0&0x0fu));
    if (is64) cfg_write32(rc,1,0,0,PCI_BAR1,(uint32_t)(bar>>32));

    uint16_t cmd=(uint16_t)(original_cmd|PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER);
    cfg_write16(rc,1,0,0,PCI_COMMAND,cmd);
    frf_usb_endpoint_command=cfg_read16(rc,1,0,0,PCI_COMMAND);
    frf_usb_endpoint_bar=cfg_read32(rc,1,0,0,PCI_BAR0);


    uintptr_t virt=mem->virt+(uintptr_t)(bar-mem->bus);
    kprintf("[VL805] BAR0 bus=%08x virt=%08x size=%08x cmd=%04x\n",
            (uint32_t)bar,(uint32_t)virt,(uint32_t)size,cmd);
    *virt_out=virt; *bytes_out=(uint32_t)size; *bus_out=(uint32_t)bar;
    return 1;
}

static int caps_snapshot(uintptr_t base,uint32_t reg_bytes,
                         uint32_t *cb,uint32_t *hs,uint32_t *hc,
                         uint32_t *db,uint32_t *rt,uint32_t *slots,
                         uint32_t *intrs,uint32_t *ports,uint32_t *ver)
{
    *cb=mmio_read32(base,XHCI_CAP_CAPBASE);
    uint32_t cl=*cb & 0xffu; *ver=(*cb>>16)&0xffffu;
    *hs=mmio_read32(base,XHCI_CAP_HCSPARAMS1);
    *hc=mmio_read32(base,XHCI_CAP_HCCPARAMS1);
    *db=mmio_read32(base,XHCI_CAP_DBOFF)&~3u;
    *rt=mmio_read32(base,XHCI_CAP_RTSOFF)&~0x1fu;
    *slots=*hs&0xffu; *intrs=(*hs>>8)&0x7ffu; *ports=(*hs>>24)&0xffu;
    if (cl<0x20u || cl>0x100u || (cl&3u) || !*ver || *ver==0xffffu) return 0;
    if (!*slots || !*intrs || !*ports) return 0;
    if (*db<0x20u || *db>=reg_bytes || *rt<0x20u || *rt>=reg_bytes) return 0;
    if ((uint64_t)cl+XHCI_OP_PORTSC1+(uint64_t)(*ports)*0x10u>reg_bytes) return 0;
    return 1;
}

static void reset_info(uint32_t n)
{
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)&g_xhci;
    for (uint32_t i=0;i<sizeof(g_xhci)/4u;i++) p[i]=0;
    g_xhci.state=FRFS_USB_BACKEND_OFFLINE;
    g_xhci.status=FRFXHCI_PROBE_NOT_RUN;
    g_xhci.probes=n;
    __asm__ volatile("dsb sy" ::: "memory");
}

const volatile struct FRFXHCIProbeInfo *frf_xhci_probe_info(void) { return &g_xhci; }

uint32_t frf_xhci_probe_run(void)
{
    reset_info(g_xhci.probes+1u);
    of_node_t *pcie=dt_find_node("/scb/pcie@7d500000");
    if (!pcie) {
        g_xhci.status=FRFXHCI_PROBE_NO_DT_NODE; frf_pci_ro_probe_status=g_xhci.status;
        g_xhci.state=FRFS_USB_BACKEND_NEEDS_CONFIG;
        return g_xhci.status;
    }

    of_property_t *reg=dt_find_property(pcie,"reg");
    if (!reg || !reg->op_value) { g_xhci.status=FRFXHCI_PROBE_BAD_REG; frf_pci_ro_probe_status=g_xhci.status; return g_xhci.status; }
    uint32_t ac=dt_get_property_value_u32(pcie->on_parent,"#address-cells",2,1);
    uint32_t sc=dt_get_property_value_u32(pcie->on_parent,"#size-cells",2,1);
    const uint32_t *cells=(const uint32_t *)reg->op_value;
    uint64_t rc_bus=cells_be(cells,ac), rc_len=cells_be(cells+ac,sc);
    uintptr_t rc=0;
    if (!current_device_virt(pcie,rc_bus,&rc)) {
        g_xhci.status=FRFXHCI_PROBE_NO_DEVICE_MAP; frf_pci_ro_probe_status=g_xhci.status;
        return g_xhci.status;
    }

    struct pcie_window mem={0}, dma={0};
    if (!pcie_get_window(pcie,"ranges",&mem) || !mem.virt ||
        !pcie_get_window(pcie,"dma-ranges",&dma)) {
        g_xhci.status=FRFXHCI_PROBE_BAD_ADDRESS; frf_pci_ro_probe_status=g_xhci.status;
        return g_xhci.status;
    }

    frf_r55_dma_cpu_base=dma.phys;
    frf_r55_dma_bus_base=dma.bus;
    frf_r55_dma_window_size=dma.size;
    kprintf("[VL805] RC bus=%08x virt=%08x len=%08x mem=%08x/%08x virt=%08x dma=%08x\n",
            (uint32_t)rc_bus,(uint32_t)rc,(uint32_t)rc_len,
            (uint32_t)mem.bus,(uint32_t)mem.size,(uint32_t)mem.virt,(uint32_t)dma.size);

    if (!root_prepare(rc,&mem,&dma)) {
        g_xhci.status=FRFXHCI_PROBE_BAD_ADDRESS; frf_pci_ro_probe_status=g_xhci.status;
        g_xhci.state=FRFS_USB_BACKEND_ERROR;
        kprintf("[VL805] root complex/link bring-up failed status=%08x rev=%08x\n",
                mmio_read32(rc,PCIE_STATUS),mmio_read32(rc,PCIE_REVISION));
        return g_xhci.status;
    }

    uintptr_t xhci=0; uint32_t bytes=0,barbus=0;
    if (!assign_vl805_bar(rc,&mem,&xhci,&bytes,&barbus)) {
        g_xhci.status=FRFXHCI_PROBE_BAD_ADDRESS; frf_pci_ro_probe_status=g_xhci.status;
        g_xhci.state=FRFS_USB_BACKEND_ERROR;
        return g_xhci.status;
    }

    /* Linux's rpi_firmware_init_vl805 checks PCI config 0x50 before
     * NOTIFY_XHCI_RESET: a non-zero firmware version means it is already
     * ready and an unconditional reset is harmful/unnecessary. */
    frf_usb_fw_before=cfg_read32(rc,1,0,0,VL805_PCI_FW_VERSION);
    frf_usb_reset_rc=0x7fffffffu; /* skipped */
    if (frf_usb_fw_before==0u) {
        int fw=frf_notify_xhci_reset(0x00100000u);
        frf_usb_reset_rc=(uint32_t)fw;
        delay_us_local(1000);
        /* The firmware reset may clear command/BAR config; re-establish
         * mappings before trying the xHCI memory registers again. */
        cfg_write32(rc,1,0,0,PCI_BAR0,frf_usb_endpoint_bar);
        uint16_t c=cfg_read16(rc,1,0,0,PCI_COMMAND);
        cfg_write16(rc,1,0,0,PCI_COMMAND,
                    (uint16_t)(c|PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER));
        frf_usb_endpoint_command=cfg_read16(rc,1,0,0,PCI_COMMAND);
        for (unsigned tries=0;tries<200u;tries++) {
            if (cfg_read32(rc,1,0,0,VL805_PCI_FW_VERSION)!=0u) break;
            delay_us_local(1000u);
        }
    }
    frf_usb_fw_after=cfg_read32(rc,1,0,0,VL805_PCI_FW_VERSION);
    kprintf("[VL805:R40] firmware before=%08x after=%08x reset=%08x BAR=%08x EPcmd=%04x\n",
       frf_usb_fw_before,frf_usb_fw_after,frf_usb_reset_rc,
       frf_usb_endpoint_bar,frf_usb_endpoint_command);

    uint32_t cb=0,hs=0,hc=0,db=0,rt=0,slots=0,intrs=0,ports=0,ver=0;
    /* Repeated reads tolerate firmware wakeup without another destructive
     * reset. Capture actual raw values even when the validation fails. */
    int caps_ok=0;
    for (unsigned tries=0u;tries<100u;tries++) {
        ++frf_usb_caps_reads;
        caps_ok=caps_snapshot(xhci,bytes,&cb,&hs,&hc,&db,&rt,
                              &slots,&intrs,&ports,&ver);
        frf_usb_caps_raw=cb;
        if (caps_ok) break;
        delay_us_local(1000u);
    }
    g_xhci.bus_base=barbus;
    g_xhci.virt_base=(uint32_t)xhci;
    g_xhci.reg_bytes=bytes;
    g_xhci.capbase=cb; g_xhci.hcsparams1=hs; g_xhci.hccparams1=hc;
    g_xhci.dboff=db; g_xhci.rtsoff=rt;
    if (!caps_ok) {
        g_xhci.status=FRFXHCI_PROBE_BAD_CAPS; frf_pci_ro_probe_status=g_xhci.status;
        g_xhci.state=FRFS_USB_BACKEND_ERROR;
        kprintf("[VL805] bad xHCI caps BAR=%08x cap=%08x hcs=%08x db=%08x rt=%08x\n",
                (uint32_t)xhci,cb,hs,db,rt);
        return g_xhci.status;
    }

    g_xhci.bus_base=barbus;
    g_xhci.phys_base=0;
    g_xhci.virt_base=(uint32_t)xhci;
    g_xhci.reg_bytes=bytes;
    g_xhci.capbase=cb; g_xhci.hcsparams1=hs; g_xhci.hccparams1=hc;
    g_xhci.dboff=db; g_xhci.rtsoff=rt;
    g_xhci.max_slots=slots; g_xhci.max_intrs=intrs; g_xhci.max_ports=ports;
    g_xhci.hci_version=ver;
    g_xhci.access_mode=FRFXHCI_ACCESS_DEVICE32;
    uint32_t caplen=cb&0xffu;
    g_xhci.portsc1=mmio_read32(xhci,caplen+XHCI_OP_PORTSC1);
    g_xhci.state=FRFS_USB_BACKEND_PROBED;
    g_xhci.status=FRFXHCI_PROBE_OK; frf_pci_ro_probe_status=g_xhci.status;
    frf_services_usb_backend_state(FRFS_USB_BACKEND_PROBED);
    __asm__ volatile("dsb sy" ::: "memory");

    kprintf("[VL805] xHCI PASS v%02x.%02x slots=%u ports=%u PORTSC1=%08x\n",
            (ver>>8)&0xffu,ver&0xffu,slots,ports,g_xhci.portsc1);
    return g_xhci.status;
}
