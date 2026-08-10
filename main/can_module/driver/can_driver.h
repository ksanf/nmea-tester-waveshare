/*
 * Copyright (c) 2026 S. Zhurba
 * Portions based on esp32_can, Copyright (c) 2018 Collin Kidder
 * SPDX-License-Identifier: MIT
 *
 * @brief   ESP32 CAN driver based on the integrated TWAI controller (ESP-IDF 5.5).
 */

#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/twai.h"

/* Increased to reduce Fast-Packet frame loss during long terminal bursts. */
#define CAN_TX_BUFFER_SIZE 64
#define CAN_RX_BUFFER_SIZE 128

/* Classic CAN configuration. ESP32-S3 TWAI does not support CAN FD. */
typedef struct {
    uint32_t nom_speed;     /* Nominal bit rate in bit/s, e.g. 250000. */
    bool listen_only;       /* Receive without transmitting or acknowledging. */
} can_config_t;

/**
 * @brief Initialize the CAN driver
 * @param config Pointer to the CAN configuration
 * @return ESP_OK on success, otherwise an error code
 *
 * Allocates TWAI resources, configures pins, and starts the bus-off watchdog.
 * Starts a watchdog task to monitor bus-off conditions.
 * Serves as the base CAN layer for the project's bridges and protocol modules.
 */
esp_err_t can_driver_init(const can_config_t *config);

/**
 * @brief Deinitialize the CAN driver
 *
 * Stops TWAI, deletes tasks, and releases resources.
 * @return ESP_OK if the driver was stopped or was already stopped.
 */
esp_err_t can_driver_deinit(void);

/**
 * @brief Transmit a CAN frame
 * @param msg Pointer to the TWAI message
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK on success
 */
esp_err_t can_driver_send(const twai_message_t *msg, uint32_t timeout_ms);
/**
 * @brief Receive a CAN frame
 * @param msg Pointer to the destination TWAI message
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success, or ESP_ERR_TIMEOUT when no data is available
 */
esp_err_t can_driver_receive(twai_message_t *msg, uint32_t timeout_ms);

/**
 * @brief Configure CAN filters
 * @param filter TWAI hardware filter configuration
 * @return ESP_OK on success
 */
esp_err_t can_driver_set_filters(const twai_filter_config_t *filter);

/**
 * @brief Get CAN status
 * @param status Pointer to the TWAI status structure
 * @return ESP_OK on success
 */
esp_err_t can_driver_get_status(twai_status_info_t *status);

/**
 * @brief Check for available RX frames
 * @return Number of available frames
 */
uint32_t can_driver_available(void);

#endif // CAN_DRIVER_H
