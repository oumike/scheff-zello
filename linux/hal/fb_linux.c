// The CardputerZero's ST7789 through the kernel's framebuffer: a 320x170
// panel, the UI drawn 2 px in from the sides and top (the glass covers the
// edges), the margin repeating the UI's edge pixels.

#include "linux_hal.h"
#include "display.h"
#include "esp_log.h"

#if defined(__linux__) && !defined(ZELLO_LINUX_SIM)

#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *TAG = "fb";

#define PANEL_W   320
#define PANEL_H   170
#define INSET_X   2
#define INSET_TOP 2
#define UI_W      DISPLAY_LCD_H_RES
#define UI_H      DISPLAY_LCD_V_RES

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fd = -1;
static void *s_base;
static uint8_t *s_mem;
static size_t s_mem_len;
static uint32_t s_line_len;
static int s_fb_w, s_fb_h, s_bpp;
static struct fb_bitfield s_red, s_green, s_blue;
static uint16_t s_kept[UI_W * UI_H];  // the last frame, for fb_dark(false)
static bool s_dark;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// $LV_LINUX_FBDEV_DEVICE, else /dev/fb_lcd, else the /proc/fb entry named
// panel-mipi-dbid or fb_st7789v, else /dev/fb0.
static void panel_node(char *out, size_t size)
{
    const char *d = getenv("LV_LINUX_FBDEV_DEVICE");
    if (d && *d) {
        snprintf(out, size, "%s", d);
        return;
    }
    if (access("/dev/fb_lcd", R_OK | W_OK) == 0) {
        snprintf(out, size, "/dev/fb_lcd");
        return;
    }
    FILE *f = fopen("/proc/fb", "r");
    if (f) {
        int n;
        char name[64];
        while (fscanf(f, "%d %63s", &n, name) == 2) {
            if (!strcmp(name, "panel-mipi-dbid") || !strcmp(name, "fb_st7789v")) {
                fclose(f);
                snprintf(out, size, "/dev/fb%d", n);
                return;
            }
        }
        fclose(f);
    }
    snprintf(out, size, "/dev/fb0");
}

static uint32_t put_field(uint32_t v8, const struct fb_bitfield *f)
{
    return f->length >= 8 ? v8 << (f->offset + f->length - 8) : (v8 >> (8 - f->length)) << f->offset;
}

static uint32_t expand(uint16_t p)
{
    uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    uint32_t r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
    return put_field(r8, &s_red) | put_field(g8, &s_green) | put_field(b8, &s_blue);
}

// Panel rows [y0, y1) from src, or black when src is NULL. s_lock held.
static void blit(const uint16_t *src, int y0, int y1)
{
    int w = PANEL_W < s_fb_w ? PANEL_W : s_fb_w;
    if (y0 < 0) y0 = 0;
    if (y1 > s_fb_h) y1 = s_fb_h;
    for (int y = y0; y < y1; y++) {
        uint8_t *row = s_mem + (size_t)y * s_line_len;
        if (!src) {
            memset(row, 0, (size_t)w * (size_t)(s_bpp / 8));
            continue;
        }
        const uint16_t *in = src + (size_t)clampi(y - INSET_TOP, 0, UI_H - 1) * UI_W;
        if (s_bpp == 16) {
            uint16_t *out = (uint16_t *)row;
            for (int x = 0; x < w; x++) out[x] = in[clampi(x - INSET_X, 0, UI_W - 1)];
        } else {
            uint32_t *out = (uint32_t *)row;
            for (int x = 0; x < w; x++) out[x] = expand(in[clampi(x - INSET_X, 0, UI_W - 1)]);
        }
    }
}

// The panel rows a UI frame's rows [y0, y1) reach, the margins included.
static void blit_ui(const uint16_t *src, int y0, int y1)
{
    blit(src, y0 == 0 ? 0 : y0 + INSET_TOP, y1 >= UI_H ? PANEL_H : y1 + INSET_TOP);
}

bool fb_open(void)
{
    char node[64];
    panel_node(node, sizeof(node));
    pthread_mutex_lock(&s_lock);
    s_fd = open(node, O_RDWR | O_CLOEXEC);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "can't open %s", node);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    struct fb_var_screeninfo var = {0};
    struct fb_fix_screeninfo fix = {0};
    if (ioctl(s_fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(s_fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
        (var.bits_per_pixel != 16 && var.bits_per_pixel != 32)) {
        ESP_LOGE(TAG, "%s: unsupported (%u bpp)", node, var.bits_per_pixel);
        goto fail;
    }
    s_fb_w = (int)var.xres;
    s_fb_h = (int)var.yres;
    s_bpp = (int)var.bits_per_pixel;
    s_red = var.red;
    s_green = var.green;
    s_blue = var.blue;
    s_line_len = fix.line_length ? fix.line_length : (uint32_t)(s_fb_w * s_bpp / 8);
    s_mem_len = fix.smem_len ? fix.smem_len : (size_t)s_line_len * s_fb_h;
    void *m = mmap(NULL, s_mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, 0);
    if (m == MAP_FAILED) {
        ESP_LOGE(TAG, "%s: mmap failed", node);
        goto fail;
    }
    s_base = m;
    s_mem = (uint8_t *)m + (size_t)var.yoffset * s_line_len + (size_t)var.xoffset * (s_bpp / 8);
    ESP_LOGI(TAG, "%s %dx%d %d bpp", node, s_fb_w, s_fb_h, s_bpp);
    if (s_fb_w != PANEL_W || s_fb_h != PANEL_H)
        ESP_LOGW(TAG, "expected %dx%d; drawing the top-left corner", PANEL_W, PANEL_H);
    blit(NULL, 0, s_fb_h);
    pthread_mutex_unlock(&s_lock);
    return true;
fail:
    close(s_fd);
    s_fd = -1;
    pthread_mutex_unlock(&s_lock);
    return false;
}

void fb_push(const uint16_t *frame, int y0, int y1)
{
    if (!frame) return;
    if (y0 < 0) y0 = 0;
    if (y1 > UI_H) y1 = UI_H;
    if (y1 <= y0) return;
    pthread_mutex_lock(&s_lock);
    memcpy(&s_kept[(size_t)y0 * UI_W], frame + (size_t)y0 * UI_W, (size_t)(y1 - y0) * UI_W * 2);
    if (s_mem && !s_dark) blit_ui(frame, y0, y1);
    pthread_mutex_unlock(&s_lock);
}

void fb_dark(bool on)
{
    pthread_mutex_lock(&s_lock);
    if (s_dark != on) {
        s_dark = on;
        if (s_mem) {
            if (on) blit(NULL, 0, s_fb_h);
            else blit_ui(s_kept, 0, UI_H);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void fb_close(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_mem) {
        blit(NULL, 0, s_fb_h);
        munmap(s_base, s_mem_len);
        close(s_fd);
        s_base = NULL;
        s_mem = NULL;
        s_fd = -1;
    }
    pthread_mutex_unlock(&s_lock);
}

#else

bool fb_open(void) { return false; }
void fb_push(const uint16_t *frame, int y0, int y1) { (void)frame; (void)y0; (void)y1; }
void fb_dark(bool on) { (void)on; }
void fb_close(void) {}

#endif
