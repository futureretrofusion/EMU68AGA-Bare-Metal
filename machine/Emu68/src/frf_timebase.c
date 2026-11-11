#include <stdint.h>
#include <support.h>
#include "frf_timebase.h"

static volatile uint64_t g_eff_hz;
static volatile uint64_t g_reg_hz;
static volatile uint64_t g_pct_hz;
static volatile uint64_t g_vct_hz;
static volatile uint32_t g_done;

static inline uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}

/* BCM system timer: 1 MHz, mapped through Emu68's peripheral window. */
static inline uint64_t read_sys_us(void)
{
    volatile struct {
        uint32_t lo;
        uint32_t hi;
    } *clk = (volatile void *)0xf2003004;

    uint32_t hi1, hi2, lo;
    do {
        hi1 = LE32(clk->hi);
        lo  = LE32(clk->lo);
        hi2 = LE32(clk->hi);
    } while (hi1 != hi2);

    return ((uint64_t)hi1 << 32) | lo;
}

void frf_timebase_calibrate(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_done, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    const uint64_t reg = read_cntfrq();
    const uint64_t us0 = read_sys_us();
    const uint64_t p0  = read_cntpct();
    const uint64_t v0  = read_cntvct();

    /* R81: Never strand CPU1 in this calibration loop when the Pi4 system
     * timer MMIO mapping is unavailable or returns a frozen value. All
     * previous native xHCI builds called this BEFORE controller probe, so a
     * frozen BCM timer could look exactly like "USB is powered but no HID".
     * Arm generic timer is our independent bounded fallback. */
    const uint64_t fallback_hz=(reg>=1000000u && reg<=1000000000u)?
                                reg:54000000u;
    uint64_t us1=us0;
    unsigned safety=0u;
    do {
        __asm__ volatile("yield");
        us1=read_sys_us();
        if (read_cntpct()-p0>=fallback_hz/8u || ++safety>=25000000u)
            break;
    } while ((us1-us0)<100000u);
    const int sys_timer_valid=((us1-us0)>=100000u);
    if(!sys_timer_valid)
        kprintf("[R81-TIMEBASE] BCM sys timer not advancing; using CNTPCT/CNTFRQ fallback\n");

    const uint64_t p1 = read_cntpct();
    const uint64_t v1 = read_cntvct();
    const uint64_t du = us1 - us0;
    const uint64_t dp = p1 - p0;
    const uint64_t dv = v1 - v0;

    uint64_t phz = sys_timer_valid ? (dp * 1000000ULL) / du : fallback_hz;
    uint64_t vhz = sys_timer_valid ? (dv * 1000000ULL) / du : fallback_hz;
    uint64_t eff = phz;

    /* CNTPCT and CNTVCT should share the same clock. If they disagree by
       >1%, keep CNTPCT because xHCI deadlines use CNTPCT and report both. */
    uint64_t diff = phz > vhz ? phz - vhz : vhz - phz;
    if (phz && diff <= phz / 100u)
        eff = (phz + vhz) / 2u;

    if (!eff || eff<1000000u || eff>1000000000u)
        eff=fallback_hz;

    __atomic_store_n(&g_reg_hz, reg, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pct_hz, phz, __ATOMIC_RELEASE);
    __atomic_store_n(&g_vct_hz, vhz, __ATOMIC_RELEASE);
    __atomic_store_n(&g_eff_hz, eff, __ATOMIC_RELEASE);

    kprintf("[TIMEBASE] CNTFRQ=%llu observed_pct=%llu observed_vct=%llu effective=%llu sys_us=%llu\n",
            (unsigned long long)reg,
            (unsigned long long)phz,
            (unsigned long long)vhz,
            (unsigned long long)eff,
            (unsigned long long)du);
}

uint64_t frf_timebase_hz(void)
{
    uint64_t v = __atomic_load_n(&g_eff_hz, __ATOMIC_ACQUIRE);
    if (v) return v;
    return read_cntfrq();
}

uint64_t frf_timebase_reg_hz(void)
{
    uint64_t v = __atomic_load_n(&g_reg_hz, __ATOMIC_ACQUIRE);
    return v ? v : read_cntfrq();
}

uint64_t frf_timebase_cntpct_hz(void)
{
    return __atomic_load_n(&g_pct_hz, __ATOMIC_ACQUIRE);
}

uint64_t frf_timebase_cntvct_hz(void)
{
    return __atomic_load_n(&g_vct_hz, __ATOMIC_ACQUIRE);
}
