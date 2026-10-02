#pragma once
#define NMEA_TXT6 7
void nmea_templates_gps_time_snapshot(char *utc, char *date);
void nmea_templates_gps_time_update(const char *utc, const char *date);
