#ifndef TEST_STUB_ESP_LOG_H
#define TEST_STUB_ESP_LOG_H

#define ESP_LOG_ERROR   1
#define ESP_LOG_WARN    2
#define ESP_LOG_INFO    3
#define ESP_LOG_DEBUG   4
#define ESP_LOG_VERBOSE 5

#define ESP_LOG_LEVEL_LOCAL(level, tag, fmt, ...) \
    do { (void)(level); (void)(tag); (void)sizeof(fmt); } while (0)

#endif
