# T-Deck build target

Date: 2026-09-28
Status: approved design, awaiting spec review

## Goal

Build Scheff for Zello for the LilyGO T-Deck / T-Deck Plus (ESP32-S3) as a
second PlatformIO target alongside the T-Display-P4, selectable from
`scripts/build-upload-monitor.sh`. The T-Deck must be usable on its own:
set up Wi-Fi and the Zello account, pick channels, and talk, all on the
device.

Hardware references: camillia-mt `src/hal/hw_tdeck.h` and `[env:tdeck]`,
wadamesh `variants/lilygo_tdeck/`. Neither drives the T-Deck microphone; the
ES7210 pins come from LilyGO's T-Deck pin map.

## Decisions

| Question | Decision |
|---|---|
| Model | T-Deck / T-Deck Plus only (not Pro, not Max) |
| Code structure | `#if CONFIG_IDF_TARGET_ESP32S3` branches inside the existing hardware files; no per-board folders |
| Push-to-talk | Trackball click (hold), on-screen Talk button, and space bar (toggle) |
| UI | Same screens at compact sizes; landscape home screen with the Talk button along the bottom |

## 1. Build target and configuration

- `platformio.ini`: new `[env:tdeck]` on the same pioarduino platform as
  `tdisplay-p4`, `board = tdeck`, 16 MB flash, `partitions.csv`.
  `default_envs` stays `tdisplay-p4`.
- `boards/tdeck.json`: ESP32-S3, 240 MHz, 16 MB QIO flash, 8 MB octal PSRAM,
  USB-Serial/JTAG console.
- sdkconfig layering:
  - `sdkconfig.defaults` keeps only what both boards share (partition table,
    LVGL, fonts, FATFS, HTTP server, mbedTLS, FreeRTOS).
  - `sdkconfig.defaults.esp32p4` (loaded by ESP-IDF for the P4 target) takes
    the P4-only lines: `IDF_EXPERIMENTAL_FEATURES`, hex PSRAM at 200 MHz,
    `SPIRAM_MALLOC_RESERVE_INTERNAL`, `SPIRAM_XIP_FROM_PSRAM`, the L2 cache
    settings, and the ESP-Hosted options.
  - `sdkconfig.defaults.esp32s3`: octal PSRAM at 80 MHz, CPU 240 MHz, Wi-Fi
    and lwIP buffers in PSRAM where the options allow, Montserrat 14 and 16.
  - `sdkconfig.pio.defaults` is split: the path-with-a-space fix
    (`COMPILER_HIDE_PATHS_MACROS=n`) stays shared, and the ECO2 revision lines
    move to `sdkconfig.pio.p4.defaults`. Each env lists its files in
    `board_build.cmake_extra_args`.
- `main/idf_component.yml`: `esp_hosted` and `esp_wifi_remote` are already
  limited to esp32p4. Add `espressif/esp_lcd_touch_gt911`, limited to esp32s3.
- `main/CMakeLists.txt`: add `input_tdeck.c` to the sources only when
  `IDF_TARGET` is esp32s3; add `esp_driver_spi`, `esp_adc` and
  `esp_driver_ledc` to the requirements.

## 2. Build script

`scripts/build-upload-monitor.sh`:

- New `--tdeck` flag that sets `ENV_NAME=tdeck`, next to the existing
  `--tdisplay-p4`.
- `--help` (`show_usage`) documents `--tdeck` and `--tdisplay-p4`, says the
  P4 is the default, and shows an example T-Deck command:
  `./scripts/build-upload-monitor.sh --tdeck --fullclean`.
- Unknown-argument errors print the same usage text, as today.
- The build directory already ends in the env name, so the two targets never
  share build output.
- When the chosen env differs from the last one built, or the env's build
  directory has no `firmware.elf` yet, print a note that `--fullclean` is
  recommended (PlatformIO keeps a stale `memory.ld` across config changes).

## 3. Documentation

- **README.md**: a T-Deck section covering the supported models (T-Deck and
  T-Deck Plus), the build and flash command (`--tdeck`, `--fullclean` on the
  first build), the controls (trackball hold, space bar toggle, screen Talk
  button, keyboard Enter on the home screen), and what differs from the P4
  (volume on the Account screen, no ESP-Hosted coprocessor firmware to
  flash). The existing build instructions name both targets.
- **Script help**: see section 2.

## 4. Hardware (per-file `#if` branches)

T-Deck pins:

| Function | Pins |
|---|---|
| Peripheral power (POWERON) | GPIO10, drive high first |
| I2C (keyboard 0x55, GT911 0x5D, ES7210 0x40) | SDA 18, SCL 8 |
| SPI2 (LCD, SD, unused LoRa) | SCK 40, MOSI 41, MISO 38 |
| ST7789 LCD | CS 12, DC 11, backlight 42 |
| GT911 touch interrupt | GPIO16 |
| Keyboard interrupt | GPIO46 |
| microSD | CS 39 |
| LoRa (unused, keep deselected) | CS 9 |
| Speaker amp MAX98357A (I2S0) | BCK 7, WS 5, DOUT 6 |
| Mic ADC ES7210 (I2S1) | MCLK 48, SCK 47, LRCK 21, DIN 14 |
| Trackball | up 3, down 2, left 1, right 15, click 0 |
| Battery ADC (2:1 divider) | GPIO4 |

- **board.c / board.h**: set POWERON high; hold LoRa CS and SD CS high; one
  I2C bus returned by both `board_main_i2c_bus()` and
  `board_codec_i2c_bus()`; initialise the shared SPI2 bus. Battery level from
  a calibrated ADC reading of GPIO4, times two, mapped to 0-100% through a
  small voltage table. P4-only functions (`board_c6_enable`,
  `board_sd_power`, the reset-line functions) are only declared and built for
  the P4; `board_audio_power` is a no-op on the S3.
- **display.c**: ST7789 through `esp_lcd` SPI panel IO at 40 MHz, colours
  inverted, rotated to 320x240 landscape. `esp_lvgl_port` with partial
  rendering into two internal-DMA draw buffers of about 24 lines each and
  byte-swapped RGB565. Backlight by LEDC PWM on GPIO42 (as camillia-mt). GT911
  touch through `esp_lcd_touch_gt911`. `display_reveal()` and
  `display_set_brightness()` keep their current meaning; the DSI test-pattern
  functions return `ESP_ERR_NOT_SUPPORTED` on the S3.
- **audio.c**: two `esp_codec_dev` devices. Output: I2S0 standard mode to the
  MAX98357A with no codec interface, volume in software. Input: I2S1 from the
  ES7210 through `es7210_codec_new()`. Both at 16 kHz mono. The public
  `audio_*` API does not change.
- **ptt.c**: on the S3, poll GPIO0 (trackball click) instead of the BOOT
  button, with the same debounce and `ptt_latch` handling. It counts only
  while the home screen is active; elsewhere the click is the LVGL "enter"
  key. The space bar toggles talking on the home screen when no text field
  has focus, whatever `ptt_latch` is set to.
- **input_tdeck.c** (new, S3 only): keyboard polled over I2C and registered
  as an LVGL keypad (characters, Enter, Backspace); trackball direction lines
  decoded into an LVGL encoder. On the home screen the keyboard's Enter
  activates the focused control, because the click there means talk.
- **sdcard.c**: SDSPI on the shared SPI2 bus, CS 39, 20 MHz, mounted at the
  same path.
- **net_wifi.c**: ESP-Hosted bring-up stays for the P4; the S3 calls
  `esp_wifi_init()` directly. The rest of the Wi-Fi code is shared.

## 5. UI

- **Sizes** (`ui_internal.h`): an S3 block redefines the `UI_*` constants:
  padding 6, gap 6, button 36 px, field 32 px, radius 6, safe margins 0,
  Talk button 56 px, and scaled status bar, spinner and icon sizes. Fonts:
  Montserrat 14 (small), 16 (normal), 20 (title). It also defines
  `UI_LANDSCAPE 1` and `UI_HAS_KEYBOARD 1`.
- **Text entry**: with `UI_HAS_KEYBOARD`, `ui.c` does not create the
  on-screen keyboard, and the `lv_keyboard_set_mode` calls in
  `ui_account.c` and `ui_channels.c` are compiled out.
- **Focus**: each screen adds its buttons, fields and lists to an LVGL group
  that the trackball and keyboard are attached to.
- **Home screen** (`UI_LANDSCAPE`), top to bottom: a header row with the
  channel name and status bar; a row of small Channels and Account buttons;
  the activity log, filling the remaining height; a thin level meter; the
  full-width Talk button.
- **Volume**: on the T-Deck the volume slider moves from the home screen to
  the Account/settings screen.
- **Other screens** keep their single-column layouts at the compact sizes and
  scroll where needed.

## 6. Testing

No automated tests exist; each step is verified by building and on hardware.

1. **Builds**
   - `./scripts/build-upload-monitor.sh --tdeck --just-build --fullclean`
     builds the T-Deck firmware.
   - `./scripts/build-upload-monitor.sh --tdisplay-p4 --just-build --fullclean`
     still builds the P4 firmware, and `readelf -lW firmware.elf` shows code
     at 0x48000020.
   - `--help` lists both targets with the T-Deck example command.
   - README.md has the T-Deck section.
2. **P4 unchanged** (user, on hardware): boots, UI works, push-to-talk works,
   no blue blinking.
3. **T-Deck bring-up** (user, one step per flash):
   1. Display shows the UI; touch and the trackball move focus.
   2. The keyboard types into the Wi-Fi password field; Wi-Fi connects.
   3. The Zello login succeeds; the startup log shows free internal RAM.
   4. The speaker plays incoming audio at the set volume.
   5. The `loopback` console command confirms the right ES7210 channel.
   6. Trackball hold, space bar toggle and screen button all transmit.
   7. The SD card mounts and a channel list import works.
   8. Battery percentage is plausible.

Done means both targets build from the script, the P4 is unchanged, and
steps 3.1-3.8 pass.

## Risks

- **ES7210 channel**: which input channel carries the T-Deck mic is
  unconfirmed; step 3.5 settles it.
- **Backlight**: LilyGO's example dims with pulses (AW9364); camillia-mt
  uses PWM successfully. Fall back to pulses if brightness misbehaves.
- **Internal RAM**: the S3 has far less internal RAM than the P4 for TLS,
  Wi-Fi, LVGL buffers and the Opus encoder together. Free internal RAM is
  logged at start-up; buffers move to PSRAM where it runs short.
