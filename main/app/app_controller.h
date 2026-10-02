#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    APP_MODE_IDLE = 0, APP_MODE_RX485, APP_MODE_TX485,
    APP_MODE_RS485_BRIDGE, APP_MODE_N2K, APP_MODE_SAILOR
} app_mode_t;

/* All lifecycle work runs in the controller task, outside the LVGL mutex. */
esp_err_t app_controller_init(void);
bool app_controller_local_start(app_mode_t mode);
bool app_controller_local_stop(void);
bool app_controller_is_web(void);
/* The LCD escape path has priority over queued remote commands. */
void app_controller_return_local(void);
bool app_controller_web_submit(int fd, const char *json, size_t len);
void app_controller_web_disconnected(int fd);

bool app_controller_local_baud(uint32_t baud);
bool app_controller_local_n2k_speed(uint32_t speed);
bool app_controller_local_sailor_baud(uint32_t baud);