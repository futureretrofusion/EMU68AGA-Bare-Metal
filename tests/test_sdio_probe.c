/*
 * Hardware-free SDIO command sequence tests. Run from repository root:
 * cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
 *    -Imachine/Emu68/src/frf_wifi \
 *    machine/Emu68/src/frf_wifi/frf_sdio_probe.c \
 *    tests/test_sdio_probe.c -o /tmp/test_sdio_probe && /tmp/test_sdio_probe
 */
#include "frf_sdio_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned calls, delays, cccr_reads;
    unsigned timeout, error, no_sdio, bad_voltage;
} fixture;
static int power(void *ctx) {(void)ctx; return 0;}
static int clock_khz(void *ctx, unsigned khz)
{ (void)ctx; assert(khz == 400u); return 0; }
static void delay(void *ctx, unsigned ms)
{ ((fixture *)ctx)->delays += ms; }
static int cmd(void *ctx, unsigned n, uint32_t arg,
               frf_sdio_response response_type, uint32_t *r)
{
    fixture *f = ctx;
    ++f->calls;
    switch (n) {
    case 0:
        assert(response_type == FRF_SDIO_RESP_NONE);
        return 0;
    case 5:
        assert(response_type == FRF_SDIO_RESP_R4);
        if (f->no_sdio) { *r = 0; return 0; }
        *r = (2u << 28) | (f->bad_voltage ? 0x40u : 0x00300000u);
        if (arg && !f->timeout) *r |= 0x80000000u;
        return 0;
    case 3:
        assert(response_type == FRF_SDIO_RESP_R6);
        *r = 0x12340000u;
        return 0;
    case 7:
        assert(response_type == FRF_SDIO_RESP_R1 && arg == 0x12340000u);
        *r = 0;
        return 0;
    case 52:
        assert(response_type == FRF_SDIO_RESP_R5);
        if (f->error) { *r = 0x800u; return 0; }
        switch ((arg >> 9) & 0x1ffffu) {
        case 0: *r = 0x32u; break;
        case 2: *r = 0; break;
        case 3: *r = 0; break;
        case 7: *r = 0; break;
        default: assert(0);
        }
        ++f->cccr_reads;
        return 0;
    default:
        assert(0);
        return -1;
    }
}
static frf_sdio_status run(fixture *f, frf_sdio_probe_info *d)
{
    frf_sdio_host h = {f, power, clock_khz, cmd, delay};
    return frf_sdio_probe(&h, 0x00ff8000u, d);
}
int main(void)
{
    fixture f; frf_sdio_probe_info d;
    memset(&f, 0, sizeof(f));
    assert(run(&f, &d) == FRF_SDIO_OK);
    assert(d.function_count == 2 && d.rca == 0x1234 &&
           d.cccr_sdio_version == 0x32 && f.cccr_reads == 4);
    puts("PASS: SDIO identification");
    memset(&f, 0, sizeof(f)); f.timeout = 1;
    assert(run(&f, &d) == FRF_SDIO_TIMEOUT && f.delays == 2000);
    puts("PASS: bounded CMD5 timeout");
    memset(&f, 0, sizeof(f)); f.no_sdio = 1;
    assert(run(&f, &d) == FRF_SDIO_NO_FUNCTIONS);
    puts("PASS: absent SDIO card");
    memset(&f, 0, sizeof(f)); f.bad_voltage = 1;
    assert(run(&f, &d) == FRF_SDIO_BAD_VOLTAGE);
    puts("PASS: incompatible voltage window");
    memset(&f, 0, sizeof(f)); f.error = 1;
    assert(run(&f, &d) == FRF_SDIO_R5_ERROR);
    puts("PASS: CMD52 error");
    puts("All five SDIO mock tests passed");
    return 0;
}
