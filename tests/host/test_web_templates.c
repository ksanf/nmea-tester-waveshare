/* Exercise the real template defaults, lock/masked-patch implementation, web
 * adapter and SDK cJSON. Only clock I/O, NVS, timers and engine notifications are stubbed. */
#include "web/web_templates.h"
#include "nmea_editor/nmea_templates.h"
#include "system/nmea_clock.h"
#include "system/nvs_rw.h"
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nmea_templates_snapshot_t defaults;
static unsigned dirty_calls, rtc_calls;
static time_t last_epoch;
static esp_err_t rtc_result;
static bool concurrent_rtc_write;
static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t save_probe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t save_probe_cond = PTHREAD_COND_INITIALIZER;
static unsigned save_lock_attempts, nvs_writes;
static bool block_first_write, first_write_entered, release_first_write;
static esp_err_t nvs_open_result = ESP_OK, nvs_write_result = ESP_OK;
static struct { uint32_t version; nmea_templates_snapshot_t templates; } saved_blob;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &save_mutex; }
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    (void)ticks;
    pthread_mutex_lock(&save_probe_mutex);
    ++save_lock_attempts; pthread_cond_broadcast(&save_probe_cond);
    pthread_mutex_unlock(&save_probe_mutex);
    return pthread_mutex_lock(sem) == 0;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { return pthread_mutex_unlock(sem) == 0; }

void rs485_engine_mark_dirty(rs485_group_t group) { assert(group < GRP_COUNT); ++dirty_calls; }
TimerHandle_t xTimerCreate(const char *name, TickType_t ticks, int reload, void *arg, TimerCallbackFunction_t cb) {
    (void)name; (void)ticks; (void)reload; (void)arg; (void)cb; return (void *)1;
}
int xTimerReset(TimerHandle_t timer, TickType_t ticks) { (void)timer; (void)ticks; return 1; }
int xTimerStop(TimerHandle_t timer, TickType_t ticks) { (void)timer; (void)ticks; return 1; }
esp_err_t nvs_rw_open_ro(const char *ns, void **handle) { (void)ns; (void)handle; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_rw_open_rw(const char *ns, void **handle) { (void)ns; *handle = (void *)1; return nvs_open_result; }
void nvs_rw_close(void *handle) { (void)handle; }
esp_err_t nvs_rw_read_blob(void *h, const char *k, void *b, size_t *n) { (void)h; (void)k; (void)b; (void)n; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_rw_write_blob(void *h, const char *k, const void *b, size_t n) {
    (void)h; (void)k; assert(n == sizeof(saved_blob));
    pthread_mutex_lock(&save_probe_mutex);
    ++nvs_writes;
    if (block_first_write && nvs_writes == 1) {
        first_write_entered = true; pthread_cond_broadcast(&save_probe_cond);
        while (!release_first_write) pthread_cond_wait(&save_probe_cond, &save_probe_mutex);
    }
    memcpy(&saved_blob, b, n);
    pthread_mutex_unlock(&save_probe_mutex);
    return nvs_write_result;
}
static void *concurrent_writer(void *unused) {
    (void)unused;
    float speed = 42.25f, rate = 7.5f;
    assert(nmea_templates_write_field(&g_nmea_gps.sog_kn, &speed, sizeof(speed)) == ESP_OK);
    assert(nmea_templates_write_field(&g_nmea_gps.rate_hz, &rate, sizeof(rate)) == ESP_OK);
    return NULL;
}
esp_err_t nmea_clock_set_time_epoch(time_t epoch) {
    ++rtc_calls; last_epoch = epoch;
    if (concurrent_rtc_write) {
        pthread_t writer;
        assert(pthread_create(&writer, NULL, concurrent_writer, NULL) == 0);
        assert(pthread_join(writer, NULL) == 0);
    }
    return rtc_result;
}

static const size_t group_offsets[] = {
    offsetof(nmea_templates_snapshot_t, gps), offsetof(nmea_templates_snapshot_t, gyro),
    offsetof(nmea_templates_snapshot_t, log), offsetof(nmea_templates_snapshot_t, echo),
    offsetof(nmea_templates_snapshot_t, weather)
};
static const size_t group_sizes[] = {
    sizeof(nmea_gps_t), sizeof(nmea_gyro_t), sizeof(nmea_log_t), sizeof(nmea_echo_t), sizeof(nmea_weather_t)
};
static const char *group_names[] = {"gps", "gyro", "log", "echo", "weather"};
/* Explicit public contract, independent of web_templates.c's metadata table. */
static const char *expected_names[] = {
    "time_utc date_dmy lat lon lat_dir lon_dir sog_kn cog_deg track_true_deg fix sats talker_id prefix send_rmc send_gga send_zda send_gll send_vtg add_crc rate_hz",
    "heading_true_deg heading_mag_deg deviation_deg dev_dir variation_deg var_dir talker_id prefix send_hdg send_hdt send_hdm add_crc rate_hz rot_deg_min rot_status send_rot follow_rot",
    "hdg_water_true_deg hdg_water_mag_deg water_speed_kn water_speed_kph dist_total_nm dist_trip_nm stern_speed_kn talker_id prefix send_vhw send_vlw send_vbw add_crc rate_hz",
    "depth_ft depth_m depth_fathom keel_offset_m talker_id prefix send_dbt send_dpt send_dbk send_dbs add_crc rate_hz",
    "wind_dir_true_deg wind_dir_mag_deg wind_speed_kn wind_speed_ms wind_speed_kph wind_angle_rel_deg rel_ref wind_side water_temp_C talker_id prefix send_mwd send_mwv send_vwr send_vwt send_mtw add_crc rate_hz"
};
static const int expected_counts[] = {20, 17, 14, 12, 18};
static void reset_templates(void) {
    uint8_t mask[sizeof(defaults)]; memset(mask, 1, sizeof(mask));
    for (int i = 0; i < GRP_COUNT; ++i)
        assert(nmea_templates_patch((rs485_group_t)i, (const char *)&defaults + group_offsets[i], mask, group_sizes[i]) == ESP_OK);
    dirty_calls = rtc_calls = 0; last_epoch = 0; rtc_result = ESP_OK; concurrent_rtc_write = false;
}
static cJSON *member(const cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key); assert(item); return item;
}
static const char *string(const cJSON *object, const char *key) {
    cJSON *item = member(object, key); assert(cJSON_IsString(item)); return item->valuestring;
}
static esp_err_t apply_json(int group, const char *text) {
    cJSON *json = cJSON_Parse(text); assert(json);
    esp_err_t err = web_templates_apply(group, json); cJSON_Delete(json); return err;
}
static void rejected(int group, cJSON *patch) {
    nmea_templates_snapshot_t before = {0}, after = {0};
    nmea_templates_snapshot_all(&before);
    unsigned dirty_before = dirty_calls, rtc_before = rtc_calls;
    assert(web_templates_apply(group, patch) == ESP_ERR_INVALID_ARG);
    nmea_templates_snapshot_all(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(dirty_calls == dirty_before && rtc_calls == rtc_before);
    cJSON_Delete(patch);
}
static void reject_json(int group, const char *text) {
    cJSON *json = cJSON_Parse(text); assert(json); rejected(group, json);
}
static cJSON *single(const char *name, cJSON *value) {
    cJSON *patch = cJSON_CreateObject(); assert(patch && value);
    assert(cJSON_AddItemToObject(patch, name, value)); return patch;
}
static void expect_number(int group, const char *field, double expected) {
    cJSON *root = web_templates_json(group); assert(root);
    cJSON *value = member(member(root, "values"), field);
    assert(cJSON_IsNumber(value)); assert(value->valuedouble == (double)(float)expected);
    cJSON_Delete(root);
}

static void test_groups_metadata_and_defaults(void) {
    assert(web_templates_group(NULL) == -1);
    assert(web_templates_group("GPS") == -1);
    assert(web_templates_group("unknown") == -1);
    assert(web_templates_json(-1) == NULL && web_templates_json(GRP_COUNT) == NULL);
    reject_json(-1, "{}"); reject_json(GRP_COUNT, "{}");
    for (int group = 0; group < GRP_COUNT; ++group) {
        assert(web_templates_group(group_names[group]) == group);
        reset_templates();
        cJSON *root = web_templates_json(group); assert(root);
        assert(!strcmp(string(root, "type"), "template"));
        assert(!strcmp(string(root, "group"), group_names[group]));
        cJSON *fields = member(root, "fields"), *values = member(root, "values");
        assert(cJSON_IsArray(fields) && cJSON_IsObject(values));
        assert(cJSON_GetArraySize(fields) == expected_counts[group]);
        assert(cJSON_GetArraySize(values) == expected_counts[group]);
        char names[512]; snprintf(names, sizeof(names), "%s", expected_names[group]);
        unsigned count = 0;
        for (char *name = strtok(names, " "); name; name = strtok(NULL, " ")) {
            cJSON *value = member(values, name), *found = NULL, *field;
            cJSON_ArrayForEach(field, fields) if (!strcmp(string(field, "name"), name)) {
                assert(!found); found = field;
            }
            assert(found); ++count;
            const char *kind = string(found, "kind"); assert(strlen(kind) == 1);
            double size = member(found, "size")->valuedouble;
            assert(size >= 1 && size <= NMEA_LONSTR);
            if (*kind == 'b') { assert(cJSON_IsBool(value)); assert(size == sizeof(bool)); }
            else if (*kind == 'n') { assert(cJSON_IsNumber(value)); assert(size == sizeof(float)); }
            else if (*kind == 'u') { assert(cJSON_IsNumber(value)); assert(size == sizeof(uint8_t)); }
            else if (*kind == 'c') {
                assert(cJSON_IsString(value) && strlen(value->valuestring) == 1);
                assert(size == sizeof(char)); assert(strchr(string(found, "options"), *value->valuestring));
            } else { assert(*kind == 's'); assert(cJSON_IsString(value)); assert(strlen(value->valuestring) < size); }
            assert(cJSON_IsNumber(member(found, "min")) && cJSON_IsNumber(member(found, "max")));
        }
        assert(count == (unsigned)expected_counts[group]);
        /* All real firmware defaults can be emitted and reapplied together. */
        assert(web_templates_apply(group, values) == ESP_OK);
        nmea_templates_snapshot_t after = {0}; nmea_templates_snapshot_all(&after);
        assert(memcmp((const char *)&defaults + group_offsets[group],
                      (const char *)&after + group_offsets[group], group_sizes[group]) == 0);
        cJSON_Delete(root);
    }
    reset_templates();
}

static void test_types_and_ranges(void) {
    rejected(GRP_GPS, NULL);
    const char *nonobjects[] = {"null", "[]", "1", "true", "\"gps\""};
    for (size_t i = 0; i < sizeof(nonobjects)/sizeof(*nonobjects); ++i) reject_json(GRP_GPS, nonobjects[i]);
    for (int group = 0; group < GRP_COUNT; ++group) {
        cJSON *schema = web_templates_json(group); assert(schema);
        cJSON *field;
        cJSON_ArrayForEach(field, member(schema, "fields")) {
            const char *name = string(field, "name"), *kind = string(field, "kind");
            reset_templates();
            rejected(group, single(name, cJSON_CreateNull()));
            rejected(group, single(name, cJSON_CreateArray()));
            rejected(group, single(name, cJSON_CreateObject()));
            if (*kind == 'n' || *kind == 'u') {
                rejected(group, single(name, cJSON_CreateString("1")));
                rejected(group, single(name, cJSON_CreateBool(true)));
                rejected(group, single(name, cJSON_CreateNumber(NAN)));
                rejected(group, single(name, cJSON_CreateNumber(INFINITY)));
                double min = member(field, "min")->valuedouble, max = member(field, "max")->valuedouble;
                double expected_min = 0, expected_max = 100000;
                if (!strcmp(name, "rate_hz")) { expected_min = 0.5; expected_max = 10; }
                else if (!strcmp(name, "rot_deg_min")) { expected_min = -720; expected_max = 720; }
                else if (!strcmp(name, "water_temp_C")) { expected_min = -100; expected_max = 200; }
                else if (!strcmp(name, "keel_offset_m") || !strcmp(name, "deviation_deg") || !strcmp(name, "variation_deg")) {
                    expected_min = -1000000; expected_max = 1000000;
                } else if (strstr(name, "_deg")) expected_max = 360;
                else if (!strcmp(name, "sats")) expected_max = 99;
                assert(min == expected_min && max == expected_max);
                rejected(group, single(name, cJSON_CreateNumber(min - 1)));
                rejected(group, single(name, cJSON_CreateNumber(max + 1)));
                if (*kind == 'u') rejected(group, single(name, cJSON_CreateNumber(1.5)));
                cJSON *patch = single(name, cJSON_CreateNumber(min));
                assert(web_templates_apply(group, patch) == ESP_OK); cJSON_Delete(patch);
                expect_number(group, name, min);
                patch = single(name, cJSON_CreateNumber(max));
                assert(web_templates_apply(group, patch) == ESP_OK); cJSON_Delete(patch);
                expect_number(group, name, max);
            } else if (*kind == 'b') {
                rejected(group, single(name, cJSON_CreateNumber(1)));
                rejected(group, single(name, cJSON_CreateString("true")));
                for (int b = 0; b < 2; ++b) {
                    cJSON *patch = single(name, cJSON_CreateBool(b));
                    assert(web_templates_apply(group, patch) == ESP_OK); cJSON_Delete(patch);
                    cJSON *result = web_templates_json(group); assert(result);
                    assert(cJSON_IsTrue(member(member(result, "values"), name)) == b);
                    cJSON_Delete(result);
                }
            } else {
                rejected(group, single(name, cJSON_CreateNumber(1)));
                rejected(group, single(name, cJSON_CreateBool(false)));
                rejected(group, single(name, cJSON_CreateString("")));
                rejected(group, single(name, cJSON_CreateString("1234567890123456789012345")));
                if (*kind == 'c') {
                    rejected(group, single(name, cJSON_CreateString("?")));
                    rejected(group, single(name, cJSON_CreateString("AA")));
                    for (const char *p = string(field, "options"); *p; ++p) {
                        char option[] = {*p, 0}; cJSON *patch = single(name, cJSON_CreateString(option));
                        assert(web_templates_apply(group, patch) == ESP_OK); cJSON_Delete(patch);
                    }
                }
            }
        }
        cJSON_Delete(schema);
        reject_json(group, "{\"not_a_template_field\":1}");
    }
    reset_templates();
    assert(apply_json(GRP_GPS, "{}") == ESP_OK && dirty_calls == 0 && rtc_calls == 0);
    reject_json(GRP_GPS, "{\"rate_hz\":2,\"rate_hz\":3}");
    reject_json(GRP_GPS, "{\"talker_id\":\"gp\"}");
    reject_json(GRP_GPS, "{\"talker_id\":\"G1\"}");
    assert(apply_json(GRP_GPS, "{\"talker_id\":\"GN\"}") == ESP_OK);
    reject_json(GRP_GPS, "{\"heading_true_deg\":10}");
    reject_json(GRP_GYRO, "{\"sog_kn\":10}");
    /* Field earlier in JSON must not commit if any later field is invalid. */
    reject_json(GRP_GPS, "{\"sog_kn\":17,\"send_rmc\":1}");
    reject_json(GRP_GPS, "{\"time_utc\":\"123456\",\"sats\":100}");
    reject_json(GRP_GYRO, "{\"rot_deg_min\":20,\"rot_status\":\"Q\"}");
}

static void test_coordinates(void) {
    const char *valid_lat[] = {"0000", "0000.0000", "8959.999999", "9000.000000", "9000"};
    const char *valid_lon[] = {"00000", "00000.0000", "17959.999999", "18000.000000", "18000"};
    const char *invalid_lat[] = {"1", ".5", "000.00", "00000.00", "0000.", "9000.000001", "9100", "8960.0000", "-4306.0", "+4306.0", " 4306.0", "4306.0 ", "4306.1.2", "43e2", "nan", "inf", "0000.0000000"};
    const char *invalid_lon[] = {"1", ".5", "0000.00", "000000.00", "00000.", "18000.000001", "18100", "17960.0000", "-13153.0", "+13153.0", "13153.1.2", "13e3", "00000.0000000"};
    reset_templates();
    for (size_t i = 0; i < sizeof(valid_lat)/sizeof(*valid_lat); ++i) {
        cJSON *patch = single("lat", cJSON_CreateString(valid_lat[i]));
        assert(web_templates_apply(GRP_GPS, patch) == ESP_OK); cJSON_Delete(patch);
    }
    for (size_t i = 0; i < sizeof(valid_lon)/sizeof(*valid_lon); ++i) {
        cJSON *patch = single("lon", cJSON_CreateString(valid_lon[i]));
        assert(web_templates_apply(GRP_GPS, patch) == ESP_OK); cJSON_Delete(patch);
    }
    for (size_t i = 0; i < sizeof(invalid_lat)/sizeof(*invalid_lat); ++i) rejected(GRP_GPS, single("lat", cJSON_CreateString(invalid_lat[i])));
    for (size_t i = 0; i < sizeof(invalid_lon)/sizeof(*invalid_lon); ++i) rejected(GRP_GPS, single("lon", cJSON_CreateString(invalid_lon[i])));
    assert(rtc_calls == 0);
}

static void test_clock_calendar_and_rtc_failure(void) {
    const char *invalid_times[] = {"240000", "126000", "125960", "-10000", "12345", "1234567", "12:345", "23596a"};
    const char *invalid_dates[] = {"000124", "320124", "010024", "011324", "310424", "310624", "310924", "311124", "290225", "300224", "290299", "29a224", "01012"};
    const char *valid_dates[] = {"290200", "290204", "290224", "280225", "310124", "310324", "300424", "310524", "300624", "310724", "310824", "300924", "311024", "301124", "311224", "311299"};
    reset_templates();
    for (size_t i = 0; i < sizeof(invalid_times)/sizeof(*invalid_times); ++i) rejected(GRP_GPS, single("time_utc", cJSON_CreateString(invalid_times[i])));
    for (size_t i = 0; i < sizeof(invalid_dates)/sizeof(*invalid_dates); ++i) rejected(GRP_GPS, single("date_dmy", cJSON_CreateString(invalid_dates[i])));
    for (size_t i = 0; i < sizeof(valid_dates)/sizeof(*valid_dates); ++i) {
        cJSON *patch = single("date_dmy", cJSON_CreateString(valid_dates[i]));
        assert(web_templates_apply(GRP_GPS, patch) == ESP_OK); cJSON_Delete(patch);
    }
    /* UTC epoch must not depend on device/process TZ, including DST. */
    assert(setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1) == 0); tzset();
    assert(apply_json(GRP_GPS, "{\"time_utc\":\"000000\",\"date_dmy\":\"010100\"}") == ESP_OK);
    assert(last_epoch == (time_t)946684800);
    assert(apply_json(GRP_GPS, "{\"time_utc\":\"235959\",\"date_dmy\":\"311299\"}") == ESP_OK);
    assert(last_epoch == (time_t)4102444799LL);
    assert(apply_json(GRP_GPS, "{\"time_utc\":\"123456\",\"date_dmy\":\"290224\"}") == ESP_OK);
    assert(last_epoch == (time_t)1709210096);
    nmea_templates_snapshot_t before = {0}, after = {0}; nmea_templates_snapshot_all(&before);
    unsigned dirty_before = dirty_calls, rtc_before = rtc_calls;
    rtc_result = ESP_FAIL;
    assert(apply_json(GRP_GPS, "{\"time_utc\":\"102030\",\"sog_kn\":81}") == ESP_FAIL);
    nmea_templates_snapshot_all(&after);
    assert(!memcmp(&before, &after, sizeof(before)) && dirty_calls == dirty_before && rtc_calls == rtc_before + 1);
    rtc_result = ESP_OK;
}

static void test_masked_updates_preserve_concurrent_changes(void) {
    reset_templates(); concurrent_rtc_write = true;
    assert(apply_json(GRP_GPS, "{\"time_utc\":\"010203\"}") == ESP_OK);
    nmea_gps_t gps; nmea_templates_gps_snapshot(&gps);
    assert(!strcmp(gps.time_utc, "010203")); assert(gps.sog_kn == 42.25f && gps.rate_hz == 7.5f);
    assert(!strcmp(gps.date_dmy, defaults.gps.date_dmy));
    concurrent_rtc_write = false;
    /* A stale GYRO edit must preserve heading advanced by ROT after its snapshot. */
    assert(apply_json(GRP_GYRO, "{\"follow_rot\":true,\"rot_deg_min\":60}") == ESP_OK);
    nmea_gyro_t edit; nmea_templates_gyro_snapshot(&edit);
    edit.send_rot = true; uint8_t mask[sizeof(edit)] = {0};
    memset(mask + offsetof(nmea_gyro_t, send_rot), 1, sizeof(edit.send_rot));
    nmea_templates_gyro_advance(1000);
    assert(nmea_templates_patch(GRP_GYRO, &edit, mask, sizeof(edit)) == ESP_OK);
    nmea_gyro_t after; nmea_templates_gyro_snapshot(&after);
    assert(after.send_rot && after.heading_true_deg == edit.heading_true_deg + 1.0f);
    assert(after.heading_mag_deg == edit.heading_mag_deg + 1.0f);
    /* Invalid patch envelopes cannot write any bytes or notify the engine. */
    unsigned before = dirty_calls;
    assert(nmea_templates_patch(GRP_GYRO, &edit, mask, sizeof(edit) - 1) == ESP_ERR_INVALID_ARG);
    assert(nmea_templates_patch(GRP_COUNT, &edit, mask, sizeof(edit)) == ESP_ERR_INVALID_ARG);
    assert(nmea_templates_patch(GRP_GYRO, NULL, mask, sizeof(edit)) == ESP_ERR_INVALID_ARG);
    assert(nmea_templates_patch(GRP_GYRO, &edit, NULL, sizeof(edit)) == ESP_ERR_INVALID_ARG);
    float value = 1;
    assert(nmea_templates_write_field(&value, &value, sizeof(value)) == ESP_ERR_INVALID_ARG);
    assert(nmea_templates_write_field((char *)&g_nmea_gps + sizeof(g_nmea_gps) - 1, &value, sizeof(value)) == ESP_ERR_INVALID_ARG);
    assert(dirty_calls == before);
}

static void *save_thread(void *unused) {
    (void)unused; assert(nmea_templates_save_now() == ESP_OK); return NULL;
}
static void test_nvs_save_ordering_and_failure_unlock(void) {
    reset_templates();
    pthread_mutex_lock(&save_probe_mutex);
    save_lock_attempts = nvs_writes = 0;
    first_write_entered = release_first_write = false; block_first_write = true;
    pthread_mutex_unlock(&save_probe_mutex);
    pthread_t first, second;
    assert(pthread_create(&first, NULL, save_thread, NULL) == 0);
    pthread_mutex_lock(&save_probe_mutex);
    while (!first_write_entered) pthread_cond_wait(&save_probe_cond, &save_probe_mutex);
    pthread_mutex_unlock(&save_probe_mutex);
    float speed = 88.0f;
    assert(nmea_templates_write_field(&g_nmea_log.water_speed_kn, &speed, sizeof(speed)) == ESP_OK);
    assert(pthread_create(&second, NULL, save_thread, NULL) == 0);
    pthread_mutex_lock(&save_probe_mutex);
    while (save_lock_attempts < 2) pthread_cond_wait(&save_probe_cond, &save_probe_mutex);
    assert(nvs_writes == 1); /* The second save waits before taking a snapshot. */
    release_first_write = true; pthread_cond_broadcast(&save_probe_cond);
    pthread_mutex_unlock(&save_probe_mutex);
    assert(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    assert(nvs_writes == 2 && saved_blob.version == 2);
    assert(saved_blob.templates.log.water_speed_kn == speed);
    block_first_write = false;
    nvs_open_result = ESP_FAIL;
    assert(nmea_templates_save_now() == ESP_FAIL);
    nvs_open_result = ESP_OK; nvs_write_result = ESP_FAIL;
    assert(nmea_templates_save_now() == ESP_FAIL);
    nvs_write_result = ESP_OK;
    assert(nmea_templates_save_now() == ESP_OK);
}

static size_t allocation_count, fail_at, live_allocations;
static void *failing_malloc(size_t size) {
    if (allocation_count++ == fail_at) return NULL;
    void *ptr = malloc(size); if (ptr) ++live_allocations; return ptr;
}
static void counting_free(void *ptr) { if (ptr) { assert(live_allocations); --live_allocations; free(ptr); } }
static void test_metadata_allocation_failure(void) {
    reset_templates();
    cJSON_Hooks hooks = {.malloc_fn = failing_malloc, .free_fn = counting_free}; cJSON_InitHooks(&hooks);
    bool complete = false;
    for (fail_at = 0; fail_at < 1000; ++fail_at) {
        allocation_count = 0;
        cJSON *root = web_templates_json(GRP_GPS);
        if (root) {
            assert(cJSON_GetArraySize(member(root, "values")) == expected_counts[GRP_GPS]);
            assert(cJSON_GetArraySize(member(root, "fields")) == expected_counts[GRP_GPS]);
            cJSON_Delete(root); complete = true;
        }
        assert(live_allocations == 0);
        if (complete) break;
    }
    assert(complete); cJSON_InitHooks(NULL);
}

int main(void) {
    assert(nmea_templates_init() == ESP_OK);
    nmea_templates_snapshot_all(&defaults);
    test_groups_metadata_and_defaults();
    test_types_and_ranges();
    test_coordinates();
    test_clock_calendar_and_rtc_failure();
    test_masked_updates_preserve_concurrent_changes();
    test_metadata_allocation_failure();
    test_nvs_save_ordering_and_failure_unlock();
    puts("test_web_templates: PASS (81 fields, 5 groups, ranges/types/calendar/atomicity/concurrency/OOM/NVS ordering)");
    return 0;
}
