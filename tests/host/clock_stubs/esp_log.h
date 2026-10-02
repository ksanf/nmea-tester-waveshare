#pragma once
void clock_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) clock_test_log(__VA_ARGS__)
#define ESP_LOGW(...) clock_test_log(__VA_ARGS__)
#define ESP_LOGE(...) clock_test_log(__VA_ARGS__)
