// The CardputerZero's keyboard: raw evdev from the kernel's TCA8418 driver,
// read on its own thread, through the launcher's keymap (the US layout with
// shift, the sym layer, and the printed fn layer). On the LCD the device isn't
// grabbed, so the launcher still sees a long Esc; in a desktop window it is
// grabbed while the window has focus, so the desktop doesn't get the keys too.
// The simulator feeds SDL's keys in as the same evdev codes.

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cp0_keys.h"
#include "esp_log.h"
#include "linux_hal.h"

#if defined(__linux__) && !defined(ZELLO_LINUX_SIM)
#define HAVE_EVDEV 1
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

static const char *TAG = "keys";

// Linux evdev codes (linux/input-event-codes.h), spelled out so the
// simulator needs no Linux headers.
enum {
    K_ESC = 1, K_1 = 2, K_0 = 11, K_MINUS = 12, K_EQUAL = 13, K_BACKSPACE = 14, K_TAB = 15,
    K_Q = 16, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_P,
    K_ENTER = 28, K_LEFTCTRL = 29,
    K_A = 30, K_S, K_D, K_F, K_G, K_H, K_J, K_K, K_L,
    K_LEFTSHIFT = 42,
    K_Z = 44, K_X, K_C, K_V, K_B, K_N, K_M,
    K_RIGHTSHIFT = 54, K_LEFTALT = 56, K_SPACE = 57,
    K_KPENTER = 96, K_HOME = 102, K_UP = 103, K_PAGEUP = 104, K_LEFT = 105, K_RIGHT = 106, K_END = 107,
    K_DOWN = 108, K_PAGEDOWN = 109, K_DELETE = 111,
    K_VOLUMEDOWN = 114, K_VOLUMEUP = 115, K_COMPOSE = 127,
    K_BRIGHTNESSDOWN = 224, K_BRIGHTNESSUP = 225,
    K_FN = 0x1d0,
};

#define REPEAT_DELAY_MS 500
#define REPEAT_MS       70
#define QUIT_HOLD_MS    1000
#define MOD_SHIFT 1
#define MOD_FN    2

#define LAUNCHER_KEYBOARD "/dev/input/by-path/platform-3f804000.i2c-event"
#define MODIFIER_STATE    "/sys/bus/i2c/devices/1-0034/modifier_state"
#define SYM_MAP_FILE      "/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map"

typedef struct {
    uint16_t code;
    uint8_t value;  // 1 press, 0 release
    uint8_t mods;   // MOD_* from the driver, read at the press
    uint32_t ms;
} raw_key_t;

#define RAW_LEN 128
static raw_key_t s_raw[RAW_LEN];
static unsigned s_raw_head, s_raw_len;
static pthread_mutex_t s_raw_lock = PTHREAD_MUTEX_INITIALIZER;

static atomic_bool s_running, s_focused, s_quit;

// The keymap's state: touched only from cp0_keys_next() (the LVGL thread).
static bool s_lshift, s_rshift, s_fn, s_ctrl, s_esc_down;
static uint32_t s_esc_ms;
static uint16_t s_held_code;
static uint32_t s_held_key, s_next_repeat_ms;

// The sym layer: the launcher's built-in table (CardputerZero/launcher,
// cp0_keyboard_keymap.c), replaced by the system keymap file when present.
#define SYM_MAX 64
static struct {
    uint16_t code;
    char ch;
} s_sym[SYM_MAX] = {
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
};
static size_t s_sym_count = 32;

uint32_t hal_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void cp0_keys_feed(uint16_t code, uint8_t value, uint32_t ms)
{
    pthread_mutex_lock(&s_raw_lock);
    if (s_raw_len < RAW_LEN) s_raw[(s_raw_head + s_raw_len++) % RAW_LEN] = (raw_key_t){code, value, 0, ms};
    pthread_mutex_unlock(&s_raw_lock);
}

static void feed_mods(uint16_t code, uint8_t value, uint8_t mods, uint32_t ms)
{
    pthread_mutex_lock(&s_raw_lock);
    if (s_raw_len < RAW_LEN) s_raw[(s_raw_head + s_raw_len++) % RAW_LEN] = (raw_key_t){code, value, mods, ms};
    pthread_mutex_unlock(&s_raw_lock);
}

void cp0_keys_set_focus(bool focused) { atomic_store(&s_focused, focused); }
bool cp0_keys_quit_requested(void) { return atomic_load(&s_quit); }

// ---- the keymap ------------------------------------------------------------

static const struct {
    const char *name;
    char ch;
} KEYSYM_NAMES[] = {
    {"space", ' '},       {"exclam", '!'},      {"quotedbl", '"'},     {"numbersign", '#'},
    {"dollar", '$'},      {"percent", '%'},     {"ampersand", '&'},    {"apostrophe", '\''},
    {"quoteright", '\''}, {"parenleft", '('},   {"parenright", ')'},   {"asterisk", '*'},
    {"plus", '+'},        {"comma", ','},       {"minus", '-'},        {"period", '.'},
    {"slash", '/'},       {"colon", ':'},       {"semicolon", ';'},    {"less", '<'},
    {"equal", '='},       {"greater", '>'},     {"question", '?'},     {"at", '@'},
    {"bracketleft", '['}, {"backslash", '\\'},  {"bracketright", ']'}, {"asciicircum", '^'},
    {"underscore", '_'},  {"grave", '`'},       {"quoteleft", '`'},    {"braceleft", '{'},
    {"bar", '|'},         {"braceright", '}'},  {"asciitilde", '~'},
};

// A keysym name's ASCII character, or 0.
static char keysym_char(const char *name)
{
    size_t n = strlen(name);
    if (n == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') ||
                   (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    for (size_t i = 0; i < sizeof(KEYSYM_NAMES) / sizeof(KEYSYM_NAMES[0]); i++)
        if (!strcmp(KEYSYM_NAMES[i].name, name)) return KEYSYM_NAMES[i].ch;
    if (n >= 5 && n <= 7 && name[0] == 'U') {
        unsigned long cp = strtoul(name + 1, NULL, 16);
        if (cp >= 0x20 && cp < 0x7f) return (char)cp;
    }
    return 0;
}

static void load_sym_map(void)
{
    FILE *f = fopen(SYM_MAP_FILE, "r");
    if (!f) return;
    char line[256];
    size_t count = 0;
    __typeof__(s_sym) parsed;
    while (fgets(line, sizeof(line), f) && count < SYM_MAX) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned code;
        char name[64];
        if (sscanf(line, " keycode %u = %63s", &code, name) != 2 || code > 0xffff) continue;
        char ch = keysym_char(name);
        if (ch) {
            parsed[count].code = (uint16_t)code;
            parsed[count].ch = ch;
            count++;
        }
    }
    fclose(f);
    if (!count) return;
    memcpy(s_sym, parsed, sizeof(parsed[0]) * count);
    s_sym_count = count;
    ESP_LOGI(TAG, "sym table from the system keymap (%u keys)", (unsigned)count);
}

static char sym_char(uint16_t code)
{
    for (size_t i = 0; i < s_sym_count; i++)
        if (s_sym[i].code == code) return s_sym[i].ch;
    return 0;
}

// The US layout's character for a key that types, or 0.
static char us_char(uint16_t code, bool shift)
{
    static const char top[] = "qwertyuiop", home[] = "asdfghjkl", bottom[] = "zxcvbnm";
    static const char digits[] = "1234567890", shifted_digits[] = "!@#$%^&*()";
    char c = 0;
    if (code >= K_Q && code <= K_P) c = top[code - K_Q];
    else if (code >= K_A && code <= K_L) c = home[code - K_A];
    else if (code >= K_Z && code <= K_M) c = bottom[code - K_Z];
    if (c) return shift ? (char)(c - 32) : c;
    if (code >= K_1 && code <= K_0) return shift ? shifted_digits[code - K_1] : digits[code - K_1];
    if (code == K_MINUS) return shift ? '_' : '-';
    if (code == K_EQUAL) return shift ? '+' : '=';
    return 0;
}

static bool is_modifier(uint16_t code)
{
    return code == K_LEFTSHIFT || code == K_RIGHTSHIFT || code == K_FN || code == K_COMPOSE ||
           code == K_LEFTCTRL || code == K_LEFTALT;
}

static bool repeats(uint32_t key)
{
    return key == CP0_K_BACKSPACE || key == CP0_K_UP || key == CP0_K_DOWN || key == CP0_K_LEFT ||
           key == CP0_K_RIGHT;
}

// Keys that mean the same whatever the modifiers; 0 for none. Brightness is
// done here.
static uint32_t fixed_key(uint16_t code, bool shift)
{
    switch (code) {
    case K_ENTER: case K_KPENTER: return CP0_K_ENTER;
    case K_TAB: return shift ? CP0_K_BACKTAB : CP0_K_TAB;
    case K_BACKSPACE: return CP0_K_BACKSPACE;
    case K_DELETE: return CP0_K_DELETE;
    case K_UP: case K_PAGEUP: return CP0_K_UP;
    case K_DOWN: case K_PAGEDOWN: return CP0_K_DOWN;
    case K_LEFT: return CP0_K_LEFT;
    case K_RIGHT: return CP0_K_RIGHT;
    case K_HOME: return CP0_K_HOME;
    case K_END: return CP0_K_END;
    case K_VOLUMEUP: return CP0_K_VOL_UP;
    case K_VOLUMEDOWN: return CP0_K_VOL_DOWN;
    case K_BRIGHTNESSUP: hal_backlight_step(32); return 0;
    case K_BRIGHTNESSDOWN: hal_backlight_step(-32); return 0;
    default: return 0;
    }
}

// The printed fn layer; 0 for none.
static uint32_t fn_key(uint16_t code)
{
    switch (code) {
    case K_F: case K_L: return CP0_K_UP;
    case K_X: case K_M: return CP0_K_DOWN;
    case K_Z: return CP0_K_LEFT;
    case K_C: return CP0_K_RIGHT;
    case K_K: return CP0_K_HOME;
    case K_BACKSPACE: return CP0_K_DELETE;
    case K_S: return CP0_K_VOL_DOWN;
    case K_D: return CP0_K_VOL_UP;
    case K_U: hal_backlight_step(-32); return 0;
    case K_I: hal_backlight_step(32); return 0;
    default: return 0;
    }
}

static bool press(const raw_key_t *in, cp0_key_event_t *out)
{
    bool shift = s_lshift || s_rshift || (in->mods & MOD_SHIFT);
    bool fn = s_fn || (in->mods & MOD_FN);
    if (in->code == K_SPACE && !fn && !s_ctrl) {
        *out = (cp0_key_event_t){CP0_K_SPACE, true};
        return true;
    }
    uint32_t key = fixed_key(in->code, shift);
    if (!key && fn) key = fn_key(in->code);
    if (key) {
        *out = (cp0_key_event_t){key, true};
        if (repeats(key)) {
            s_held_code = in->code;
            s_held_key = key;
            s_next_repeat_ms = in->ms + REPEAT_DELAY_MS;
        }
        return true;
    }
    if (fn || s_ctrl) return false;
    char c = sym_char(in->code);
    if (!c) c = us_char(in->code, shift);
    if (!c) return false;
    *out = (cp0_key_event_t){(uint32_t)(unsigned char)c, true};
    return true;
}

static bool feed(const raw_key_t *in, cp0_key_event_t *out)
{
    if (s_esc_down && in->ms - s_esc_ms >= QUIT_HOLD_MS) atomic_store(&s_quit, true);
    bool down = in->value == 1, up = in->value == 0;
    switch (in->code) {
    case K_LEFTSHIFT: s_lshift = down; return false;
    case K_RIGHTSHIFT: s_rshift = down; return false;
    case K_FN: case K_LEFTALT: s_fn = down; return false;
    case K_LEFTCTRL: s_ctrl = down; return false;
    case K_ESC:
        if (down && !s_esc_down) {
            s_esc_down = true;
            s_esc_ms = in->ms;
        } else if (up && s_esc_down) {
            s_esc_down = false;
            if (atomic_load(&s_quit)) return false;
            *out = (cp0_key_event_t){CP0_K_ESC, true};
            return true;
        }
        return false;
    case K_SPACE:
        if (up) {
            *out = (cp0_key_event_t){CP0_K_SPACE, false};
            return true;
        }
        break;
    default: break;
    }
    if (up) {
        if (in->code == s_held_code) s_held_code = 0;
        return false;
    }
    if (!down || is_modifier(in->code)) return false;
    return press(in, out);
}

bool cp0_keys_next(cp0_key_event_t *ev)
{
    for (;;) {
        raw_key_t in;
        pthread_mutex_lock(&s_raw_lock);
        bool have = s_raw_len > 0;
        if (have) {
            in = s_raw[s_raw_head];
            s_raw_head = (s_raw_head + 1) % RAW_LEN;
            s_raw_len--;
        }
        pthread_mutex_unlock(&s_raw_lock);
        if (!have) break;
        if (feed(&in, ev)) return true;
    }
    uint32_t now = hal_now_ms();
    if (s_esc_down && now - s_esc_ms >= QUIT_HOLD_MS) atomic_store(&s_quit, true);
    if (!s_held_code || (int32_t)(now - s_next_repeat_ms) < 0) return false;
    s_next_repeat_ms = now + REPEAT_MS;
    *ev = (cp0_key_event_t){s_held_key, true};
    return true;
}

// ---- the evdev reader -------------------------------------------------------

#if HAVE_EVDEV

static pthread_t s_reader;

static bool has_key_a(int fd)
{
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    const size_t w = 8 * sizeof(unsigned long);
    return (bits[KEY_A / w] >> (KEY_A % w)) & 1;
}

// SCHEFF_KEYBOARD_DEVICE, APPLAUNCH_LINUX_KEYBOARD_DEVICE, the launcher's
// default, else the first event node with KEY_A.
static int open_keyboard(char *path, size_t size)
{
    const char *envs[] = {"SCHEFF_KEYBOARD_DEVICE", "APPLAUNCH_LINUX_KEYBOARD_DEVICE"};
    for (size_t i = 0; i < 2; i++) {
        const char *p = getenv(envs[i]);
        if (p && *p) {
            snprintf(path, size, "%s", p);
            return open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
    }
    snprintf(path, size, "%s", LAUNCHER_KEYBOARD);
    int fd = open(LAUNCHER_KEYBOARD, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) return fd;
    for (int i = 0; i < 32; i++) {
        snprintf(path, size, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (has_key_a(fd)) return fd;
        close(fd);
    }
    path[0] = 0;
    return -1;
}

// modifier_state's shift and fn (0 inactive, 1 held, 2 one-shot, 3 locked)
// as MOD_* bits; 0 when the file isn't there.
static uint8_t modifier_state(void)
{
    FILE *f = fopen(MODIFIER_STATE, "r");
    if (!f) return 0;
    uint8_t mods = 0;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char name[16] = {0};
        int v = 0;
        if (sscanf(line, " %15[a-z_] %*[:=]%d", name, &v) != 2 && sscanf(line, " %15[a-z_] %d", name, &v) != 2)
            continue;
        if (v <= 0) continue;
        if (!strcmp(name, "shift")) mods |= MOD_SHIFT;
        if (!strcmp(name, "fn")) mods |= MOD_FN;
    }
    fclose(f);
    return mods;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    int fd = -1;
    char path[96];
    bool told = false, grabbed = false;
    const bool window = hal_windowed();
    while (atomic_load(&s_running)) {
        if (fd < 0) {
            fd = open_keyboard(path, sizeof(path));
            if (fd < 0) {
                if (!told) ESP_LOGW(TAG, "no keyboard found; retrying");
                told = true;
                usleep(1000 * 1000);
                continue;
            }
            ESP_LOGI(TAG, "keyboard %s", path);
            told = false;
            grabbed = false;
        }
        bool focused = atomic_load(&s_focused);
        if (window && grabbed != focused) {
            if (ioctl(fd, EVIOCGRAB, focused ? 1 : 0) == 0 || !focused) grabbed = focused;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) continue;
        struct input_event ev[16];
        ssize_t n = read(fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            close(fd);  // unplugged or gone: look again
            fd = -1;
            continue;
        }
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            // Value 2 is the kernel's autorepeat; the keymap repeats itself.
            if (ev[i].type != EV_KEY || ev[i].value > 1) continue;
            if (window && !grabbed) continue;  // the desktop has these
            uint8_t mods = ev[i].value == 1 ? modifier_state() : 0;
            feed_mods(ev[i].code, (uint8_t)ev[i].value, mods, hal_now_ms());
        }
    }
    if (fd >= 0) {
        if (grabbed) ioctl(fd, EVIOCGRAB, 0);
        close(fd);
    }
    return NULL;
}

#endif

esp_err_t cp0_keys_start(void)
{
    load_sym_map();
#if HAVE_EVDEV
    atomic_store(&s_running, true);
    if (pthread_create(&s_reader, NULL, reader_thread, NULL) != 0) return ESP_FAIL;
#endif
    return ESP_OK;
}

void cp0_keys_stop(void)
{
#if HAVE_EVDEV
    if (atomic_exchange(&s_running, false)) pthread_join(s_reader, NULL);
#endif
}
