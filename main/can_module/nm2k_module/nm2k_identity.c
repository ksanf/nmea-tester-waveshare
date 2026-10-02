/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "nm2k_identity.h"

#include <string.h>
#include "esp_mac.h"

esp_err_t nm2k_identity_read(nm2k_identity_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    uint8_t mac[6];
    const esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;

    /* Include all six bytes: low MAC bits alone ignore the manufacturer prefix.
     * Unsigned FNV-1a arithmetic makes the result independent of CPU endianness. */
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned i = 0; i < sizeof(mac); ++i)
        hash = (hash ^ mac[i]) * UINT32_C(16777619);
    out->identity_number = hash & NM2K_IDENTITY_MASK;

    /* Preserve the existing monitor profile: manufacturer 2046, function 130,
     * marine industry group 4 and arbitrary-address-capable. All instances,
     * vehicle system and reserved bits remain zero. */
    out->name = ((uint64_t)out->identity_number) |
                (UINT64_C(2046) << 21) |
                (UINT64_C(130) << 40) |
                (UINT64_C(4) << 60) |
                (UINT64_C(1) << 63);

    static const char hex[] = "0123456789ABCDEF";
    memcpy(out->serial, "NM2K", 4);
    for (unsigned i = 0; i < sizeof(mac); ++i) {
        out->serial[4 + 2 * i] = hex[mac[i] >> 4];
        out->serial[5 + 2 * i] = hex[mac[i] & 0x0fu];
    }
    out->serial[NM2K_IDENTITY_SERIAL_SIZE - 1] = '\0';
    return ESP_OK;
}
