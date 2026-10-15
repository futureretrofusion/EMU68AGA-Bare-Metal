/*
 * EMU68AGA Bare Metal: read-only SDIO enumeration stage.
 * Copyright (c) 2026 Future Retro Fusion contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * No third-party Wi-Fi driver code is included.
 */
#include "frf_sdio_probe.h"
#include <string.h>

#define SDIO_OCR_READY UINT32_C(0x80000000)
#define SDIO_OCR_FN_MASK UINT32_C(0x70000000)
#define SDIO_OCR_VOLTAGES UINT32_C(0x00FFFFFF)
#define SDIO_R5_ERRORS UINT32_C(0x0000CB00)
#define SDIO_CMD5_RETRIES 200u

static int command(const frf_sdio_host *h, unsigned n, uint32_t arg,
                   frf_sdio_response response_type, uint32_t *result)
{
    uint32_t unused = 0;
    return h->command(h->ctx, n, arg, response_type,
                      result ? result : &unused);
}

static frf_sdio_status cccr_read(const frf_sdio_host *h, unsigned address,
                                 uint8_t *value)
{
    uint32_t r5 = 0;
    /* CMD52 read, function zero, 17-bit CCCR address. */
    if (command(h, 52u, (address & 0x1ffffu) << 9,
                FRF_SDIO_RESP_R5, &r5) != 0)
        return FRF_SDIO_HOST_FAILURE;
    if (r5 & SDIO_R5_ERRORS) return FRF_SDIO_R5_ERROR;
    *value = (uint8_t)r5;
    return FRF_SDIO_OK;
}

frf_sdio_status frf_sdio_probe(const frf_sdio_host *h, uint32_t host_ocr,
                               frf_sdio_probe_info *out)
{
    uint32_t r4 = 0, r6 = 0, common_ocr;
    frf_sdio_status err;
    unsigned i;

    if (!h || !out || !h->power_on || !h->set_clock_khz ||
        !h->command || !h->delay_ms || !(host_ocr & SDIO_OCR_VOLTAGES))
        return FRF_SDIO_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (h->power_on(h->ctx) != 0 ||
        h->set_clock_khz(h->ctx, 400u) != 0 ||
        command(h, 0u, 0u, FRF_SDIO_RESP_NONE, 0) != 0 ||
        command(h, 5u, 0u, FRF_SDIO_RESP_R4, &r4) != 0)
        return FRF_SDIO_HOST_FAILURE;

    out->function_count = (uint8_t)((r4 & SDIO_OCR_FN_MASK) >> 28);
    if (!out->function_count) return FRF_SDIO_NO_FUNCTIONS;

    common_ocr = r4 & host_ocr & SDIO_OCR_VOLTAGES;
    if (!common_ocr) return FRF_SDIO_BAD_VOLTAGE;

    for (i = 0; i < SDIO_CMD5_RETRIES; ++i) {
        if (command(h, 5u, common_ocr, FRF_SDIO_RESP_R4, &r4) != 0)
            return FRF_SDIO_HOST_FAILURE;
        if (r4 & SDIO_OCR_READY) break;
        h->delay_ms(h->ctx, 10u);
    }
    if (!(r4 & SDIO_OCR_READY)) return FRF_SDIO_TIMEOUT;
    out->card_ocr = r4;

    if (command(h, 3u, 0u, FRF_SDIO_RESP_R6, &r6) != 0)
        return FRF_SDIO_HOST_FAILURE;
    out->rca = (uint16_t)(r6 >> 16);
    if (!out->rca) return FRF_SDIO_NO_RCA;
    if (command(h, 7u, (uint32_t)out->rca << 16,
                FRF_SDIO_RESP_R1, 0) != 0)
        return FRF_SDIO_HOST_FAILURE;

    err = cccr_read(h, 0x00u, &out->cccr_sdio_version);
    if (err != FRF_SDIO_OK) return err;
    err = cccr_read(h, 0x02u, &out->cccr_io_enable);
    if (err != FRF_SDIO_OK) return err;
    err = cccr_read(h, 0x03u, &out->cccr_io_ready);
    if (err != FRF_SDIO_OK) return err;
    err = cccr_read(h, 0x07u, &out->cccr_bus_interface);
    return err;
}

const char *frf_sdio_status_text(frf_sdio_status code)
{
    switch (code) {
    case FRF_SDIO_OK: return "SDIO card enumerated (not connected to Wi-Fi)";
    case FRF_SDIO_BAD_ARGUMENT: return "Invalid SDIO host/voltage configuration";
    case FRF_SDIO_HOST_FAILURE: return "SDIO host controller command failed";
    case FRF_SDIO_NO_FUNCTIONS: return "No SDIO I/O functions reported";
    case FRF_SDIO_BAD_VOLTAGE: return "No compatible SDIO voltage window";
    case FRF_SDIO_TIMEOUT: return "SDIO device did not report ready";
    case FRF_SDIO_NO_RCA: return "SDIO device gave no RCA";
    case FRF_SDIO_R5_ERROR: return "SDIO device reported CMD52 error";
    default: return "Unknown SDIO status";
    }
}
