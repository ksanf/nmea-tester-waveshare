/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief Centralized NVS read/write wrapper.
 */

#include "system/nvs_rw.h"

#include <nvs.h>

esp_err_t nvs_rw_open_ro(const char *ns, void **handle)
{
    if (!ns || !handle) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    *handle = (void *)(uintptr_t)h;
    return ESP_OK;
}

esp_err_t nvs_rw_open_rw(const char *ns, void **handle)
{
    if (!ns || !handle) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    *handle = (void *)(uintptr_t)h;
    return ESP_OK;
}

void nvs_rw_close(void *handle)
{
    if (handle) nvs_close((nvs_handle_t)(uintptr_t)handle);
}

esp_err_t nvs_rw_read_u8(void *handle, const char *key, uint8_t *val)
{
    if (!handle || !key || !val) return ESP_ERR_INVALID_ARG;
    return nvs_get_u8((nvs_handle_t)(uintptr_t)handle, key, val);
}

esp_err_t nvs_rw_read_blob(void *handle, const char *key, void *buf, size_t *len)
{
    if (!handle || !key || !buf || !len) return ESP_ERR_INVALID_ARG;
    return nvs_get_blob((nvs_handle_t)(uintptr_t)handle, key, buf, len);
}

esp_err_t nvs_rw_write_u8(void *handle, const char *key, uint8_t val)
{
    if (!handle || !key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h = (nvs_handle_t)(uintptr_t)handle;

    esp_err_t err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

esp_err_t nvs_rw_write_blob(void *handle, const char *key, const void *buf, size_t len)
{
    if (!handle || !key || !buf || !len) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h = (nvs_handle_t)(uintptr_t)handle;

    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}
