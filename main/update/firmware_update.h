#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Controller task only: transport is stopped before opening this mode. */
esp_err_t firmware_update_open(void);
void firmware_update_tick(void);
bool firmware_update_active(void);
bool firmware_update_busy(void);
void firmware_update_request_close(void);
/* Call after essential startup checks; never write eFuses or erase settings. */
void firmware_update_boot_check(bool startup_ok);
bool firmware_update_pending_boot(void);
/* Start at app_main entry, before potentially blocking peripheral setup. */
void firmware_update_start_boot_guard(void);
