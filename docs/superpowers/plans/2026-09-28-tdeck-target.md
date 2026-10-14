# T-Deck Build Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build Scheff for Zello for the LilyGO T-Deck / T-Deck Plus as a second PlatformIO env, selectable with `--tdeck` in the build script, without changing the T-Display-P4 build.

**Architecture:** Hardware files branch on `CONFIG_IDF_TARGET_ESP32S3` / `CONFIG_IDF_TARGET_ESP32P4`. Target-only sdkconfig lines move to `sdkconfig.defaults.<target>`, which ESP-IDF layers on automatically. A new S3-only `input_tdeck.c` turns the keyboard and trackball into one LVGL keypad and keeps an LVGL focus group in step with whatever screen or overlay is in front. The UI reads board-specific sizes from `ui_internal.h`.

**Tech Stack:** ESP-IDF 5.5.5 via pioarduino 55.03.312-1, LVGL 9.3 + esp_lvgl_port 2.6, esp_codec_dev 1.5 (ES7210), esp_lcd ST7789, esp_lcd_touch_gt911.

**Spec:** `docs/superpowers/specs/2026-09-28-tdeck-target-design.md`

## Global Constraints

- The P4 build must behave exactly as before; every P4 path keeps its current code.
- The T-Deck target covers the T-Deck and T-Deck Plus only.
- The public headers (`audio.h`, `display.h`, `ptt.h`, `sdcard.h`, `net_wifi.h`, `ui.h`) keep their existing functions; new ones are additions.
- Pins are the table in spec section 4.
- No automated test framework exists. Each task's check is a build of both envs (`--just-build`), plus the hardware steps in spec section 6 that the user runs.
- Build with `--fullclean` after any sdkconfig change (stale `memory.ld`).

## Review Focus

1. Switching envs without `--fullclean`: the script must warn, and the P4 ELF must still link code at 0x48000020.
2. A text field focused on the T-Deck: space must type a space, not key the transmitter.
3. The trackball click held while an overlay opens over Home: talking must stop when Home loses focus.
4. No SD card inserted on the T-Deck: mount fails quietly and the SPI bus stays usable for the display.
5. The ES7210 mic on the wrong channel: loopback shows silence; the channel is set in one place.

---

### Task 1: Build config, board file, build script and docs

**Files:**
- Create: `boards/tdeck.json`, `sdkconfig.defaults.esp32p4`, `sdkconfig.defaults.esp32s3`, `sdkconfig.pio.p4.defaults`
- Modify: `sdkconfig.defaults`, `sdkconfig.pio.defaults`, `platformio.ini`, `main/idf_component.yml`, `main/CMakeLists.txt`, `scripts/build-upload-monitor.sh`, `README.md`

**Interfaces:**
- Produces: env name `tdeck`; build script flags `--tdeck`, `--tdisplay-p4`.

- [ ] Move `IDF_EXPERIMENTAL_FEATURES`, all `SPIRAM_*`, `CACHE_L2_*` and `ESP_HOSTED_*` lines from `sdkconfig.defaults` into `sdkconfig.defaults.esp32p4`, keeping their comments.
- [ ] Write `sdkconfig.defaults.esp32s3`: `SPIRAM=y`, `SPIRAM_MODE_OCT=y`, `SPIRAM_SPEED_80M=y`, `ESP_DEFAULT_CPU_FREQ_MHZ_240=y`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, `SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, `ESP_CONSOLE_USB_SERIAL_JTAG=y`, `LV_FONT_MONTSERRAT_14=y`, `LV_FONT_MONTSERRAT_16=y`.
- [ ] Move the ECO2 lines to `sdkconfig.pio.p4.defaults`; `sdkconfig.pio.defaults` keeps `COMPILER_HIDE_PATHS_MACROS=n`.
- [ ] `platformio.ini`: P4 env `cmake_extra_args = -DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.pio.defaults;sdkconfig.pio.p4.defaults`; add `[env:tdeck]` (same platform, `board = tdeck`, same partitions/flash settings, `upload_speed = 921600`, `-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.pio.defaults`).
- [ ] `boards/tdeck.json`: mcu esp32s3, f_cpu 240 MHz, flash 16 MB qio, `arduino.memory_type` qio_opi, psram.
- [ ] `idf_component.yml`: `espressif/esp_lcd_touch_gt911: "^1.1"` with rule `target in [esp32s3]`.
- [ ] `main/CMakeLists.txt`: add `input_tdeck.c` when `IDF_TARGET STREQUAL esp32s3`; add `esp_driver_spi esp_driver_ledc esp_adc` to `PRIV_REQUIRES`.
- [ ] Build script: `--tdeck` and `--tdisplay-p4` set `ENV_NAME`; `show_usage` documents both, the default and the example; warn to use `--fullclean` when `$BUILD_DIR/.last-env` differs from `ENV_NAME` or `firmware.elf` is missing, and record `ENV_NAME` in `.last-env` after a successful build.
- [ ] README: T-Deck section per spec section 3.
- [ ] Verify: `--help` shows both flags; the P4 builds with `--fullclean`, and `readelf -lW` shows 0x48000020.

### Task 2: Board layer for the S3

**Files:** Modify `main/board.h`, `main/board.c`.

**Interfaces:**
- Produces (S3): `board_init()`, `board_main_i2c_bus()`, `board_codec_i2c_bus()` (the same bus), `board_battery_percent()`, `board_audio_power()` (no-op), `board_spi_host()` returning `SPI2_HOST`, plus the pin `#define`s `BOARD_LCD_*`, `BOARD_SD_CS`, `BOARD_TOUCH_INT`, `BOARD_KB_ADDR`, `BOARD_KB_INT`, `BOARD_TBALL_*`, `BOARD_SPK_*`, `BOARD_MIC_*`, `BOARD_ES7210_ADDR`, `BOARD_PTT_BUTTON`.
- P4 keeps `BOARD_BOOT_BUTTON`; both targets define `BOARD_PTT_BUTTON` (the BOOT button on the P4, the trackball click on the S3).

- [ ] Wrap the P4 pin block and the P4-only prototypes in `#if CONFIG_IDF_TARGET_ESP32P4`; add the S3 block.
- [ ] In `board.c`, wrap the existing body in `#if CONFIG_IDF_TARGET_ESP32P4` and add the S3 implementation: POWERON high plus a 100 ms delay; LoRa CS and SD CS as outputs held high; I2C0 on 18/8; `spi_bus_initialize(SPI2_HOST, …, SPI_DMA_CH_AUTO)` with `max_transfer_sz = 320 * 24 * 2`; battery via `adc_oneshot` + curve-fitting calibration on ADC1 channel 3 (GPIO4), 8 samples, ×2, mapped through a LiPo table (4.20 V→100 … 3.30 V→0).

### Task 3: Display, touch and backlight

**Files:** Modify `main/display.h`, `main/display.c`.

- [ ] `display.h`: `DISPLAY_H_RES`/`DISPLAY_V_RES` are 320/240 on the S3.
- [ ] `display.c`: the S3 branch uses `esp_lcd_new_panel_io_spi` (CS 12, DC 11, 40 MHz, 8-bit command/parameter, `trans_queue_depth 10`) and `esp_lcd_new_panel_st7789` (RGB element order, 16 bpp), then reset, init, `invert_color(true)` and display on. LVGL port: `buffer_size = 320 * 24`, double buffer, `buff_dma`, `swap_bytes`, rotation `{swap_xy = true, mirror_x = true, mirror_y = false}`. GT911 via `esp_lcd_touch_new_i2c_gt911` on the board I2C bus, `int_gpio_num = 16`, x_max 240, y_max 320, `swap_xy = 1`, `mirror_y = 1` (LilyGO's T-Deck setting), added with `lvgl_port_add_touch`. Backlight: LEDC timer 0, 13-bit, 5 kHz on GPIO42, starting at 0; `display_set_brightness(level)` sets the duty to `level * 8191 / 255`. Test-pattern functions return `ESP_ERR_NOT_SUPPORTED`. Log free internal RAM.

### Task 4: Audio

**Files:** Modify `main/audio.c`.

- [ ] S3 `codec_open()`: I2S0 TX only (std Philips, 16-bit mono slot, BCK 7, WS 5, DOUT 6) → `esp_codec_dev` `OUT` device with `codec_if = NULL` (software volume). I2S1 RX only (std, 16-bit stereo, MCLK 48, BCK 47, WS 21, DIN 14, `mclk_multiple` 256) → `es7210_codec_new` (addr `0x40 << 1`, `mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2`) → `IN` device, opened mono. `s_out` / `s_in` replace `s_codec` on the S3 (P4 sets both to the one device). A `MIC_CHANNEL_MASK` define is the single place to change the mic channel.

### Task 5: SD card and Wi-Fi

**Files:** Modify `main/sdcard.c`, `main/net_wifi.c`.

- [ ] `sdcard.c` S3: `SDSPI_HOST_DEFAULT()` with `slot = board_spi_host()`, `max_freq_khz = 20000`, device `gpio_cs = BOARD_SD_CS`, `esp_vfs_fat_sdspi_mount`; `sdmmc_get_status` recheck unchanged; no power control.
- [ ] `net_wifi.c`: guard the `esp_hosted` includes, `c6_reset_cb` and the hosted bring-up with `#if CONFIG_IDF_TARGET_ESP32P4`.

### Task 6: Input and push-to-talk

**Files:** Create `main/input_tdeck.h`, `main/input_tdeck.c`; modify `main/ptt.c`, `main/ptt.h`, `main/app_main.c`, `main/ui_home.c`.

**Interfaces:**
- Produces: `esp_err_t input_tdeck_init(void)` (called after `display_init`, S3 only); `void input_tdeck_set_talk_screen(lv_obj_t *scr)` (called by `ui_home_show`); `bool input_tdeck_talk_active(void)` (true while Home is the focus root; read by `ptt.c`).

- [ ] Keypad indev: each read pops one queued key and reports it pressed, then released on the next call. Sources: keyboard (I2C read of 1 byte from 0x55; 0x0D→ENTER, 0x08→BACKSPACE, 0x1B→ESC, printable ASCII as is) and trackball (negative-edge ISR counters: up/down → PREV/NEXT, left/right → LEFT/RIGHT; with an open dropdown, up/down → UP/DOWN; click → ENTER unless Home is the root).
- [ ] Space on Home with no text field focused → `zello_set_ptt(!zello_ptt())`, swallowed.
- [ ] Focus group timer (150 ms): the root is the last visible full-screen child of `lv_layer_top()`, otherwise `lv_screen_active()`. Collect visible buttons, text areas, sliders, dropdowns, switches and checkboxes in tree order; rebuild the group when the list changes, keeping the focused object if it is still there. Store `s_talk_active = (root == talk screen)`.
- [ ] `ptt.c`: poll `BOARD_PTT_BUTTON`; on the S3, treat it as released while `!input_tdeck_talk_active()` (so leaving Home ends a hold).

### Task 7: UI sizes and Home layout

**Files:** Modify `main/ui_internal.h`, `main/ui.c`, `main/ui_home.c`, `main/ui_account.c`.

- [ ] `ui_internal.h`: an S3 block with padding 6, gap 6, button 36, field 32, radius 6, safe margins 0, status bar 120×24, spinner 40, icon button 32, edit button 30, clear button 32, Talk button 56, slider 14, volume value 44, meter 6, and fonts 14/16/20; `UI_LANDSCAPE 1`, `UI_HAS_KEYBOARD 1`. The P4 block gets `UI_LANDSCAPE 0` and `UI_HAS_KEYBOARD 0`.
- [ ] `ui.c`: with `UI_HAS_KEYBOARD`, `field_event` never un-hides the on-screen keyboard (it stays created and hidden, so every caller keeps working).
- [ ] `ui_home.c` with `UI_LANDSCAPE`: a header row (channel on the left, status bar space on the right; the username is dropped), a buttons row, the log box (flex-grow), the meter and the Talk button; no volume row. Call `input_tdeck_set_talk_screen(s_scr)` on the S3.
- [ ] `ui_account.c` with `UI_LANDSCAPE`: in `ui_account_edit`, add a "Volume" slider row (same handler as Home's) under the Zello account fields.
- [ ] Verify: both envs build with `--fullclean`; P4 `readelf` shows 0x48000020.
