/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester RS-485 driver: half-duplex communication through SP3485EN.
 */

#include "rs485/rs485_driver.h"
#include "config/config_pins.h"
#include "config/config_nmea_tester.h"
#include "config/memory_config.h"

#include <driver/uart.h>
#include <driver/gpio.h>
#include <hal/uart_ll.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_RS485_DRIVER
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ───── Global value (declared extern in config_nmea_tester.h) ───── */

static const char *TAG = "rs485_drv";

/* ───── Internal state ───── */
static bool           s_uart_installed = false;
static bool           s_gpio_inited    = false;
static SemaphoreHandle_t s_bus_mutex   = NULL;
static atomic_uint_fast32_t s_baudrate = ATOMIC_VAR_INIT(RS485_BAUD_DEFAULT);
static atomic_int s_owner = ATOMIC_VAR_INIT(RS485_OWNER_NONE);

#define RS485_OWNER_TRANSITION (-1)

/* ───── Minimum delay (guaranteed to be at least one tick) ───── */
#define SETTLE_TICKS  pdMS_TO_TICKS(2)
#define RX_TIMEOUT_SYMBOLS      2

/* ───── helpers ───── */

static void prepare_rx_pad_for_uart_matrix_(void)
{
    if (PIN_RS485_RX < 0) return;

    /* GPIO43 is U0TXD on ESP32-S3. Force it out of the UART0 IOMUX function
     * before routing UART2 RX through the GPIO matrix. */
    (void)gpio_reset_pin(PIN_RS485_RX);
    (void)gpio_set_direction(PIN_RS485_RX, GPIO_MODE_INPUT);
    (void)gpio_input_enable(PIN_RS485_RX);
    (void)gpio_pullup_en(PIN_RS485_RX);
    (void)gpio_pulldown_dis(PIN_RS485_RX);
}

/**
 * @brief Initialize control GPIOs once.
 */
static void gpio_init_once(void)
{
    if (s_gpio_inited) return;

#if PIN_RS485_RE >= 0 || PIN_RS485_SE >= 0
    uint64_t pin_mask = 0;
#if PIN_RS485_RE >= 0
    pin_mask |= (1ULL << PIN_RS485_RE);
#endif
#if PIN_RS485_SE >= 0
    pin_mask |= (1ULL << PIN_RS485_SE);
#endif

    const gpio_config_t io_cfg = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
#endif

#if PIN_RS485_RE >= 0
    gpio_set_level(PIN_RS485_RE, 0);   /* RX enabled          */
#endif
#if PIN_RS485_SE >= 0
    gpio_set_level(PIN_RS485_SE, 1);   /* Driver enable        */
#endif

    s_gpio_inited = true;
}

/**
 * @brief Calculate the UART timeout in ticks using integer arithmetic.
 *
 * Formula: timeout_ms = n_bytes * 10 bits * 1000 ms * 1.5 / baud
 *        = n_bytes * 15000 / baud
 */
static TickType_t calculate_uart_timeout(size_t n_bytes, uint32_t baud)
{
    if (baud == 0) return pdMS_TO_TICKS(100);

    /* Keep a wider margin than pure wire time.
     * uart_wait_tx_done() is sensitive to scheduling jitter on ESP32,
     * and the previous 1.5x factor caused false write timeouts on short prompt bursts. */
    uint32_t ms = (uint32_t)((n_bytes * 30000UL) / baud);
    if (ms < 20) ms = 20;   /* Minimum 20 ms. */

    return pdMS_TO_TICKS(ms);
}

/**
 * @brief Acquire the bus mutex.
 */
static inline bool bus_lock(TickType_t timeout)
{
    if (!s_bus_mutex) return false;
    return xSemaphoreTake(s_bus_mutex, timeout) == pdTRUE;
}

/**
 * @brief Release the bus mutex.
 */
static inline void bus_unlock(void)
{
    if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
}

static esp_err_t configure_rx_interrupts_(void)
{
    esp_err_t err;

    err = uart_set_rx_timeout(RS485_UART_PORT, RX_TIMEOUT_SYMBOLS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_rx_timeout: %s", esp_err_to_name(err));
        return err;
    }
    uart_set_always_rx_timeout(RS485_UART_PORT, true);
    (void)uart_clear_intr_status(RS485_UART_PORT, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    (void)uart_enable_rx_intr(RS485_UART_PORT);
    return ESP_OK;
}

static esp_err_t rs485_read_common_(uint8_t *buf, size_t *len, TickType_t timeout, bool settle)
{
    if (!s_uart_installed || !buf || !len || *len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!bus_lock(pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG, "Bus lock timeout (read)");
        return ESP_ERR_TIMEOUT;
    }

#if PIN_RS485_RE >= 0
    gpio_set_level(PIN_RS485_RE, 0);
#endif
    if (settle) {
        vTaskDelay(SETTLE_TICKS);
    }

    /* Poll ring buffer with non-blocking reads to avoid
     * xRingbufferSendFromISR yield-assert in SMP FreeRTOS. */
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    int got = uart_read_bytes(RS485_UART_PORT, buf, (uint32_t)*len, 0);

    while (got == 0 && timeout > 0 && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
        got = uart_read_bytes(RS485_UART_PORT, buf, (uint32_t)*len, 0);
    }

    bus_unlock();

    if (got > 0) {
        *len = (size_t)got;
        return ESP_OK;
    }
    if (got == 0) {
        *len = 0;
        return ESP_ERR_TIMEOUT;
    }

    *len = 0;
    ESP_LOGW(TAG, "uart_read_bytes error: %d", got);
    return ESP_FAIL;
}

/* ───── Public API ───── */

static esp_err_t driver_init_(uint32_t baud)
{
    bool uart_installed_now = false;

    gpio_init_once();

    if (!baud) baud = RS485_BAUD_DEFAULT;

    ESP_LOGI(TAG, "RS-485 init request: UART%d TX=%d RX=%d baud=%"PRIu32" RE=%d SE=%d",
             (int)RS485_UART_PORT,
             (int)PIN_RS485_TX,
             (int)PIN_RS485_RX,
             baud,
             (int)PIN_RS485_RE,
             (int)PIN_RS485_SE);

    /* Create the bus mutex once. */
    if (!s_bus_mutex) {
        s_bus_mutex = xSemaphoreCreateMutex();
        if (!s_bus_mutex) {
            ESP_LOGE(TAG, "Failed to create bus mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Install the UART driver once. */
    if (!s_uart_installed) {
        esp_err_t err = uart_driver_install(RS485_UART_PORT, UART_RX_RING_BUF_SIZE, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
            return err;
        }
        s_uart_installed = true;
        uart_installed_now = true;
    }

    /* Configure pin routing. */
    prepare_rx_pad_for_uart_matrix_();
    esp_err_t err = uart_set_pin(RS485_UART_PORT,
                                 PIN_RS485_TX, PIN_RS485_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        if (uart_installed_now) {
            (void)uart_driver_delete(RS485_UART_PORT);
            s_uart_installed = false;
        }
        return err;
    }
    (void)gpio_pullup_en(PIN_RS485_RX);
    (void)gpio_pulldown_dis(PIN_RS485_RX);

    err = rs485_set_baudrate(baud);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RS-485 UART%d ready: TX=%d RX=%d baud=%"PRIu32,
                 (int)RS485_UART_PORT,
                 (int)PIN_RS485_TX,
                 (int)PIN_RS485_RX,
                 baud);
    } else if (uart_installed_now) {
        (void)uart_driver_delete(RS485_UART_PORT);
        s_uart_installed = false;
    }
    return err;
}

static esp_err_t driver_deinit_(void)
{
    if (!s_uart_installed) return ESP_OK;

    /* Wait for transmission to complete. */
    if (!xPortInIsrContext()) {
        esp_err_t err = uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TX wait before deinit: %s", esp_err_to_name(err));
        }
    }

    /* Uninstall the UART driver. */
    esp_err_t err = uart_driver_delete(RS485_UART_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_delete: %s", esp_err_to_name(err));
        return err;
    }
    s_uart_installed = false;

    /* Delete the mutex. */
    if (s_bus_mutex) {
        vSemaphoreDelete(s_bus_mutex);
        s_bus_mutex = NULL;
    }

    s_gpio_inited = false;
    ESP_LOGI(TAG, "RS-485 deinitialized");
    return ESP_OK;
}

static const char *owner_name_(int owner)
{
    switch (owner) {
        case RS485_OWNER_TRANSITION: return "transition";
        case RS485_OWNER_PARSER: return "parser";
        case RS485_OWNER_TRANSMITTER: return "transmitter";
        case RS485_OWNER_NETWORK_BRIDGE: return "network_bridge";
        case RS485_OWNER_CAN_BRIDGE: return "can_bridge";
        default: return "none";
    }
}

esp_err_t rs485_acquire(rs485_owner_t owner, uint32_t baud)
{
    int current;
    int expected;
    esp_err_t err;

    if (owner <= RS485_OWNER_NONE || owner > RS485_OWNER_CAN_BRIDGE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!baud) baud = RS485_BAUD_DEFAULT;

    current = atomic_load_explicit(&s_owner, memory_order_acquire);
    if (current == owner) {
        if (!s_uart_installed) {
            err = driver_init_(baud);
            if (err != ESP_OK) {
                atomic_store_explicit(&s_owner, RS485_OWNER_NONE, memory_order_release);
            }
            return err;
        }
        return (rs485_get_baudrate() == baud) ? ESP_OK : rs485_set_baudrate(baud);
    }

    expected = RS485_OWNER_NONE;
    if (!atomic_compare_exchange_strong_explicit(
            &s_owner, &expected, RS485_OWNER_TRANSITION,
            memory_order_acq_rel, memory_order_acquire)) {
        ESP_LOGW(TAG, "RS-485 busy: requested=%s owner=%s",
                 owner_name_(owner), owner_name_(expected));
        return ESP_ERR_INVALID_STATE;
    }

    err = driver_init_(baud);
    if (err != ESP_OK) {
        atomic_store_explicit(&s_owner, RS485_OWNER_NONE, memory_order_release);
        return err;
    }

    atomic_store_explicit(&s_owner, owner, memory_order_release);
    ESP_LOGI(TAG, "RS-485 acquired by %s", owner_name_(owner));
    return ESP_OK;
}

esp_err_t rs485_release(rs485_owner_t owner)
{
    int expected = owner;
    esp_err_t err;

    if (owner <= RS485_OWNER_NONE || owner > RS485_OWNER_CAN_BRIDGE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &s_owner, &expected, RS485_OWNER_TRANSITION,
            memory_order_acq_rel, memory_order_acquire)) {
        if (expected == RS485_OWNER_NONE) return ESP_OK;
        ESP_LOGW(TAG, "RS-485 release rejected: requested=%s owner=%s",
                 owner_name_(owner), owner_name_(expected));
        return ESP_ERR_INVALID_STATE;
    }

    err = driver_deinit_();
    if (err == ESP_OK) {
        atomic_store_explicit(&s_owner, RS485_OWNER_NONE, memory_order_release);
        ESP_LOGI(TAG, "RS-485 released by %s", owner_name_(owner));
    } else {
        atomic_store_explicit(&s_owner, owner, memory_order_release);
    }
    return err;
}

rs485_owner_t rs485_get_owner(void)
{
    const int owner = atomic_load_explicit(&s_owner, memory_order_acquire);
    if (owner < RS485_OWNER_NONE || owner > RS485_OWNER_CAN_BRIDGE) {
        return RS485_OWNER_NONE;
    }
    return (rs485_owner_t)owner;
}

esp_err_t rs485_set_baudrate(uint32_t baud)
{
    esp_err_t err = ESP_OK;
    bool baud_changed;

    if (baud == 0) return ESP_ERR_INVALID_ARG;
    if (!s_uart_installed) return ESP_ERR_INVALID_STATE;

    if (!bus_lock(pdMS_TO_TICKS(300))) {
        ESP_LOGW(TAG, "Bus lock timeout (set_baudrate)");
        return ESP_ERR_TIMEOUT;
    }
    baud_changed = (rs485_get_baudrate() != baud);

    /* Baud switch must not race with read/write. Force RX mode, wait for any
     * in-flight TX to finish, then flush stale bytes sampled at the old rate. */
#if PIN_RS485_RE >= 0
    gpio_set_level(PIN_RS485_RE, 0);
#endif
    vTaskDelay(SETTLE_TICKS);

    if (!xPortInIsrContext()) {
        esp_err_t tx_err = uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100));
        if (tx_err != ESP_OK) {
            ESP_LOGW(TAG, "TX wait before baud change: %s", esp_err_to_name(tx_err));
        }
    }

    const uart_config_t ucfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    if (baud_changed) {
        (void)uart_flush_input(RS485_UART_PORT);
    }
    err = uart_param_config(RS485_UART_PORT, &ucfg);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_baudrate, baud, memory_order_release);
        err = configure_rx_interrupts_();
    }
    if (err == ESP_OK) {
        if (baud_changed) {
            (void)uart_flush_input(RS485_UART_PORT);
        }
        vTaskDelay(SETTLE_TICKS);
    }

    bus_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Baudrate set to %"PRIu32, baud);
    }
    return err;
}

uint32_t rs485_get_baudrate(void)
{
    return (uint32_t)atomic_load_explicit(&s_baudrate, memory_order_acquire);
}

/**
 * @brief  Transmit data over RS-485.
 *
 * Acquires the bus mutex, switches the transceiver to TX, sends the data,
 * waits for completion, and switches back to RX.
 */
esp_err_t rs485_driver_write(const char *data, size_t len)
{
    if (!s_uart_installed)    return ESP_ERR_INVALID_STATE;
    if (!data || len == 0)    return ESP_ERR_INVALID_ARG;

    if (!bus_lock(pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG, "Bus lock timeout (write)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* TX mode: disable the receiver to avoid reading the local echo. */
#if PIN_RS485_RE >= 0
    gpio_set_level(PIN_RS485_RE, 1);
#endif
    vTaskDelay(SETTLE_TICKS);

    int sent = uart_write_bytes(RS485_UART_PORT, data, len);
    if (sent != (int)len) {
        ESP_LOGE(TAG, "TX mismatch: sent %d / %u", sent, (unsigned)len);
        ret = ESP_FAIL;
        goto out;
    }

    /* Wait for the hardware to finish transmitting. */
    if (!xPortInIsrContext()) {
        TickType_t tx_timeout = calculate_uart_timeout(len, rs485_get_baudrate());
        esp_err_t err = uart_wait_tx_done(RS485_UART_PORT, tx_timeout);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uart_wait_tx_done: %s", esp_err_to_name(err));
            ret = err;
        }
    }
out:
#if PIN_RS485_RE >= 0
    gpio_set_level(PIN_RS485_RE, 0);   /* Switch back to RX. */
#endif
    bus_unlock();
    return ret;
}

/**
 * @brief  Receive RS-485 data by polling.
 *
 * Flushes the UART input buffer, then reads up to *len bytes using a
 * dynamically calculated timeout.
 *
 * @param[in]     buf  Caller-provided receive buffer.
 * @param[in,out] len  [in] buffer capacity, [out] bytes actually read.
 * @return ESP_OK / ESP_ERR_TIMEOUT / ESP_FAIL
 */
esp_err_t rs485_driver_read(uint8_t *buf, size_t *len)
{
    if (!buf || !len || *len == 0) return ESP_ERR_INVALID_ARG;
    TickType_t timeout = calculate_uart_timeout(*len, rs485_get_baudrate());
    return rs485_read_common_(buf, len, timeout, true);
}

esp_err_t rs485_driver_read_timeout(uint8_t *buf, size_t *len, uint32_t timeout_ms)
{
    return rs485_read_common_(buf, len, pdMS_TO_TICKS(timeout_ms), false);
}

esp_err_t rs485_driver_read_nowait(uint8_t *buf, size_t *len)
{
    return rs485_read_common_(buf, len, 0, false);
}

size_t rs485_driver_available(void)
{
    if (!s_uart_installed) return 0;

    size_t available = 0;
    esp_err_t err = uart_get_buffered_data_len(RS485_UART_PORT, &available);
    if (err != ESP_OK) {
        return 0;
    }

    return available;
}
