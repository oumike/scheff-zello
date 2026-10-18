#pragma once

// Crash and disconnect reports on the microSD card, for diagnosing faults
// that don't happen at the desk with a serial cable attached.
//
// Every log line is also kept in a ring buffer in RAM that survives a panic
// or watchdog reset. After such a reset the next boot writes, to
// /sdcard/crash/:
//   NNNN-crash.txt  reset reason, firmware, the panic summary and the log
//                   lines leading up to the reset
//   NNNN-core.elf   the raw ESP-IDF core dump from the flash partition
// An unexpected drop of the Zello connection writes NNNN-disconnect.txt with
// the recent log, since that needs no crash at all.

#include <stdbool.h>

// Hooks logging and takes the previous boot's log if it ended in a crash.
// Call first thing in app_main.
void crash_report_init(void);

// Writes any pending crash report in the background. Call once the board is
// up and Wi-Fi is initialised (on the T-Display-P4 the SD card shares the
// SDMMC host with the Wi-Fi coprocessor).
void crash_report_start(void);

// Queues a report of an unexpected disconnect; rate-limited.
void crash_report_disconnect(const char *why);

// Console: prints the reset reason and report status, retrying a pending
// crash report.
void crash_report_print_status(void);
