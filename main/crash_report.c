#include "crash_report.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sdcard.h"
#include "settings.h"
#include "zello_client.h"

static const char *TAG = "crash";

#define CRASH_DIR SDCARD_MOUNT "/crash"

// Internal RAM is scarce on the S3; the P4 can afford a longer history.
#if CONFIG_IDF_TARGET_ESP32P4
#define RING_SIZE (16 * 1024)
#define BOARD_NAME "LilyGO T-Display-P4"
#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2
#define RING_SIZE (6 * 1024)
#define BOARD_NAME "Seeed Wio Tracker L2"
#else
#define RING_SIZE (6 * 1024)
#define BOARD_NAME "LilyGO T-Deck"
#endif

#define RING_MAGIC 0x52435A53u  // "SZCR"
#define RING_LINE_MAX 256
#define REPORT_STACK 6144
// Lets the lines after a disconnect (Wi-Fi, TLS, the reconnect) reach the log.
#define DISCONNECT_SETTLE_MS 3000
#define DISCONNECT_MIN_GAP_US (30LL * 1000 * 1000)
#define DISCONNECT_MAX_REPORTS 50

// In .noinit: survives a panic or watchdog reset, garbage after power-on.
typedef struct {
    uint32_t magic;
    uint32_t head;  // bytes ever written; the write position is head % RING_SIZE
    char buf[RING_SIZE];
} log_ring_t;

static __NOINIT_ATTR log_ring_t s_ring;

static SemaphoreHandle_t s_ring_lock;
static vprintf_like_t s_prev_vprintf;
static char s_line[RING_LINE_MAX];  // formatting scratch, under s_ring_lock

static SemaphoreHandle_t s_report_lock;
static esp_reset_reason_t s_reset_reason;
static bool s_crash_pending;
static char *s_prev_log;  // the crashed boot's log, linear
static size_t s_prev_log_len;
static int s_reports_written;
static char s_last_report[64];

static volatile bool s_disc_busy;
static int64_t s_disc_last_us;
static int s_disc_count;
static char s_disc_why[64];

// ---- log ring ---------------------------------------------------------------

static void ring_put(const char *data, size_t len)
{
    while (len) {
        size_t pos = s_ring.head % RING_SIZE;
        size_t n = RING_SIZE - pos < len ? RING_SIZE - pos : len;
        memcpy(s_ring.buf + pos, data, n);
        s_ring.head += n;
        data += n;
        len -= n;
    }
}

// Copies the ring out in order, starting at a line boundary once it has wrapped.
static size_t ring_copy(char *out, size_t cap)
{
    uint32_t head = s_ring.head;
    size_t n = head < RING_SIZE ? head : RING_SIZE;
    size_t start = head < RING_SIZE ? 0 : head % RING_SIZE;
    if (n > cap) n = cap;
    size_t first = RING_SIZE - start < n ? RING_SIZE - start : n;
    memcpy(out, s_ring.buf + start, first);
    memcpy(out + first, s_ring.buf, n - first);
    if (head >= RING_SIZE) {
        char *nl = memchr(out, '\n', n);
        if (nl) {
            size_t skip = nl + 1 - out;
            memmove(out, nl + 1, n - skip);
            n -= skip;
        }
    }
    return n;
}

static int ring_vprintf(const char *fmt, va_list ap)
{
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        va_end(copy);
        if (n > 0) ring_put(s_line, n < (int)sizeof(s_line) ? (size_t)n : sizeof(s_line) - 1);
        xSemaphoreGive(s_ring_lock);
    }
    return s_prev_vprintf(fmt, ap);
}

// ---- report helpers ---------------------------------------------------------

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic (exception or abort)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

static bool reason_is_crash(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH || r == ESP_RST_CPU_LOCKUP;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#define HAVE_COREDUMP 1
#else
#define HAVE_COREDUMP 0
#endif

static bool coredump_present(void)
{
#if HAVE_COREDUMP
    return esp_core_dump_image_check() == ESP_OK;
#else
    return false;
#endif
}

// The next free NNNN in CRASH_DIR.
static int next_index(void)
{
    int max = 0;
    DIR *d = opendir(CRASH_DIR);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        int n = atoi(e->d_name);
        if (n > max) max = n;
    }
    closedir(d);
    return max + 1;
}

static void write_header(FILE *f, const char *kind)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[65];
    esp_app_get_elf_sha256(sha, sizeof(sha));

    fprintf(f, "Scheff for Zello %s report\n", kind);
    fprintf(f, "Board:        %s\n", BOARD_NAME);
    fprintf(f, "Firmware:     %s %s, built %s %s, ESP-IDF %s\n", app->project_name, app->version, app->date,
            app->time, app->idf_ver);
    fprintf(f, "ELF SHA256:   %s\n", sha);
    fprintf(f, "Reset reason: %s (this boot)\n", reset_reason_name(s_reset_reason));

    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm;
        char when[32];
        gmtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &tm);
        fprintf(f, "Written:      %s\n", when);
    }
    fprintf(f, "Uptime:       %lld s\n", (long long)(esp_timer_get_time() / 1000000));
    fprintf(f, "Free heap:    %u internal (largest %u), %u PSRAM\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void write_zello_status(FILE *f)
{
    static const char *const states[] = {"offline", "connecting", "signing in", "online", "failed"};
    zello_status_t st;
    zello_get_status(&st);
    fprintf(f, "Zello:        %s, channel \"%s\", %s, rx %lu / tx %lu packets\n", states[st.state],
            g_settings.channel, st.ptt ? "transmitting" : st.receiving ? "receiving" : "idle",
            (unsigned long)st.rx_packets, (unsigned long)st.tx_packets);
    if (st.error[0]) fprintf(f, "Last error:   %s\n", st.error);
}

#if HAVE_COREDUMP
// Summarises the core dump and copies it out as an ELF file.
static void write_coredump(FILE *f, int idx)
{
    char reason[200];
    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
        fprintf(f, "Panic reason: %s\n", reason);
    }

    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        fprintf(f, "Crashed task: %s, PC 0x%08lx\n", sum->exc_task, (unsigned long)sum->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        fprintf(f, "Exception:    cause %lu, vaddr 0x%08lx\n", (unsigned long)sum->ex_info.exc_cause,
                (unsigned long)sum->ex_info.exc_vaddr);
        fprintf(f, "Backtrace:   ");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
            fprintf(f, " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
        }
        fprintf(f, "%s\n", sum->exc_bt_info.corrupted ? " |<-CORRUPTED" : "");
#else
        fprintf(f, "Exception:    mcause 0x%08lx, mtval 0x%08lx, ra 0x%08lx, sp 0x%08lx\n",
                (unsigned long)sum->ex_info.mcause, (unsigned long)sum->ex_info.mtval,
                (unsigned long)sum->ex_info.ra, (unsigned long)sum->ex_info.sp);
#endif
        fprintf(f, "Dump ELF:     %.*s\n", (int)sizeof(sum->app_elf_sha256), (const char *)sum->app_elf_sha256);
    }
    free(sum);

    size_t addr = 0, size = 0;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || esp_core_dump_image_get(&addr, &size) != ESP_OK || addr < part->address) {
        fprintf(f, "Core dump:    unreadable\n");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), CRASH_DIR "/%04d-core.elf", idx);
    FILE *out = fopen(path, "wb");
    uint8_t *buf = malloc(4096);
    esp_err_t err = (out && buf) ? ESP_OK : ESP_FAIL;
    for (size_t off = 0; err == ESP_OK && off < size; off += 4096) {
        size_t n = size - off < 4096 ? size - off : 4096;
        err = esp_partition_read(part, addr - part->address + off, buf, n);
        if (err == ESP_OK && fwrite(buf, 1, n, out) != n) err = ESP_FAIL;
    }
    free(buf);
    if (out && fclose(out) != 0) err = ESP_FAIL;
    fprintf(f, "Core dump:    %s (%u bytes)%s\n", path + strlen(CRASH_DIR) + 1, (unsigned)size,
            err == ESP_OK ? "" : ", WRITE FAILED");
    if (err == ESP_OK) {
        esp_err_t erase_err = esp_core_dump_image_erase();
        if (erase_err != ESP_OK) ESP_LOGE(TAG, "could not erase the saved flash core dump");
    } else {
        ESP_LOGE(TAG, "could not save %s; retaining the flash core dump", path);
    }
}
#endif

// ---- reports ----------------------------------------------------------------

// Writes the pending crash report. Call with s_report_lock held.
static bool save_crash_locked(void)
{
    if (!s_crash_pending) return true;
    if (sdcard_mount() != ESP_OK) {
        ESP_LOGW(TAG, "crash report waiting for a microSD card ('crash' retries)");
        return false;
    }
    mkdir(CRASH_DIR, 0777);
    int idx = next_index();
    char path[64];
    snprintf(path, sizeof(path), CRASH_DIR "/%04d-crash.txt", idx);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "could not create %s", path);
        return false;
    }
    write_header(f, "crash");
#if HAVE_COREDUMP
    if (coredump_present()) {
        write_coredump(f, idx);
    } else
#endif
    {
        fprintf(f, "Core dump:    none (%s)\n",
                s_reset_reason == ESP_RST_BROWNOUT ? "a brownout leaves none"
                : HAVE_COREDUMP ? "not captured" : "core dumps are off in this build's sdkconfig");
    }
    fprintf(f, "\n--- log before the reset ---\n");
    if (s_prev_log_len) {
        fwrite(s_prev_log, 1, s_prev_log_len, f);
    } else {
        fprintf(f, "(not kept)\n");
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGE(TAG, "could not finish writing %s", path);
        return false;
    }

    free(s_prev_log);
    s_prev_log = NULL;
    s_prev_log_len = 0;
    s_crash_pending = false;
    s_reports_written++;
    strlcpy(s_last_report, path, sizeof(s_last_report));
    ESP_LOGW(TAG, "crash report saved to %s", path);
    return true;
}

static void save_disconnect_locked(const char *why)
{
    if (sdcard_mount() != ESP_OK) return;
    save_crash_locked();
    mkdir(CRASH_DIR, 0777);

    char *log = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!log) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t len = ring_copy(log, RING_SIZE);
    xSemaphoreGive(s_ring_lock);

    char path[64];
    snprintf(path, sizeof(path), CRASH_DIR "/%04d-disconnect.txt", next_index());
    FILE *f = fopen(path, "w");
    if (f) {
        write_header(f, "disconnect");
        fprintf(f, "Event:        %s\n", why);
        write_zello_status(f);
        fprintf(f, "\n--- recent log ---\n");
        fwrite(log, 1, len, f);
        bool ok = !ferror(f);
        if (fclose(f) != 0) ok = false;
        if (ok) {
            s_reports_written++;
            strlcpy(s_last_report, path, sizeof(s_last_report));
            ESP_LOGW(TAG, "disconnect report saved to %s", path);
        } else {
            ESP_LOGE(TAG, "could not finish writing %s", path);
        }
    } else {
        ESP_LOGE(TAG, "could not create %s", path);
    }
    free(log);
}

static void crash_task(void *arg)
{
    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    save_crash_locked();
    xSemaphoreGive(s_report_lock);
    vTaskDelete(NULL);
}

static void disconnect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(DISCONNECT_SETTLE_MS));
    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    save_disconnect_locked(s_disc_why);
    xSemaphoreGive(s_report_lock);
    s_disc_busy = false;
    vTaskDelete(NULL);
}

// ---- API --------------------------------------------------------------------

void crash_report_init(void)
{
    s_ring_lock = xSemaphoreCreateMutex();
    s_report_lock = xSemaphoreCreateMutex();
    s_reset_reason = esp_reset_reason();

    bool ring_ok = s_ring.magic == RING_MAGIC && s_reset_reason != ESP_RST_POWERON;
    s_crash_pending = reason_is_crash(s_reset_reason) || coredump_present();
    if (s_crash_pending && ring_ok && s_ring.head) {
        s_prev_log = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
        if (s_prev_log) s_prev_log_len = ring_copy(s_prev_log, RING_SIZE);
    }
    s_ring.magic = RING_MAGIC;
    s_ring.head = 0;

    if (s_ring_lock && s_report_lock) s_prev_vprintf = esp_log_set_vprintf(ring_vprintf);

    if (s_crash_pending) {
        ESP_LOGW(TAG, "last reset: %s%s", reset_reason_name(s_reset_reason),
                 coredump_present() ? ", core dump in flash" : "");
    }
}

void crash_report_start(void)
{
    if (!s_crash_pending || !s_report_lock) return;
    if (xTaskCreate(crash_task, "crash_rpt", REPORT_STACK, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the crash report");
    }
}

void crash_report_disconnect(const char *why)
{
    if (!s_report_lock || s_disc_busy || s_disc_count >= DISCONNECT_MAX_REPORTS) return;
    int64_t now = esp_timer_get_time();
    if (s_disc_count && now - s_disc_last_us < DISCONNECT_MIN_GAP_US) return;
    s_disc_busy = true;
    s_disc_last_us = now;
    s_disc_count++;
    strlcpy(s_disc_why, why, sizeof(s_disc_why));
    if (xTaskCreate(disconnect_task, "disc_rpt", REPORT_STACK, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the disconnect report");
        s_disc_busy = false;
    }
}

void crash_report_print_status(void)
{
    printf("Last reset: %s\n", reset_reason_name(s_reset_reason));
    printf("Core dump in flash: %s\n", coredump_present() ? "yes" : "no");
    if (s_crash_pending) {
        xSemaphoreTake(s_report_lock, portMAX_DELAY);
        bool saved = save_crash_locked();
        xSemaphoreGive(s_report_lock);
        printf(saved ? "Crash report saved.\n" : "Crash report pending: no microSD card.\n");
    }
    printf("Reports written this boot: %d%s%s\n", s_reports_written, s_last_report[0] ? ", last " : "",
           s_last_report);
}

// ---- web access -------------------------------------------------------------

// NNNN-word.txt or NNNN-word.elf, nothing else: the name becomes a path.
static bool valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len < 10 || len > 40) return false;
    for (int i = 0; i < 4; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    if (name[4] != '-') return false;
    const char *dot = strrchr(name, '.');
    if (!dot || (strcmp(dot, ".txt") && strcmp(dot, ".elf"))) return false;
    for (const char *p = name + 5; p < dot; p++) {
        if (*p < 'a' || *p > 'z') return false;
    }
    return dot > name + 5;
}

esp_err_t crash_report_list(crash_report_file_cb cb, void *ctx)
{
    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    esp_err_t err = sdcard_mount();
    DIR *d = err == ESP_OK ? opendir(CRASH_DIR) : NULL;
    if (d) {
        struct dirent *e;
        char path[300];
        while ((e = readdir(d))) {
            if (!valid_name(e->d_name)) continue;
            struct stat st;
            snprintf(path, sizeof(path), CRASH_DIR "/%s", e->d_name);
            if (stat(path, &st) == 0) cb(ctx, e->d_name, (long)st.st_size);
        }
        closedir(d);
    }
    xSemaphoreGive(s_report_lock);
    return err;
}

FILE *crash_report_open(const char *name)
{
    if (!valid_name(name) || sdcard_mount() != ESP_OK) return NULL;
    char path[64];
    snprintf(path, sizeof(path), CRASH_DIR "/%s", name);
    return fopen(path, "rb");
}

int crash_report_delete_all(void)
{
    int n = 0;
    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    DIR *d = sdcard_mount() == ESP_OK ? opendir(CRASH_DIR) : NULL;
    if (d) {
        struct dirent *e;
        char path[300];
        while ((e = readdir(d))) {
            if (!valid_name(e->d_name)) continue;
            snprintf(path, sizeof(path), CRASH_DIR "/%s", e->d_name);
            if (remove(path) == 0) n++;
        }
        closedir(d);
    }
    s_last_report[0] = 0;
    xSemaphoreGive(s_report_lock);
    return n;
}

const char *crash_report_last_reset(void)
{
    return reset_reason_name(s_reset_reason);
}
