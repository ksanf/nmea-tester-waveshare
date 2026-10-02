/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#define ESP_OK 0
#define ESP_MAC_WIFI_STA 0
int esp_read_mac(uint8_t *mac, int type);
