/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Centralised NVS read/write wrapper.
 */

#pragma once

#include <esp_err.h>
#include <nvs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Open NVS namespace in read-only mode.
 */
esp_err_t nvs_rw_open_ro(const char *ns, void **handle);

/**
 * @brief Open NVS namespace in read-write mode.
 */
esp_err_t nvs_rw_open_rw(const char *ns, void **handle);

/**
 * @brief Close NVS handle.
 */
void nvs_rw_close(void *handle);

/**
 * @brief Read uint8_t from NVS. Returns ESP_ERR_NOT_FOUND if key missing.
 */
esp_err_t nvs_rw_read_u8(void *handle, const char *key, uint8_t *val);

/**
 * @brief Read blob from NVS.
 * @param[out] len actual length read
 */
esp_err_t nvs_rw_read_blob(void *handle, const char *key, void *buf, size_t *len);

/**
 * @brief Write uint8_t and commit.
 */
esp_err_t nvs_rw_write_u8(void *handle, const char *key, uint8_t val);

/**
 * @brief Write blob and commit.
 */
esp_err_t nvs_rw_write_blob(void *handle, const char *key, const void *buf, size_t len);
