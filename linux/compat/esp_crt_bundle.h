#pragma once

#include "esp_err.h"

// libcurl verifies against the system's CA store; this only keeps the
// websocket config's field filled in as on the device.
esp_err_t esp_crt_bundle_attach(void *conf);
