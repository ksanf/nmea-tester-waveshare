#ifndef NMEA_OTA_SELECTION_H
#define NMEA_OTA_SELECTION_H
#include "update_format.h"
/* Matches ESP-IDF esp_rom_crc32_le(UINT32_MAX, sequence, 4). */
static inline uint32_t nmea_ota_crc(const uint8_t *p) {
    uint32_t c=0;
    for(unsigned i=0;i<4;i++) { c^=p[i]; for(unsigned b=0;b<8;b++) c=(c>>1)^(0xedb88320u&(0u-(c&1u))); }
    return ~c;
}
static inline bool nmea_ota_entry_valid(const uint8_t *p) {
    uint32_t seq=nmea_read_le32(p),state=nmea_read_le32(p+24);
    return seq && seq!=UINT32_MAX && state!=3 && state!=4 && nmea_read_le32(p+28)==nmea_ota_crc(p);
}
static inline int nmea_ota_active(const uint8_t *data) {
    bool a=nmea_ota_entry_valid(data),b=nmea_ota_entry_valid(data+4096);
    if(a && b) return nmea_read_le32(data)>=nmea_read_le32(data+4096)?0:1;
    return a?0:b?1:-1;
}
static inline void nmea_ota_make_entry(uint8_t *sector,uint32_t seq,uint32_t state) {
    memset(sector,0xff,4096); nmea_write_le32(sector,seq);
    nmea_write_le32(sector+24,state); nmea_write_le32(sector+28,nmea_ota_crc(sector));
}
#endif
