/*
 * Copyright (c) 2026 S. Zhurba
 * Portions based on esp32_can, Copyright (c) 2018 Collin Kidder
 * SPDX-License-Identifier: MIT
 *
 * @brief   ESP32 CAN driver implementation based on TWAI (ESP-IDF 5.5).
 */

#include "can_driver.h"
#define CFG_LOG_MODULE LOG_CFG_CAN_DRIVER
#include "bridge_can_config.h"
#include "config_pins.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include <stdatomic.h>
#include <stdlib.h>

// Logging tag
static const char *TAG = "can_driver";

static inline void _hex(const uint8_t *d, size_t n, char *out, size_t cap) {
    size_t o=0;
    for (size_t i=0;i<n && o+3<cap;i++) o += snprintf(out+o, cap-o, "%02X ", d[i]);
    if (o<cap) out[o]=0;
}
// Static variables
static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static can_config_t current_config; // Current configuration
static _Atomic(TaskHandle_t) watchdog_task_handle = NULL; // Watchdog task
static atomic_bool s_watchdog_stop = ATOMIC_VAR_INIT(false);

#define WATCHDOG_STOP_WAIT_MS 500U
#define WATCHDOG_STOP_POLL_MS  10U

static esp_err_t watchdog_start_(void);
static bool watchdog_stop_(void);

static bool watchdog_stop_requested_(void)
{
    return atomic_load_explicit(&s_watchdog_stop, memory_order_acquire);
}

static bool can_is_initialized_(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire);
}

// Select timing parameters for the requested bit rate.
static twai_timing_config_t get_timing(uint32_t speed) {
    switch (speed) {
        case 1000000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_1MBITS();
        case 800000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_800KBITS();
        case 500000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_500KBITS();
        case 250000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_250KBITS();
        case 125000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_125KBITS();
        case 100000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_100KBITS();
        case 50000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_50KBITS();
        case 25000: return (twai_timing_config_t) TWAI_TIMING_CONFIG_25KBITS();
        default: {
            ESP_LOGW(TAG, "Unsupported speed: %u, using default 500K", speed);
            return (twai_timing_config_t) TWAI_TIMING_CONFIG_500KBITS();
        }
    }
}

// Watchdog task for bus-off monitoring
static void watchdog_task(void *pvParameters) {
    (void)pvParameters;
    bool recovery_pending = false;

    /* watchdog_start_() publishes our handle before releasing this task. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    while (!watchdog_stop_requested_()) {
        twai_status_info_t status;
        if (twai_get_status_info(&status) == ESP_OK) {
            if (status.state == TWAI_STATE_BUS_OFF && !recovery_pending) {
                ESP_LOGW(TAG, "CAN bus-off detected, initiating recovery");
                esp_err_t err = twai_initiate_recovery();
                if (err == ESP_OK) {
                    recovery_pending = true;
                } else {
                    ESP_LOGE(TAG, "CAN recovery start failed: %s", esp_err_to_name(err));
                }
            } else if (status.state == TWAI_STATE_STOPPED && recovery_pending) {
                esp_err_t err = twai_start();
                if (err == ESP_OK) {
                    recovery_pending = false;
                    ESP_LOGI(TAG, "CAN restarted after bus-off recovery");
                } else {
                    ESP_LOGE(TAG, "CAN restart after recovery failed: %s",
                             esp_err_to_name(err));
                }
            } else if (status.state == TWAI_STATE_RUNNING) {
                recovery_pending = false;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Watchdog task exiting gracefully");
    atomic_store_explicit(&watchdog_task_handle, NULL, memory_order_release);
    vTaskDelete(NULL);
}

static esp_err_t watchdog_start_(void)
{
    TaskHandle_t created_task = NULL;

    if (atomic_load_explicit(&watchdog_task_handle, memory_order_acquire)) {
        return watchdog_stop_requested_() ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    atomic_store_explicit(&s_watchdog_stop, false, memory_order_release);
    if (xTaskCreate(watchdog_task, "can_watchdog", 2048, NULL, 5,
                    &created_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create watchdog task");
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&watchdog_task_handle, created_task,
                          memory_order_release);
    xTaskNotifyGive(created_task);
    return ESP_OK;
}

static bool watchdog_stop_(void)
{
    TaskHandle_t task = atomic_load_explicit(&watchdog_task_handle,
                                             memory_order_acquire);
    if (!task) {
        atomic_store_explicit(&s_watchdog_stop, false, memory_order_release);
        return true;
    }

    atomic_store_explicit(&s_watchdog_stop, true, memory_order_release);
    xTaskNotifyGive(task);

    for (uint32_t waited_ms = 0;
         waited_ms < WATCHDOG_STOP_WAIT_MS &&
             atomic_load_explicit(&watchdog_task_handle,
                                  memory_order_acquire);
         waited_ms += WATCHDOG_STOP_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_STOP_POLL_MS));
    }

    if (atomic_load_explicit(&watchdog_task_handle, memory_order_acquire)) {
        ESP_LOGE(TAG, "Watchdog stop timed out; TWAI kept installed");
        return false;
    }

    atomic_store_explicit(&s_watchdog_stop, false, memory_order_release);
    return true;
}

esp_err_t can_driver_init(const can_config_t *config) {
    if (can_is_initialized_()) {
        ESP_LOGW(TAG, "CAN is already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        ESP_LOGE(TAG, "CAN configuration was not provided");
        return ESP_ERR_INVALID_ARG;
    }

    // Copy the configuration.
    current_config = *config;

    // General TWAI configuration
    twai_general_config_t g_cfg = {
        .mode = config->listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL,
        .tx_io = PIN_CAN_TX,
        .rx_io = PIN_CAN_RX,
        .clkout_io = TWAI_IO_UNUSED,
        .bus_off_io = TWAI_IO_UNUSED,
        .tx_queue_len = CAN_TX_BUFFER_SIZE,
        .rx_queue_len = CAN_RX_BUFFER_SIZE,
        .alerts_enabled = TWAI_ALERT_ALL,
        .clkout_divider = 0,
        .intr_flags = ESP_INTR_FLAG_LOWMED
    };

    // Timing
    twai_timing_config_t t_cfg = get_timing(config->nom_speed);

    // Accept all frames by default.
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install the driver.
    esp_err_t err = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start TWAI.
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return err;
    }

    // Create the watchdog task.
    err = watchdog_start_();
    if (err != ESP_OK) {
        twai_stop();
        twai_driver_uninstall();
        return err;
    }

    // Configure the SE pin when present on the board.
#if PIN_CAN_SE >= 0
    gpio_set_direction(PIN_CAN_SE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CAN_SE, 0);
#endif

    atomic_store_explicit(&s_initialized, true, memory_order_release);
    ESP_LOGI(TAG, "CAN driver initialized, nom_speed: %u, listen_only: %d",
             config->nom_speed, config->listen_only);
    return ESP_OK;
}

esp_err_t can_driver_deinit(void) {
    const bool initialized = can_is_initialized_();

    if (!initialized &&
        !atomic_load_explicit(&watchdog_task_handle, memory_order_acquire)) {
        return ESP_OK;
    }

    // Watchdog must stop while TWAI is still installed.
    if (!watchdog_stop_()) {
        ESP_LOGE(TAG, "CAN deinit aborted: watchdog is still running");
        return ESP_ERR_TIMEOUT;
    }

    // Stop TWAI.
    if (initialized) {
        esp_err_t stop_err = twai_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "TWAI stop failed: %s", esp_err_to_name(stop_err));
        }
        esp_err_t uninstall_err = twai_driver_uninstall();
        if (uninstall_err != ESP_OK) {
            ESP_LOGE(TAG, "TWAI uninstall failed: %s; driver state retained",
                     esp_err_to_name(uninstall_err));
            esp_err_t restart_err = twai_start();
            if (restart_err != ESP_OK && restart_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "TWAI restart after uninstall error failed: %s",
                         esp_err_to_name(restart_err));
            }
            if (watchdog_start_() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore CAN watchdog after uninstall error");
            }
            return uninstall_err;
        }
    }

    // Disable the transceiver.
#if PIN_CAN_SE >= 0
    gpio_set_level(PIN_CAN_SE, 1);
#endif

    atomic_store_explicit(&s_initialized, false, memory_order_release);
    ESP_LOGI(TAG, "CAN driver deinitialized");
    return ESP_OK;
}

esp_err_t can_driver_send(const twai_message_t *msg, uint32_t timeout_ms) {
    if (!msg) return ESP_ERR_INVALID_ARG;
    if (!can_is_initialized_()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = twai_transmit(msg, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN send failed: %s", esp_err_to_name(err));
        return err;
    }

    //ESP_LOGW(TAG, "Sent CAN frame ID=0x%X, length=%u", msg->identifier, msg->data_length_code);
    return ESP_OK;
}

esp_err_t can_driver_receive(twai_message_t *msg, uint32_t timeout_ms) {
    if (!msg) return ESP_ERR_INVALID_ARG;
    if (!can_is_initialized_()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = twai_receive(msg, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "CAN receive failed: %s", esp_err_to_name(err));
        }
        return err;
    }

if (BRIDGE_CAN_LOG_CANDRV) {
    char h[32]; _hex(msg->data, msg->data_length_code, h, sizeof(h));
    CFG_LOGI(BRIDGE_CAN_LOG_CANDRV, "CANDRV", "RX id=%08X dlc=%u %s", msg->identifier, msg->data_length_code, h);
}
    return ESP_OK;
}

esp_err_t can_driver_set_filters(const twai_filter_config_t *filter) {
    if (!filter) return ESP_ERR_INVALID_ARG;
    if (!can_is_initialized_()) return ESP_ERR_INVALID_STATE;

    // Stop and reinstall the driver with the new filter.
    if (!watchdog_stop_()) return ESP_ERR_TIMEOUT;
    esp_err_t err = twai_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "TWAI stop for filter update failed: %s", esp_err_to_name(err));
        (void)watchdog_start_();
        return err;
    }
    err = twai_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI uninstall for filter update failed: %s", esp_err_to_name(err));
        (void)twai_start();
        (void)watchdog_start_();
        return err;
    }

    twai_general_config_t g_config = {
        .mode = current_config.listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL,
        .tx_io = PIN_CAN_TX,
        .rx_io = PIN_CAN_RX,
        .clkout_io = TWAI_IO_UNUSED,
        .bus_off_io = TWAI_IO_UNUSED,
        .tx_queue_len = CAN_TX_BUFFER_SIZE,
        .rx_queue_len = CAN_RX_BUFFER_SIZE,
        .alerts_enabled = TWAI_ALERT_ALL,
        .clkout_divider = 0,
        .intr_flags = ESP_INTR_FLAG_LOWMED
    };
    twai_timing_config_t t_config = get_timing(current_config.nom_speed);

    err = twai_driver_install(&g_config, &t_config, filter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI reinstall failed: %s", esp_err_to_name(err));
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return err;
    }

    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI restart failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return err;
    }

    err = watchdog_start_();
    if (err != ESP_OK) {
        (void)twai_stop();
        (void)twai_driver_uninstall();
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return err;
    }

    ESP_LOGI(TAG, "CAN filters set");
    return ESP_OK;
}

esp_err_t can_driver_get_status(twai_status_info_t *status) {
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!can_is_initialized_()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = twai_get_status_info(status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN get status failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "CAN status: state=%d, tx_error=%u, rx_error=%u", status->state, status->tx_error_counter, status->rx_error_counter);
    return ESP_OK;
}

uint32_t can_driver_available(void) {
    if (!can_is_initialized_()) return 0;

    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        ESP_LOGW(TAG, "CAN available: %u frames", status.msgs_to_rx);
        return status.msgs_to_rx;
    }
    return 0;
}
