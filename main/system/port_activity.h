#pragma once
#include <stdint.h>

typedef enum {
    PORT_ACTIVITY_RS485_TX,
    PORT_ACTIVITY_RS485_RX,
    PORT_ACTIVITY_CAN_TX,
    PORT_ACTIVITY_CAN_RX,
    PORT_ACTIVITY_COUNT
} port_activity_channel_t;

/* Task-safe activity latch. Repeated events coalesce until the UI consumes them. */
void port_activity_mark(port_activity_channel_t channel);
/* Single consumer: the web-control screen. Bit positions match the enum. */
uint32_t port_activity_take(void);
