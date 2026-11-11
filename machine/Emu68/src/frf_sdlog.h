#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint32_t frf_sdlog_last_pc;
extern volatile uint32_t frf_sdlog_last_cia_addr;
extern volatile uint32_t frf_sdlog_last_custom_addr;
extern volatile uint32_t frf_sdlog_color_value;
extern volatile uint32_t frf_sdlog_df0_value;
extern volatile uint32_t frf_sdlog_xhci_stage;
extern volatile uint32_t frf_sdlog_xhci_error;
extern volatile uint32_t frf_sdlog_post_flags;
extern volatile uint64_t frf_sdlog_first_cia_vct;
extern volatile uint64_t frf_sdlog_first_custom_vct;
extern volatile uint64_t frf_sdlog_first_color_vct;
extern volatile uint64_t frf_sdlog_first_df0_vct;
extern volatile uint64_t frf_sdlog_first_pc_vct;

int frf_sdlog_init(void);
void frf_sdlog_stage(unsigned stage, uint32_t flags);
void frf_sdlog_tick(void);
void frf_sdlog_force(void);

static inline uint64_t frf_sdlog_vct_inline(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

static inline void frf_sdlog_mark_cia(uint32_t addr)
{
    frf_sdlog_last_cia_addr = addr;
    if (!frf_sdlog_first_cia_vct)
        frf_sdlog_first_cia_vct = frf_sdlog_vct_inline();
}

static inline void frf_sdlog_mark_custom(uint32_t addr)
{
    frf_sdlog_last_custom_addr = addr;
    if (!frf_sdlog_first_custom_vct)
        frf_sdlog_first_custom_vct = frf_sdlog_vct_inline();
}

static inline void frf_sdlog_mark_color(uint32_t value)
{
    frf_sdlog_color_value = value;
    if (!frf_sdlog_first_color_vct)
        frf_sdlog_first_color_vct = frf_sdlog_vct_inline();
}

static inline void frf_sdlog_mark_df0(uint32_t value)
{
    frf_sdlog_df0_value = value;
    if (!frf_sdlog_first_df0_vct)
        frf_sdlog_first_df0_vct = frf_sdlog_vct_inline();
}

#ifdef __cplusplus
}
#endif
