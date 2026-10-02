/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef RS485_ENGINE_H
#define RS485_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" { 
#endif

/* Group indices shared with editor tabs and colors. */
typedef enum {
    GRP_GPS, GRP_GYRO, GRP_LOG, GRP_ECHO, GRP_WX, GRP_COUNT
} rs485_group_t;

/* Initialize the engine and create its FreeRTOS task. */
bool rs485_engine_init(void);
bool rs485_engine_running(void);
bool rs485_engine_group_active(rs485_group_t grp);
/* Gracefully stop the task. Returns false without freeing live resources. */
bool rs485_engine_deinit(void);

/* Enable or disable transmission for a group from the UI. */
void rs485_engine_set_active(rs485_group_t grp, bool on);

/* Mark a group dirty after its editor fields change. */
void rs485_engine_mark_dirty(rs485_group_t grp);

/*
 * Drain the latest display snapshot for every group.
 * Must be called from the LVGL task; RS-485 transmission is not throttled.
 */
void rs485_engine_flush_ui(void);

#ifdef __cplusplus
}
#endif
#endif /* RS485_ENGINE_H */
