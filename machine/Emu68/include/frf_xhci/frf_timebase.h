#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void frf_timebase_calibrate(void);
uint64_t frf_timebase_hz(void);
uint64_t frf_timebase_reg_hz(void);
uint64_t frf_timebase_cntpct_hz(void);
uint64_t frf_timebase_cntvct_hz(void);
#ifdef __cplusplus
}
#endif
