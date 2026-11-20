#pragma once

// ESP_LOGx to stderr, with a millisecond timestamp, as the serial log reads.

#include <stdint.h>
#include <stdio.h>

void esp_log_write_line(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) esp_log_write_line('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write_line('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write_line('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { if (0) esp_log_write_line('D', tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { if (0) esp_log_write_line('V', tag, fmt, ##__VA_ARGS__); } while (0)
