#include "update_format.h"
#include "ota_selection.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint32_t version=0;
    assert(nmea_release_number("1.1",4,&version) && version==11);
    assert(nmea_release_number("1.9",4,&version) && version==19);
    assert(nmea_release_number("2.0",4,&version) && version==20);
    assert(nmea_release_number("999.9",6,&version) && version==9999);
    const char *bad[]={"", "1", "1.", "01.2", "1.10", "1000.1", "-1.0", "1.2x", " 1.2"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!nmea_release_number(bad[i],strlen(bad[i])+1,&version));
    assert(!nmea_release_number("1.2",3,&version));
    assert(!nmea_release_number(NULL,4,&version));
    assert(nmea_crc32((const uint8_t *)"123456789",9)==0xcbf43926u);
    uint8_t sum[32];
    assert(nmea_parse_sha256("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF",sum));
    assert(!nmea_parse_sha256("00",sum));
    assert(!nmea_parse_sha256("z123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF",sum));

    uint8_t image[432]={0};
    image[0]=0xe9;image[1]=1;image[12]=9;image[23]=1;
    nmea_write_le32(image+28,352);nmea_write_le32(image+32,0xabcd5432);
    strcpy((char *)image+48,"1.2");strcpy((char *)image+80,"nmea-tester");
    memcpy(image+288,"NMEATST1",8);strcpy((char *)image+296,NMEA_BOARD_ID);
    nmea_write_le32(image+328,NMEA_LAYOUT_VERSION);nmea_write_le32(image+332,NMEA_SETTINGS_SCHEMA);
    image[399]=0xef;for(unsigned i=32;i<384;i++)image[399]^=image[i];
    assert(!nmea_validate_image(image,sizeof(image),sizeof(image)));
    size_t end=0;assert(nmea_esp_image_bounds(image,sizeof(image),&end) && end==400);
    for(unsigned i=0;i<sizeof(image);i++)assert(!nmea_esp_image_bounds(image,i,&end));
    image[399]^=1;assert(!nmea_esp_image_bounds(image,sizeof(image),&end));image[399]^=1;
    nmea_write_le32(image+28,UINT32_MAX);assert(!nmea_esp_image_bounds(image,sizeof(image),&end));nmea_write_le32(image+28,352);
    image[12]=8;assert(nmea_validate_image(image,sizeof(image),sizeof(image)));image[12]=9;
    image[296]='x';assert(nmea_validate_image(image,sizeof(image),sizeof(image)));
    assert(nmea_validate_image(NULL,0,0));
    assert(nmea_validate_image(image,383,sizeof(image)));
    assert(nmea_validate_image(image,sizeof(image),NMEA_APP_SIZE+1));

    uint8_t data[8192];memset(data,0xff,sizeof(data));assert(nmea_ota_active(data)==-1);
    nmea_ota_make_entry(data,1,2);assert(nmea_ota_active(data)==0);
    assert(nmea_read_le32(data+28)==0x4743989au);
    nmea_ota_make_entry(data+4096,2,0);assert(nmea_ota_active(data)==1);
    nmea_write_le32(data+4096+24,1);assert(nmea_ota_active(data)==1);
    nmea_write_le32(data+4096+24,3);assert(nmea_ota_active(data)==0);
    nmea_write_le32(data+4096+24,4);assert(nmea_ota_active(data)==0);
    nmea_ota_make_entry(data+4096,2,2);data[4096+28]^=1;assert(nmea_ota_active(data)==0);
    /* Every partial sector write retains the old selector, until a complete valid new entry exists. */
    uint8_t next[4096];nmea_ota_make_entry(next,2,0);
    for(unsigned i=0;i<32;i++) {memset(data+4096,0xff,4096);memcpy(data+4096,next,i);assert(nmea_ota_active(data)==0);}
    memcpy(data+4096,next,32);assert(nmea_ota_active(data)==1);
    nmea_ota_make_entry(data+4096,UINT32_MAX,2);assert(nmea_ota_active(data)==0);
    nmea_ota_make_entry(data+4096,0,2);assert(nmea_ota_active(data)==0);
    puts("PASS: release numbering, image bounds/board checks, CRC and OTA power-cut selector cases");
    return 0;
}
