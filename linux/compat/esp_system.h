#pragma once

#include "esp_err.h"

// Restarts the app: the process execs itself again.
void esp_restart(void) __attribute__((noreturn));
