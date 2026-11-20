#pragma once

#include <stdint.h>

// Microseconds since the program started, on the monotonic clock.
int64_t esp_timer_get_time(void);
