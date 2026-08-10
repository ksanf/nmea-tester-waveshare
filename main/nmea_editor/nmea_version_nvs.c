/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NVS persistence and transmitter refresh for NMEA profiles.
 */

#include "nmea_editor/nmea_version.h"
#include "rs485/rs485_engine.h"
#include "system/nvs_rw.h"

#include "esp_log.h"

#define NMEA_VERSION_NAMESPACE "nmea_cfg"
#define NMEA_VERSION_KEY       "protocol"

static const char *TAG = "nmea_version";

static bool version_valid_(nmea_version_t version)
{
    return version >= NMEA_VERSION_2_1 && version < NMEA_VERSION_COUNT;
}

static void mark_all_groups_dirty_(void)
{
    for (int grp = 0; grp < GRP_COUNT; ++grp) {
        rs485_engine_mark_dirty((rs485_group_t)grp);
    }
}

esp_err_t nmea_version_init(void)
{
    void *handle;
    uint8_t raw = (uint8_t)NMEA_VERSION_2_3;
    esp_err_t err = nvs_rw_open_ro(NMEA_VERSION_NAMESPACE, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        (void)nmea_version_set_runtime(NMEA_VERSION_2_3);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    err = nvs_rw_read_u8(handle, NMEA_VERSION_KEY, &raw);
    nvs_rw_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        (void)nmea_version_set_runtime(NMEA_VERSION_2_3);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    if (!version_valid_((nmea_version_t)raw)) {
        ESP_LOGW(TAG, "invalid stored profile %u, using 2.3", (unsigned)raw);
        raw = (uint8_t)NMEA_VERSION_2_3;
    }
    (void)nmea_version_set_runtime((nmea_version_t)raw);
    ESP_LOGI(TAG, "NMEA 0183 profile %s loaded", nmea_version_short_name());
    return ESP_OK;
}

esp_err_t nmea_version_set(nmea_version_t version, bool persist)
{
    void *handle;
    esp_err_t err;
    const bool changed = version != nmea_version_get();

    if (!version_valid_(version)) return ESP_ERR_INVALID_ARG;

    if (persist) {
        err = nvs_rw_open_rw(NMEA_VERSION_NAMESPACE, &handle);
        if (err != ESP_OK) return err;
        err = nvs_rw_write_u8(handle, NMEA_VERSION_KEY, (uint8_t)version);
        nvs_rw_close(handle);
        if (err != ESP_OK) return err;
    }

    (void)nmea_version_set_runtime(version);
    if (changed) mark_all_groups_dirty_();
    ESP_LOGI(TAG, "NMEA 0183 profile set to %s", nmea_version_short_name());
    return ESP_OK;
}

