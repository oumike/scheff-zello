// The app as a desktop window: the simulator's, and the CardputerZero's when
// started from its desktop. The window resizes freely and the UI is laid out
// at its size (in points), so nothing is scaled; F11 toggles full screen, and
// the size and place are kept for next time. SDL's video runs on the main
// thread (macOS requires it); LVGL's thread hands frames over through
// sdlwin_push() and picks up new sizes with sdlwin_take_resize().
//
// The keyboard skips the CardputerZero's keymap: text arrives as SDL lays it
// out, and keys mean what they say on them (cp0_keys_push()).

#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cp0_keys.h"
#include "esp_log.h"
#include "linux_hal.h"

static const char *TAG = "window";

#define DEFAULT_W 800
#define DEFAULT_H 520
#define MIN_W     480
#define MIN_H     400
#define STATE_FILE "window.conf"

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static int s_tex_w, s_tex_h;

// The last frame LVGL drew, at the size it drew it.
static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *s_shown;
static int s_fw, s_fh;
static bool s_dirty = true;
static atomic_int s_level = 255;
static atomic_bool s_redraw = true;

static atomic_int s_w, s_h;
static atomic_bool s_resized;

static atomic_int s_mouse_x = -1, s_mouse_y = -1;
static atomic_bool s_mouse_down;
static atomic_int s_wheel_x, s_wheel_y;
static atomic_int s_cursor_want;
static SDL_Cursor *s_cursors[3];

static bool s_quit;
static bool s_space_down;

// The window's size and place when neither maximized nor full screen, kept
// for next time.
static int s_norm_x, s_norm_y, s_norm_w = DEFAULT_W, s_norm_h = DEFAULT_H;
static bool s_norm_known;

// ---- keyboard ----------------------------------------------------------------

static void paste(void)
{
    char *text = SDL_GetClipboardText();
    if (!text) return;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            cp0_keys_push(' ', true);  // the character, not the talk key
        } else if (c > 0x20 && c < 0x7f) {
            cp0_keys_push(c, true);
        }
    }
    SDL_free(text);
}

static void toggle_fullscreen(void)
{
    bool full = SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(s_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static void on_key(const SDL_KeyboardEvent *k)
{
    bool down = k->type == SDL_KEYDOWN;
    SDL_Keycode sym = k->keysym.sym;
    SDL_Keymod mod = k->keysym.mod;
    bool cmd = (mod & (KMOD_CTRL | KMOD_GUI)) != 0;

    // Space is the talk key: a press and a release, without repeats. A
    // release always goes through, so talking can't get stuck on.
    if (sym == SDLK_SPACE) {
        if (down && (k->repeat || cmd || s_space_down)) return;
        if (!down && !s_space_down) return;
        s_space_down = down;
        cp0_keys_push(CP0_K_SPACE, down);
        return;
    }
    if (!down) return;

    if (cmd && sym == SDLK_q) {
        s_quit = true;
        return;
    }
    if (sym == SDLK_F11 || (sym == SDLK_f && (mod & KMOD_CTRL) && (mod & KMOD_GUI))) {
        if (!k->repeat) toggle_fullscreen();
        return;
    }
    if (cmd && sym == SDLK_v) {
        paste();
        return;
    }

    uint32_t key = 0;
    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: key = CP0_K_ENTER; break;
    case SDLK_ESCAPE: key = k->repeat ? 0 : CP0_K_ESC; break;
    case SDLK_TAB: key = (mod & KMOD_SHIFT) ? CP0_K_BACKTAB : CP0_K_TAB; break;
    case SDLK_BACKSPACE: key = CP0_K_BACKSPACE; break;
    case SDLK_DELETE: key = CP0_K_DELETE; break;
    case SDLK_UP: key = cmd ? CP0_K_VOL_UP : CP0_K_UP; break;
    case SDLK_DOWN: key = cmd ? CP0_K_VOL_DOWN : CP0_K_DOWN; break;
    case SDLK_PAGEUP: key = CP0_K_UP; break;
    case SDLK_PAGEDOWN: key = CP0_K_DOWN; break;
    case SDLK_LEFT: key = CP0_K_LEFT; break;
    case SDLK_RIGHT: key = CP0_K_RIGHT; break;
    case SDLK_HOME: key = CP0_K_HOME; break;
    case SDLK_END: key = CP0_K_END; break;
    case SDLK_VOLUMEUP: key = CP0_K_VOL_UP; break;
    case SDLK_VOLUMEDOWN: key = CP0_K_VOL_DOWN; break;
    default: break;
    }
    if (key) cp0_keys_push(key, true);
}

// Typed text, in the keyboard's own layout. Space comes as the talk key; the
// UI's fonts are ASCII only.
static void on_text(const char *text)
{
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 0x20 && c < 0x7f) cp0_keys_push(c, true);
    }
}

// The window lost the keyboard or the mouse mid-press: let go.
static void release_all(void)
{
    if (s_space_down) {
        s_space_down = false;
        cp0_keys_push(CP0_K_SPACE, false);
    }
    atomic_store(&s_mouse_down, false);
}

// ---- size and place ------------------------------------------------------------

static void note_geometry(void)
{
    Uint32 flags = SDL_GetWindowFlags(s_window);
    if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED))
        return;
    SDL_GetWindowPosition(s_window, &s_norm_x, &s_norm_y);
    SDL_GetWindowSize(s_window, &s_norm_w, &s_norm_h);
    s_norm_known = true;
}

static void note_size(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSize(s_window, &w, &h);
    if (w < 1 || h < 1) return;  // minimized
    if (w != atomic_load(&s_w) || h != atomic_load(&s_h)) {
        atomic_store(&s_w, w);
        atomic_store(&s_h, h);
        atomic_store(&s_resized, true);
    }
}

// Whether at least a corner's worth of the rectangle is on some display.
static bool on_a_display(int x, int y, int w, int h)
{
    SDL_Rect r = {x, y, w, h};
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect d, both;
        if (SDL_GetDisplayBounds(i, &d) == 0 && SDL_IntersectRect(&r, &d, &both) && both.w >= 64 && both.h >= 64)
            return true;
    }
    return false;
}

static bool load_state(int *x, int *y, int *w, int *h, bool *maximized)
{
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return false;
    int max = 0;
    bool ok = fscanf(f, "%d %d %d %d %d", x, y, w, h, &max) == 5;
    fclose(f);
    *maximized = max != 0;
    if (!ok || *w < MIN_W || *h < MIN_H || *w > 16384 || *h > 16384) return false;
    return true;
}

static void save_state(void)
{
    if (!s_window) return;
    note_geometry();
    if (!s_norm_known) return;
    bool maximized = SDL_GetWindowFlags(s_window) & SDL_WINDOW_MAXIMIZED;
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d\n", s_norm_x, s_norm_y, s_norm_w, s_norm_h, maximized ? 1 : 0);
    fclose(f);
}

bool sdlwin_open(const char *title)
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // The desktop entry's StartupWMClass matches this.
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "scheff-zello");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "scheff-zello");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ESP_LOGE(TAG, "SDL_Init: %s", SDL_GetError());
        return false;
    }

    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = DEFAULT_W, h = DEFAULT_H;
    bool maximized = false;
    int sx, sy, sw, sh;
    if (load_state(&sx, &sy, &sw, &sh, &maximized)) {
        w = sw;
        h = sh;
        if (on_a_display(sx, sy, sw, sh)) {
            x = sx;
            y = sy;
        }
    }
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | (maximized ? SDL_WINDOW_MAXIMIZED : 0);
    s_window = SDL_CreateWindow(title, x, y, w, h, flags);
    s_renderer = s_window ? SDL_CreateRenderer(s_window, -1, 0) : NULL;
    if (!s_renderer) {
        ESP_LOGE(TAG, "%s", SDL_GetError());
        sdlwin_close(NULL);
        return false;
    }
    SDL_SetWindowMinimumSize(s_window, MIN_W, MIN_H);
    note_geometry();
    note_size();
    atomic_store(&s_resized, false);

    s_cursors[SDLWIN_CURSOR_ARROW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    s_cursors[SDLWIN_CURSOR_HAND] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    s_cursors[SDLWIN_CURSOR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    SDL_StartTextInput();
    return true;
}

void sdlwin_size(int *w, int *h)
{
    *w = atomic_load(&s_w);
    *h = atomic_load(&s_h);
}

bool sdlwin_take_resize(int *w, int *h)
{
    if (!atomic_exchange(&s_resized, false)) return false;
    sdlwin_size(w, h);
    return true;
}

// ---- drawing -------------------------------------------------------------------

static void present(void)
{
    pthread_mutex_lock(&s_frame_lock);
    bool dirty = s_dirty && s_shown;
    if (dirty && (s_tex_w != s_fw || s_tex_h != s_fh || !s_texture)) {
        if (s_texture) SDL_DestroyTexture(s_texture);
        s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, s_fw, s_fh);
        s_tex_w = s_texture ? s_fw : 0;
        s_tex_h = s_texture ? s_fh : 0;
    }
    if (dirty && s_texture) SDL_UpdateTexture(s_texture, NULL, s_shown, s_fw * 2);
    s_dirty = false;
    pthread_mutex_unlock(&s_frame_lock);
    if (!dirty && !atomic_exchange(&s_redraw, false)) return;
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    if (s_texture) {
        uint8_t v = (uint8_t)atomic_load(&s_level);
        SDL_SetTextureColorMod(s_texture, v, v, v);
        SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    }
    SDL_RenderPresent(s_renderer);
}

// Window points to UI pixels: the same, unless a frame at the new size hasn't
// arrived yet.
static void to_ui(int x, int y)
{
    int ww = 1, wh = 1;
    SDL_GetWindowSize(s_window, &ww, &wh);
    pthread_mutex_lock(&s_frame_lock);
    int fw = s_fw ? s_fw : ww, fh = s_fh ? s_fh : wh;
    pthread_mutex_unlock(&s_frame_lock);
    atomic_store(&s_mouse_x, x * fw / (ww > 0 ? ww : 1));
    atomic_store(&s_mouse_y, y * fh / (wh > 0 ? wh : 1));
}

static void on_window(const SDL_WindowEvent *w)
{
    switch (w->event) {
    case SDL_WINDOWEVENT_FOCUS_GAINED: cp0_keys_set_focus(true); break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        cp0_keys_set_focus(false);
        release_all();
        break;
    case SDL_WINDOWEVENT_LEAVE:
        // Nothing is hovered once the mouse is gone.
        if (!atomic_load(&s_mouse_down)) {
            atomic_store(&s_mouse_x, -1);
            atomic_store(&s_mouse_y, -1);
        }
        break;
    case SDL_WINDOWEVENT_MOVED: note_geometry(); break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_RESIZED:
    case SDL_WINDOWEVENT_MAXIMIZED:
    case SDL_WINDOWEVENT_RESTORED:
        note_geometry();
        note_size();
        break;
    default: break;
    }
    atomic_store(&s_redraw, true);
}

void sdlwin_run(bool (*quit)(void))
{
    int cursor = -1;
    while (!quit() && !s_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                return;
            case SDL_WINDOWEVENT:
                on_window(&e.window);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                on_key(&e.key);
                break;
            case SDL_TEXTINPUT:
                on_text(e.text.text);
                break;
            case SDL_MOUSEMOTION:
                to_ui(e.motion.x, e.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    to_ui(e.button.x, e.button.y);
                    atomic_store(&s_mouse_down, e.type == SDL_MOUSEBUTTONDOWN);
                }
                break;
            case SDL_MOUSEWHEEL: {
                float x = e.wheel.preciseX, y = e.wheel.preciseY;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    x = -x;
                    y = -y;
                }
                // SDL's x is positive to the right; ours is towards the start.
                atomic_fetch_add(&s_wheel_x, (int)(-x * 100));
                atomic_fetch_add(&s_wheel_y, (int)(y * 100));
                break;
            }
            default:
                break;
            }
        }
        int want = atomic_load(&s_cursor_want);
        if (want != cursor && s_cursors[want]) {
            SDL_SetCursor(s_cursors[want]);
            cursor = want;
        }
        present();
        SDL_Delay(8);
    }
}

// The last frame as a 24-bit BMP, for checks that can't see the window.
static void write_shot(const char *path)
{
    pthread_mutex_lock(&s_frame_lock);
    const int w = s_fw, h = s_fh;
    FILE *f = s_shown ? fopen(path, "wb") : NULL;
    if (!f) {
        pthread_mutex_unlock(&s_frame_lock);
        return;
    }
    const uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, data = row * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
#define PUT32(at, v) for (int i_ = 0; i_ < 4; i_++) hdr[(at) + i_] = (uint8_t)((uint32_t)(v) >> (8 * i_))
    PUT32(2, 54 + data);
    PUT32(10, 54);
    PUT32(14, 40);
    PUT32(18, w);
    PUT32(22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    PUT32(34, data);
#undef PUT32
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t *line = calloc(1, row);
    for (int y = h - 1; line && y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t p = s_shown[y * w + x];
            line[x * 3 + 0] = (uint8_t)((p & 0x1f) << 3);
            line[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) << 2);
            line[x * 3 + 2] = (uint8_t)((p >> 11) << 3);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    pthread_mutex_unlock(&s_frame_lock);
    fclose(f);
}

void sdlwin_close(const char *shot)
{
    if (shot && *shot) write_shot(shot);
    save_state();
    for (int i = 0; i < 3; i++) {
        if (s_cursors[i]) SDL_FreeCursor(s_cursors[i]);
        s_cursors[i] = NULL;
    }
    if (s_texture) SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void sdlwin_push(const uint16_t *frame, int w, int h, int y0, int y1)
{
    if (!frame || w < 1 || h < 1) return;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (y1 <= y0) return;
    pthread_mutex_lock(&s_frame_lock);
    if (w != s_fw || h != s_fh) {
        // A new size: the whole frame, or what is drawn so far of it.
        uint16_t *buf = realloc(s_shown, (size_t)w * h * 2);
        if (!buf) {
            pthread_mutex_unlock(&s_frame_lock);
            return;
        }
        s_shown = buf;
        s_fw = w;
        s_fh = h;
        y0 = 0;
        y1 = h;
    }
    memcpy(&s_shown[y0 * w], frame + y0 * w, (size_t)(y1 - y0) * w * 2);
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

void sdlwin_take_wheel(int *x100, int *y100)
{
    *x100 = atomic_exchange(&s_wheel_x, 0);
    *y100 = atomic_exchange(&s_wheel_y, 0);
}

void sdlwin_set_cursor(sdlwin_cursor_t cursor)
{
    atomic_store(&s_cursor_want, (int)cursor);
}
