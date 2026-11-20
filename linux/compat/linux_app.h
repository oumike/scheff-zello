#pragma once

// Process-level hooks shared by the compat layer and the CardputerZero HAL.

// Remembers argv so esp_restart() can exec the app again.
void linux_app_save_args(char **argv);
// Puts the device back as the launcher left it (backlight, input grab, lock).
void linux_app_before_exit(void);
// Asks the app to quit (from the Esc hold, SIGTERM or closing the window).
void linux_app_request_quit(void);
