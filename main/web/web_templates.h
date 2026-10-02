#pragma once
#include "cJSON.h"
#include "esp_err.h"
int web_templates_group(const char *name);
/* Complete values/schema snapshot; NULL on invalid group or allocation failure. */
cJSON *web_templates_json(int group);
/* Validate the entire object before modifying any template. Unknown/duplicate
 * fields, wrong types and out-of-range values reject the entire patch.
 * GPS date/time is DDMMYY/HHMMSS in UTC (2000..2099); coordinates use the
 * NMEA ddmm[.fraction]/dddmm[.fraction] representation. RTC failure leaves
 * template bytes untouched. An empty object is a successful no-op. */
esp_err_t web_templates_apply(int group, const cJSON *patch);
