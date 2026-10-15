/*
 * FRF bare-metal USB environment for USPi on Emu68/Pi3.
 * USB host IRQ/timers live entirely on CPU1.
 */
#include <stdint.h>
#include <stdarg.h>
#include "support.h"
#include "mmu.h"
#include "tlsf.h"
#include "frf_usb.h"
#include <uspios.h>

#define FRF_USB_HEAP_BYTES (512u * 1024u)
#define FRF_USB_TIMER_MAX  32
#define ARM_PERI_BASE      0xF2000000UL
#define IRQ_BASE           (ARM_PERI_BASE + 0xB200)
#define IRQ_PENDING_1      (IRQ_BASE + 0x204)
#define IRQ_ENABLE_1       (IRQ_BASE + 0x210)
#define IRQ_DISABLE_1      (IRQ_BASE + 0x21C)
#define IRQ_DISABLE_2      (IRQ_BASE + 0x220)
#define IRQ_DISABLE_BASIC  (IRQ_BASE + 0x224)
extern uintptr_t local_intc_base;
#define USB_IRQ            9u

struct frf_timer {
    uint64_t due;
    TKernelTimerHandler *handler;
    void *param;
    void *context;
    uint16_t generation;
    uint8_t active;
};

static void *usb_heap_block;
static void *usb_tlsf;
static struct frf_timer timers[FRF_USB_TIMER_MAX];
static TInterruptHandler *usb_irq_handler;
static void *usb_irq_param;
static volatile uint32_t usb_irq_count;

static inline uint64_t cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}
static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}
static inline uint64_t irq_save(void)
{
    uint64_t d;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(d) :: "memory");
    return d;
}
static inline void irq_restore(uint64_t d)
{
    if (!(d & (1u << 7)))
        __asm__ volatile("msr daifclr, #2" ::: "memory");
}

int frf_uspi_env_init(void)
{
    if (usb_tlsf) return 1;
    usb_heap_block = tlsf_malloc_aligned(tlsf, FRF_USB_HEAP_BYTES, 64);
    if (!usb_heap_block) {
        kprintf("[USB] cannot allocate %u KiB private heap\n", FRF_USB_HEAP_BYTES >> 10);
        return 0;
    }
    usb_tlsf = tlsf_init_with_memory(usb_heap_block, FRF_USB_HEAP_BYTES);
    if (!usb_tlsf) {
        kprintf("[USB] private TLSF init failed\n");
        return 0;
    }
    for (unsigned i = 0; i < FRF_USB_TIMER_MAX; ++i) {
        timers[i].active = 0;
        timers[i].generation = 1;
    }
    kprintf("[USB] private heap %u KiB at %p phys=%p\n",
            FRF_USB_HEAP_BYTES >> 10, usb_heap_block,
            (void *)mmu_virt2phys((uintptr_t)usb_heap_block));
    return 1;
}

void *frf_uspi_malloc(unsigned nSize)
{
    if (!usb_tlsf) return 0;
    return tlsf_malloc_aligned(usb_tlsf, nSize ? nSize : 1, 16);
}

void frf_uspi_free(void *pBlock)
{
    if (usb_tlsf && pBlock) tlsf_free(usb_tlsf, pBlock);
}

unsigned frf_uspi_bus_address(unsigned long virt)
{
    uintptr_t phys = mmu_virt2phys((uintptr_t)virt);
    if (phys == (uintptr_t)-1) return 0;
    return (unsigned)((phys & 0x3FFFFFFFUL) | 0xC0000000UL);
}

void MsDelay(unsigned nMilliSeconds)
{
    uint64_t f = cntfrq();
    uint64_t end = cntvct() + (f * nMilliSeconds) / 1000u;
    while (cntvct() < end) __asm__ volatile("yield");
}

void usDelay(unsigned nMicroSeconds)
{
    uint64_t f = cntfrq();
    uint64_t ticks = (f * nMicroSeconds + 999999u) / 1000000u;
    uint64_t end = cntvct() + (ticks ? ticks : 1);
    while (cntvct() < end) __asm__ volatile("yield");
}

unsigned StartKernelTimer(unsigned nHzDelay, TKernelTimerHandler *pHandler,
                          void *pParam, void *pContext)
{
    if (!pHandler) return 0;
    if (!nHzDelay) nHzDelay = 1;
    uint64_t d = irq_save();
    for (unsigned i = 0; i < FRF_USB_TIMER_MAX; ++i) {
        if (!timers[i].active) {
            uint16_t g = ++timers[i].generation;
            if (!g) g = ++timers[i].generation;
            timers[i].handler = pHandler;
            timers[i].param = pParam;
            timers[i].context = pContext;
            timers[i].due = cntvct() + (cntfrq() * nHzDelay + HZ - 1) / HZ;
            timers[i].active = 1;
            unsigned h = ((unsigned)g << 8) | (i + 1);
            irq_restore(d);
            return h;
        }
    }
    irq_restore(d);
    kprintf("[USB] timer table full\n");
    return 0;
}

void CancelKernelTimer(unsigned hTimer)
{
    if (!hTimer) return;
    unsigned slot = (hTimer & 0xFFu);
    uint16_t gen = (uint16_t)(hTimer >> 8);
    if (!slot || slot > FRF_USB_TIMER_MAX) return;
    uint64_t d = irq_save();
    struct frf_timer *t = &timers[slot - 1];
    if (t->active && t->generation == gen) t->active = 0;
    irq_restore(d);
}

void frf_uspi_timer_service(void)
{
    for (;;) {
        uint64_t d = irq_save();
        uint64_t now = cntvct();
        int found = -1;
        uint64_t best = ~(uint64_t)0;
        for (unsigned i = 0; i < FRF_USB_TIMER_MAX; ++i) {
            if (timers[i].active && timers[i].due <= now && timers[i].due < best) {
                found = (int)i;
                best = timers[i].due;
            }
        }
        if (found < 0) {
            irq_restore(d);
            return;
        }
        struct frf_timer t = timers[found];
        timers[found].active = 0;
        unsigned h = ((unsigned)t.generation << 8) | ((unsigned)found + 1);
        /* Timer callbacks in USPi are kernel-timer/IRQ style: keep IRQ masked. */
        t.handler((TKernelTimerHandle)h, t.param, t.context);
        irq_restore(d);
    }
}

void ConnectInterrupt(unsigned nIRQ, TInterruptHandler *pHandler, void *pParam)
{
    if (nIRQ != USB_IRQ || !pHandler) {
        kprintf("[USB] refusing unsupported IRQ %u\n", nIRQ);
        return;
    }
    usb_irq_handler = pHandler;
    usb_irq_param = pParam;
    kprintf("[USB] DWC2 IRQ9 registered for dedicated CPU1 polling\n");
}

void frf_uspi_poll(void)
{
    if (usb_irq_handler)
        usb_irq_handler(usb_irq_param);
}

int frf_uspi_irq_dispatch(void)
{
    return 0;
}

/* Raspberry Pi property mailbox, channel 8. */
#define MBOX_READ   (ARM_PERI_BASE + 0xB880)
#define MBOX_STATUS (ARM_PERI_BASE + 0xB898)
#define MBOX_WRITE  (ARM_PERI_BASE + 0xB8A0)
#define MBOX_FULL   (1u << 31)
#define MBOX_EMPTY  (1u << 30)

static volatile uint32_t power_req[8] __attribute__((aligned(16)));

static void mbox_send(uint32_t data)
{
    while (rd32le(MBOX_STATUS) & MBOX_FULL) __asm__ volatile("yield");
    wr32le(MBOX_WRITE, (data & ~15u) | 8u);
}
static uint32_t mbox_recv(void)
{
    for (;;) {
        while (rd32le(MBOX_STATUS) & MBOX_EMPTY) __asm__ volatile("yield");
        uint32_t r = rd32le(MBOX_READ);
        if ((r & 15u) == 8u) return r & ~15u;
    }
}

int SetPowerStateOn(unsigned nDeviceId)
{
    power_req[0] = __builtin_bswap32(8u * 4u);
    power_req[1] = 0;
    power_req[2] = __builtin_bswap32(0x00028001u); /* SET_POWER_STATE */
    power_req[3] = __builtin_bswap32(8u);
    power_req[4] = 0;
    power_req[5] = __builtin_bswap32(nDeviceId);
    power_req[6] = __builtin_bswap32(3u);          /* ON | WAIT */
    power_req[7] = 0;
    arm_flush_cache((uintptr_t)power_req, sizeof power_req);
    uintptr_t phys = mmu_virt2phys((uintptr_t)power_req);
    mbox_send((uint32_t)phys);
    (void)mbox_recv();
    arm_dcache_invalidate((uintptr_t)power_req, sizeof power_req);
    uint32_t state = __builtin_bswap32(power_req[6]);
    int ok = !(state & 2u) && (state & 1u);
    kprintf("[USB] firmware USB power: %s (state=%08x)\n", ok ? "on" : "FAILED", state);
    return ok;
}

int GetMACAddress(unsigned char Buffer[6])
{
    (void)Buffer;
    return 0;       /* networking intentionally absent in input bring-up */
}

void LogWrite(const char *pSource, unsigned Severity, const char *pMessage, ...)
{
    static const char sev[] = "?EWND";
    char c = Severity < sizeof(sev)-1 ? sev[Severity] : '?';
    kprintf("[USB:%c:%s] ", c, pSource ? pSource : "-");
    va_list ap;
    va_start(ap, pMessage);
    vkprintf(pMessage, ap);
    va_end(ap);
    kprintf("\n");
}
