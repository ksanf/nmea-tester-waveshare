/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Compatibility wrapper around wifi_manager.
 */

#include "system/wifi_ap.h"
#include "wifi/wifi_manager.h"

esp_err_t wifi_ap_init(void)
{
    return wifi_manager_init();
}

esp_err_t wifi_ap_start(void)
{
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) return err;
    if (wifi_manager_is_started()) return ESP_OK;
    return wifi_manager_start();
}

esp_err_t wifi_ap_stop(void)
{
    return wifi_manager_deinit();
}

bool wifi_ap_is_enabled(void)
{
    return wifi_manager_is_started();
}

bool wifi_ap_get_ip(char *buf, size_t buf_sz)
{
    return wifi_manager_get_ip_str(buf, buf_sz);
}
