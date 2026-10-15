#include "frf_native_bus.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned read_calls, write_calls;
static int passthrough_read(void *ctx,uint32_t a,unsigned size,uint64_t *out)
{
    (void)ctx;
    ++read_calls;
    if (a==0xda001c && size==1) { *out=0x50; return 1; }
    return 0;
}
static int passthrough_write(void *ctx,uint32_t a,unsigned size,uint64_t v)
{
    (void)ctx;
    ++write_calls;
    return a==0xda001c && size==1 && v==0xec;
}
int main(void)
{
    uint64_t v=0;
    frf_native_bus_stats stats;
    frf_native_bus_set_backend(NULL);
    frf_native_bus_reset();
    assert(frf_native_bus_read(0xda8000,1,&v) && v==0); /* No card */
    assert(frf_native_bus_read(0xda9000,1,&v) && v==0); /* No IRQ */
    assert(frf_native_bus_read(0xda001c,1,&v) && v==0x7f); /* IDE not busy */
    assert(frf_native_bus_read(0xda001d,1,&v) && v==0x7f);
    assert(frf_native_bus_read(0xda101c,1,&v) && v==0x7f);
    assert(frf_native_bus_read(0xda0005,1,&v) && v==0xff);
    assert(frf_native_bus_read(0xda8000,2,&v) && v==0x00ff);
    assert(frf_native_bus_write(0xda001d,1,0xec));
    assert(frf_native_bus_read(0xda9000,1,&v) && v==0); /* No phantom IRQ */
    assert(frf_native_bus_write(0xdaa000,1,0xc0));
    assert(frf_native_bus_read(0xdaa000,1,&v) && v==0xc0);
    assert(frf_native_bus_write(0xdab000,1,0xff));
    assert(frf_native_bus_read(0xdab000,1,&v) && v==0x0f);
    assert(frf_native_bus_read(0xda9000,1,&v) && v==0);
    assert(!frf_native_bus_read(0xdff01e,2,&v)); /* core chipset stays core */
    assert(!frf_native_bus_read(0x600000,1,&v)); /* future PC card */
    assert(!frf_native_bus_write(0xda8fff,1,1));
    assert(!frf_native_bus_read(0xda8000,16,&v));
    { frf_native_backend b={NULL,passthrough_read,passthrough_write};
      frf_native_bus_set_backend(&b);
    }
    assert(frf_native_bus_read(0xda001c,1,&v) && v==0x50);
    assert(frf_native_bus_write(0xda001c,1,0xec));
    assert(frf_native_bus_read(0xda9000,1,&v) && v==0);
    stats=frf_native_bus_get_stats();
    assert(stats.passthrough_reads==1 && stats.passthrough_writes==1);
    assert(read_calls>0 && write_calls>0);
    frf_native_bus_set_backend(NULL);
    frf_native_bus_reset();
    assert(frf_native_bus_read(0xda001c,1,&v) && v==0x7f);
    puts("PASS: absent Gayle PCMCIA, no IRQ, IDE no-BSY response,");
    puts("      multi-byte bus lanes, bounded synchronous access,");
    puts("      optional future real hardware backend and reset.");
    return 0;
}
