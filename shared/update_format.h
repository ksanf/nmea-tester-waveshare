/* Shared, allocation-free validation for the firmware and Windows updater. */
#ifndef NMEA_UPDATE_FORMAT_H
#define NMEA_UPDATE_FORMAT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define NMEA_BOARD_ID "waveshare-s3-5-800x480"
#define NMEA_LAYOUT_VERSION 2u
#define NMEA_SETTINGS_SCHEMA 1u
#define NMEA_FLASH_SIZE 0x1000000u
#define NMEA_APP_SIZE 0x600000u
#define NMEA_APP0_OFFSET 0x10000u
#define NMEA_APP1_OFFSET 0x610000u
#define NMEA_OTADATA_OFFSET 0xc10000u
#define NMEA_IMAGE_METADATA_OFFSET 288u
#define NMEA_IMAGE_PREFIX_SIZE 384u
#define NMEA_PACKAGE_HEADER_SIZE 512u
#define NMEA_PACKAGE_SEGMENTS 3u
#define NMEA_INFO_PORT 80u
#define NMEA_UPDATE_PORT 8080u

typedef struct {
    char magic[8];
    char board[32];
    uint32_t layout;
    uint32_t settings_schema;
    char source_commit[40];
    uint8_t reserved[8];
} nmea_image_metadata_t;

static inline uint32_t nmea_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void nmea_write_le32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
/* Versions are integer tenths, never floating point: 1.9 -> 2.0. */
static inline bool nmea_release_number(const char *s, size_t capacity, uint32_t *out) {
    uint32_t major=0; size_t i=0;
    if (!s || !capacity || !out) return false;
    while(i<capacity && s[i]>='0' && s[i]<='9') {
        if(i>=3 || (i && s[0]=='0')) return false;
        major=major*10u+(uint32_t)(s[i++]-'0');
    }
    if(!i || i+2>=capacity || s[i]!='.' || s[i+1]<'0' || s[i+1]>'9' || s[i+2]) return false;
    *out=major*10u+(uint32_t)(s[i+1]-'0'); return true;
}
static inline bool nmea_fixed_string(const uint8_t *p,size_t n,const char *expected) {
    size_t l=strlen(expected); return l<n && !memcmp(p,expected,l) && p[l]==0;
}
static inline const char *nmea_validate_image(const uint8_t *p,size_t available,size_t total) {
    uint32_t version;
    if(!p || available<NMEA_IMAGE_PREFIX_SIZE || total<NMEA_IMAGE_PREFIX_SIZE || total>NMEA_APP_SIZE)
        return "Invalid firmware size";
    if(p[0]!=0xe9 || p[1]<1 || p[1]>16 || p[12]!=9 || p[13]!=0)
        return "Not an ESP32-S3 application";
    if(nmea_read_le32(p+28)<352u || nmea_read_le32(p+32)!=0xabcd5432u)
        return "Invalid ESP application descriptor";
    if(!nmea_fixed_string(p+80,32,"nmea-tester")) return "Wrong firmware project";
    if(!nmea_release_number((const char *)p+48,32,&version)) return "Invalid release version";
    if(memcmp(p+288,"NMEATST1",8) || !nmea_fixed_string(p+296,32,NMEA_BOARD_ID))
        return "Firmware is for another board or predates updater support";
    if(nmea_read_le32(p+328)!=NMEA_LAYOUT_VERSION) return "Incompatible flash layout";
    if(nmea_read_le32(p+332)!=NMEA_SETTINGS_SCHEMA) return "Incompatible settings schema";
    return NULL;
}
/* Full unsigned ESP image: segment bounds, XOR checksum and appended SHA region.
 * The caller verifies SHA-256 over [0, hash_offset), using its platform crypto. */
static inline bool nmea_esp_image_bounds(const uint8_t *p,size_t n,size_t *hash_offset) {
    if(!p || !hash_offset || n<64 || p[0]!=0xe9 || !p[1] || p[1]>16 ||
       p[12]!=9 || p[13]!=0 || p[23]!=1) return false;
    size_t cursor=24; uint8_t checksum=0xef;
    for(unsigned i=0;i<p[1];i++) {
        if(cursor>n || n-cursor<8) return false;
        uint32_t size=nmea_read_le32(p+cursor+4); cursor+=8;
        if(size>n-cursor) return false;
        for(size_t j=0;j<size;j++) checksum^=p[cursor+j];
        cursor+=size;
    }
    size_t padding=16u-(cursor%16u);
    if(n-cursor<padding || n-cursor-padding!=32 || p[cursor+padding-1]!=checksum) return false;
    *hash_offset=cursor+padding; return true;
}
/* Exact v2 layout, not just the presence of similarly named partitions.
 * ESP-IDF's MD5 entry follows six entries; caller checks that MD5 separately. */
static inline bool nmea_partition_layout(const uint8_t *p,size_t n) {
    static const char *const names[]={"nvs","phy_init","ota_0","ota_1","otadata","storage"};
    static const uint32_t offsets[]={0x9000,0xf000,NMEA_APP0_OFFSET,NMEA_APP1_OFFSET,NMEA_OTADATA_OFFSET,0xc12000};
    static const uint32_t sizes[]={0x6000,0x1000,NMEA_APP_SIZE,NMEA_APP_SIZE,0x2000,0x3ee000};
    static const uint8_t types[]={1,1,0,0,1,1},subtypes[]={2,1,0x10,0x11,0,0x82};
    if(!p || n<0xc00) return false;
    for(unsigned i=0;i<6;i++) {
        const uint8_t *e=p+i*32;
        if(e[0]!=0xaa || e[1]!=0x50 || e[2]!=types[i] || e[3]!=subtypes[i] ||
           nmea_read_le32(e+4)!=offsets[i] || nmea_read_le32(e+8)!=sizes[i] ||
           !nmea_fixed_string(e+12,16,names[i]) || nmea_read_le32(e+28)!=0) return false;
    }
    if(p[192]!=0xeb || p[193]!=0xeb) return false;
    for(size_t i=194;i<208;i++) if(p[i]!=0xff) return false;
    for(size_t i=224;i<0xc00;i++) if(p[i]!=0xff) return false;
    return true;
}
static inline uint32_t nmea_crc32(const uint8_t *p,size_t n) {
    uint32_t c=0xffffffffu;
    while(n--) { c^=*p++; for(unsigned b=0;b<8;b++) c=(c>>1)^(0xedb88320u & (0u-(c&1u))); }
    return ~c;
}
static inline bool nmea_parse_sha256(const char *s,uint8_t out[32]) {
    if(!s || strlen(s)!=64) return false;
    for(unsigned i=0;i<32;i++) { unsigned v=0;
        for(unsigned j=0;j<2;j++) { unsigned char c=(unsigned char)s[2*i+j]; unsigned d;
            if(c>='0'&&c<='9') d=c-'0'; else if(c>='a'&&c<='f') d=c-'a'+10;
            else if(c>='A'&&c<='F') d=c-'A'+10; else return false;
            v=(v<<4)|d;
        } out[i]=(uint8_t)v;
    } return true;
}
#endif
