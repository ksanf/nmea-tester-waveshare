#include "update_format.h"
#include "config/config_nmea_tester.h"
#if PANEL_PROFILE != PANEL_PROFILE_WAVESHARE_5_800X480
#error "Define a distinct updater board ID before releasing another panel profile"
#endif
_Static_assert(sizeof(nmea_image_metadata_t)==96,"Image descriptor ABI");
const nmea_image_metadata_t nmea_image_metadata
    __attribute__((section(".rodata_custom_desc"),used)) = {
    .magic="NMEATST1", .board=NMEA_BOARD_ID,
    .layout=NMEA_LAYOUT_VERSION, .settings_schema=NMEA_SETTINGS_SCHEMA,
    .source_commit=NMEA_SOURCE_COMMIT
};
