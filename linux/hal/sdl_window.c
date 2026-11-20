// A fixed-size window showing the panel, scaled up: the simulator's screen,
// and the app's on the CardputerZero's desktop. SDL's video runs on the main
// thread (macOS requires it); LVGL's thread hands frames over through
// sdlwin_push().

#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "esp_log.h"
#include "linux_hal.h"

static const char *TAG = "window";

#define W DISPLAY_H_RES
#define H DISPLAY_V_RES

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static int s_scale = 1;

static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t s_shown[W * H];
static bool s_dirty = true;
static atomic_int s_level = 255;
static atomic_bool s_redraw = true;

static atomic_int s_mouse_x, s_mouse_y;
static atomic_bool s_mouse_down;

// SDL's key to the CardputerZero's evdev code, so the keymap is the same.
static uint16_t evdev_code(SDL_Keycode k, bool shift)
{
    static const uint16_t letters[] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                       49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    if (k >= SDLK_a && k <= SDLK_z) return letters[k - SDLK_a];
    if (k >= SDLK_1 && k <= SDLK_9) return (uint16_t)(2 + (k - SDLK_1));
    // The sym layer's codes, from the launcher's built-in table.
    switch (k) {
    case SDLK_0: return 11;
    case SDLK_MINUS: return 12;
    case SDLK_EQUALS: return 13;
    case SDLK_SPACE: return 57;
    case SDLK_ESCAPE: return 1;
    case SDLK_RETURN: return 28;
    case SDLK_KP_ENTER: return 96;
    case SDLK_TAB: return 15;
    case SDLK_BACKSPACE: return 14;
    case SDLK_DELETE: return 111;
    case SDLK_UP: return 103;
    case SDLK_DOWN: return 108;
    case SDLK_LEFT: return 105;
    case SDLK_RIGHT: return 106;
    case SDLK_HOME: return 102;
    case SDLK_END: return 107;
    case SDLK_PAGEUP: return 104;
    case SDLK_PAGEDOWN: return 109;
    case SDLK_LSHIFT: return 42;
    case SDLK_RSHIFT: return 54;
    case SDLK_LALT: return 0x1d0;  // fn
    case SDLK_LCTRL: return 29;
    case SDLK_BACKQUOTE: return shift ? 55 : 69;
    case SDLK_LEFTBRACKET: return shift ? 76 : 74;
    case SDLK_RIGHTBRACKET: return shift ? 77 : 75;
    case SDLK_BACKSLASH: return shift ? 89 : 86;
    case SDLK_SEMICOLON: return shift ? 80 : 79;
    case SDLK_QUOTE: return shift ? 82 : 81;
    case SDLK_COMMA: return shift ? 83 : 90;
    case SDLK_PERIOD: return shift ? 85 : 91;
    case SDLK_SLASH: return shift ? 93 : 92;
    default: return 0;
    }
}

// The code each held scancode went down as, so its release matches.
static uint16_t s_down[SDL_NUM_SCANCODES];

static void on_key(const SDL_KeyboardEvent *k)
{
    if (k->repeat || k->keysym.scancode >= SDL_NUM_SCANCODES) return;
    uint16_t code;
    if (k->type == SDL_KEYDOWN) {
        code = evdev_code(k->keysym.sym, (k->keysym.mod & KMOD_SHIFT) != 0);
        if (!code) return;
        s_down[k->keysym.scancode] = code;
    } else {
        code = s_down[k->keysym.scancode];
        if (!code) return;
        s_down[k->keysym.scancode] = 0;
    }
    cp0_keys_feed(code, k->type == SDL_KEYDOWN ? 1 : 0, hal_now_ms());
}

bool sdlwin_open(int w, int h, int scale, const char *title)
{
    (void)w;
    (void)h;
    s_scale = scale < 1 ? 1 : scale;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // The desktop entry's StartupWMClass matches this.
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "scheff-zello");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "scheff-zello");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ESP_LOGE(TAG, "SDL_Init: %s", SDL_GetError());
        return false;
    }
    s_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * s_scale,
                                H * s_scale, SDL_WINDOW_ALLOW_HIGHDPI);
    s_renderer = s_window ? SDL_CreateRenderer(s_window, -1, 0) : NULL;
    if (s_renderer) {
        SDL_RenderSetLogicalSize(s_renderer, W, H);
        s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
    }
    if (!s_texture) {
        ESP_LOGE(TAG, "%s", SDL_GetError());
        sdlwin_close(NULL);
        return false;
    }
    return true;
}

static void present(void)
{
    pthread_mutex_lock(&s_frame_lock);
    bool dirty = s_dirty;
    if (dirty) SDL_UpdateTexture(s_texture, NULL, s_shown, W * 2);
    s_dirty = false;
    pthread_mutex_unlock(&s_frame_lock);
    if (!dirty && !atomic_exchange(&s_redraw, false)) return;
    uint8_t v = (uint8_t)atomic_load(&s_level);
    SDL_SetTextureColorMod(s_texture, v, v, v);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

// Window points to panel pixels (the renderer's logical size does the scale).
static void to_panel(int x, int y)
{
    int ww = 1, wh = 1;
    SDL_GetWindowSize(s_window, &ww, &wh);
    atomic_store(&s_mouse_x, x * W / (ww > 0 ? ww : 1));
    atomic_store(&s_mouse_y, y * H / (wh > 0 ? wh : 1));
}

void sdlwin_run(bool (*quit)(void))
{
    while (!quit()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                return;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) cp0_keys_set_focus(true);
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) cp0_keys_set_focus(false);
                atomic_store(&s_redraw, true);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                on_key(&e.key);
                break;
            case SDL_MOUSEMOTION:
                to_panel(e.motion.x, e.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    to_panel(e.button.x, e.button.y);
                    atomic_store(&s_mouse_down, e.type == SDL_MOUSEBUTTONDOWN);
                }
                break;
            default:
                break;
            }
        }
        present();
        SDL_Delay(10);
    }
}

// The last frame as a 24-bit BMP, for checks that can't see the window.
static void write_shot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t row = (W * 3 + 3) & ~3u, data = row * H;
    uint8_t hdr[54] = {'B', 'M'};
#define PUT32(at, v) for (int i_ = 0; i_ < 4; i_++) hdr[(at) + i_] = (uint8_t)((uint32_t)(v) >> (8 * i_))
    PUT32(2, 54 + data);
    PUT32(10, 54);
    PUT32(14, 40);
    PUT32(18, W);
    PUT32(22, H);
    hdr[26] = 1;
    hdr[28] = 24;
    PUT32(34, data);
#undef PUT32
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t line[(W * 3 + 3) & ~3u];
    memset(line, 0, sizeof(line));
    pthread_mutex_lock(&s_frame_lock);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            uint16_t p = s_shown[y * W + x];
            line[x * 3 + 0] = (uint8_t)((p & 0x1f) << 3);
            line[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) << 2);
            line[x * 3 + 2] = (uint8_t)((p >> 11) << 3);
        }
        fwrite(line, 1, sizeof(line), f);
    }
    pthread_mutex_unlock(&s_frame_lock);
    fclose(f);
}

void sdlwin_close(const char *shot)
{
    if (shot && *shot) write_shot(shot);
    if (s_texture) SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void sdlwin_push(const uint16_t *frame, int y0, int y1)
{
    if (!frame) return;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    pthread_mutex_lock(&s_frame_lock);
    memcpy(&s_shown[y0 * W], frame + y0 * W, (size_t)(y1 - y0) * W * 2);
    s_dirty = true;
    pthread_mutex_unlock(&s_frame_lock);
}

void sdlwin_set_level(uint8_t level)
{
    atomic_store(&s_level, level);
    atomic_store(&s_redraw, true);
}

bool sdlwin_pointer(int16_t *x, int16_t *y)
{
    *x = (int16_t)atomic_load(&s_mouse_x);
    *y = (int16_t)atomic_load(&s_mouse_y);
    return atomic_load(&s_mouse_down);
}
