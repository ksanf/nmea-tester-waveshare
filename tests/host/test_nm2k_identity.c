#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "can_module/nm2k_module/nm2k_identity.h"
#include "esp_mac.h"

static uint8_t board_mac[6];
static esp_err_t read_result = ESP_OK;
static unsigned read_calls;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    assert(type == ESP_MAC_WIFI_STA);
    ++read_calls;
    if (read_result != ESP_OK) return read_result;
    memcpy(mac, board_mac, sizeof(board_mac));
    return ESP_OK;
}

static void test_stable_device_and_profile(void)
{
    const uint8_t mac[] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
    memcpy(board_mac, mac, sizeof(mac));
    nm2k_identity_t first, reboot;
    assert(nm2k_identity_read(&first) == ESP_OK);
    assert(nm2k_identity_read(&reboot) == ESP_OK);
    assert(first.name == reboot.name);
    assert(first.identity_number == reboot.identity_number);
    assert(strcmp(first.serial, reboot.serial) == 0);
    assert(strcmp(first.serial, "NM2K02123456789A") == 0);
    assert(strlen(first.serial) == 16);
    assert(first.identity_number <= NM2K_IDENTITY_MASK);
    assert((first.name & NM2K_IDENTITY_MASK) == first.identity_number);
    assert((first.name & ~((uint64_t)NM2K_IDENTITY_MASK)) ==
           ((UINT64_C(2046) << 21) | (UINT64_C(130) << 40) |
            (UINT64_C(4) << 60) | (UINT64_C(1) << 63)));
}

static void test_different_boards_use_complete_mac(void)
{
    const uint8_t mac[] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
    memcpy(board_mac, mac, sizeof(mac));
    nm2k_identity_t first, different;
    assert(nm2k_identity_read(&first) == ESP_OK);
    /* Exercise changes in every byte, including the vendor prefix. This is a
     * sample regression check, not a claim that a 21-bit hash never collides. */
    for (unsigned byte = 0; byte < sizeof(mac); ++byte) {
        memcpy(board_mac, mac, sizeof(mac));
        board_mac[byte] ^= 0x02u;
        assert(nm2k_identity_read(&different) == ESP_OK);
        assert(different.identity_number <= NM2K_IDENTITY_MASK);
        assert(different.identity_number != first.identity_number);
        assert(different.name != first.name);
        assert(strcmp(different.serial, first.serial) != 0);
    }
    const uint8_t edges[] = {0x00, 0x0f, 0x10, 0x7f, 0x80, 0xff};
    memcpy(board_mac, edges, sizeof(edges));
    assert(nm2k_identity_read(&different) == ESP_OK);
    assert(strcmp(different.serial, "NM2K000F107F80FF") == 0);
}

static void test_read_failure_cannot_reuse_identity(void)
{
    nm2k_identity_t identity;
    assert(nm2k_identity_read(&identity) == ESP_OK);
    assert(identity.name != 0);
    read_result = ESP_ERR_INVALID_STATE;
    assert(nm2k_identity_read(&identity) == ESP_ERR_INVALID_STATE);
    assert(identity.name == 0 && identity.identity_number == 0);
    for (unsigned i = 0; i < sizeof(identity.serial); ++i)
        assert(identity.serial[i] == '\0');
    read_result = ESP_OK;
    unsigned calls_before = read_calls;
    assert(nm2k_identity_read(NULL) == ESP_ERR_INVALID_ARG);
    assert(read_calls == calls_before);
}

int main(void)
{
    test_stable_device_and_profile();
    test_different_boards_use_complete_mac();
    test_read_failure_cannot_reuse_identity();
    puts("NMEA2000 device identity tests passed");
    return 0;
}
