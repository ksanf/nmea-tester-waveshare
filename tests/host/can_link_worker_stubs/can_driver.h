#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define TWAI_MSG_FLAG_EXTD 1u
#define TWAI_MSG_FLAG_RTR 2u
#define TWAI_MSG_FLAG_DLC_NON_COMP 0x10u
/* Match the real IDF 5.5 legacy message ABI. receive updates only extd/rtr,
 * not the other bits of the deprecated flags word. */
typedef struct {
    union {
        struct {
            uint32_t extd:1, rtr:1, ss:1, self:1, dlc_non_comp:1, reserved:27;
        };
        uint32_t flags;
    };
    uint32_t identifier;
    uint8_t data_length_code, data[8];
} twai_message_t;
esp_err_t can_driver_receive(twai_message_t *msg, uint32_t timeout_ms);
esp_err_t can_driver_send(const twai_message_t *msg, uint32_t timeout_ms);
