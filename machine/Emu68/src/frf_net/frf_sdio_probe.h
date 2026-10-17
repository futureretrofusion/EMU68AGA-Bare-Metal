/*
 * EMU68AGA Bare Metal: conservative SDIO card enumeration / Wi-Fi probe.
 * Copyright (c) 2026 Future Retro Fusion contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Controller-INDEPENDENT code, not a Wi-Fi driver. It cannot join networks.
 * The Pi 3A+ Arasan SD1 controller callback implementation is pending.
 */
#ifndef FRF_SDIO_PROBE_H
#define FRF_SDIO_PROBE_H
#include <stdint.h>

/* Command results are normalized native SD response words, no CRC/index. */
typedef enum {
    FRF_SDIO_RESP_NONE = 0,
    FRF_SDIO_RESP_R1,
    FRF_SDIO_RESP_R4,
    FRF_SDIO_RESP_R5,
    FRF_SDIO_RESP_R6
} frf_sdio_response;

typedef struct {
    void *ctx;
    /* Return 0 on success, negative on controller failure. */
    int (*power_on)(void *ctx);
    int (*set_clock_khz)(void *ctx, unsigned khz);
    int (*command)(void *ctx, unsigned cmd_index, uint32_t argument,
                   frf_sdio_response type, uint32_t *response);
    void (*delay_ms)(void *ctx, unsigned ms);
} frf_sdio_host;

typedef enum {
    FRF_SDIO_OK = 0,
    FRF_SDIO_BAD_ARGUMENT,
    FRF_SDIO_HOST_FAILURE,
    FRF_SDIO_NO_FUNCTIONS,
    FRF_SDIO_BAD_VOLTAGE,
    FRF_SDIO_TIMEOUT,
    FRF_SDIO_NO_RCA,
    FRF_SDIO_R5_ERROR
} frf_sdio_status;

typedef struct {
    uint32_t card_ocr;
    uint16_t rca;
    uint8_t function_count;
    uint8_t cccr_sdio_version;
    uint8_t cccr_io_enable;
    uint8_t cccr_io_ready;
    uint8_t cccr_bus_interface;
} frf_sdio_probe_info;

/* Read-only probing: no CCCR writes, firmware loads or Wi-Fi association.
 * Must only be used with the Wi-Fi SD1 controller, never boot microSD SD0.
 */
frf_sdio_status frf_sdio_probe(const frf_sdio_host *host,
                              uint32_t host_ocr,
                              frf_sdio_probe_info *out);
const char *frf_sdio_status_text(frf_sdio_status code);
#endif
