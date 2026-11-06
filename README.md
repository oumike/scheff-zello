# Scheff for Zello

**Scheff for Zello** (`scheff-zello`) is a [Zello](https://zello.com) push-to-talk client for the **LilyGO T-Display-P4**
(ESP32-P4 + ESP32-C6), the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3), the
**Seeed Wio Tracker L2** (ESP32-S3) and the **Nicolai Electronics Tanmatsu**
(ESP32-P4 + ESP32-C6), built on ESP-IDF with an LVGL UI.

It signs in to the [Zello Channels API](https://github.com/zelloptt/zello-channel-api)
over a WebSocket, joins one channel, and carries voice both ways as Opus through
the board's own microphone and speaker — no extra hardware — along with the
channel's text messages, which every board can read and send. Everything is configurable from the touch screen or from a browser.

Board bring-up (display, touch, audio, power rails, Wi-Fi coprocessor) follows
`camillia-mt` and `echolink-p4`, and the setup flow is deliberately the same
shape as `echolink-p4`'s: a setup hotspot on first boot, step-by-step
onboarding mirrored on screen and in the browser, and one YAML file that backs up
every setting.

```
           ESP32-P4 ──── ES8311 codec ── mic / NS4150B speaker
              │  │
              │  └────── RM69A10 AMOLED 568x1232 + GT9895 touch (MIPI-DSI)
              │
        SDIO  └───────── ESP32-C6 (ESP-Hosted) ── Wi-Fi ── wss://zello.io/ws
```

## Status

Builds clean and flashes. The protocol, audio path, UI and web config are all
implemented; it has not yet been through a long soak test on hardware.

## Hardware

### LilyGO T-Display-P4

- **LilyGO T-Display-P4 V1.0** (ECO2 silicon, 16 MB flash, HEX PSRAM).
  `boards/tdisplay_p4.json` and `sdkconfig.pio.p4.defaults` select the pre-rev-3.0
  chip revision; on rev 3.0+ silicon drop `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`.
- The **ESP32-C6** coprocessor must be running ESP-Hosted slave firmware
  matching `third_party/esp_hosted` (v2.12.x) — the LilygoBox factory image
  does. `scripts/flash-c6.sh` reflashes it if another project overwrote it.
- Push-to-talk is the big button on screen. The **BOOT button** is the screen
  button (see *Screensaver* below).

### LilyGO T-Deck / T-Deck Plus

- ESP32-S3 with 16 MB flash and 8 MB PSRAM; the Wi-Fi is the S3's own, so there
  is no coprocessor firmware to flash. The T-Deck Pro and T-Deck Max are
  different boards and are not supported.
- 320x240 LCD with touch, the QWERTY keyboard and the trackball all work. The
  UI is the same as the P4's at a compact size; the keyboard types into text
  fields, and the trackball moves between controls.
- Push-to-talk, on the home screen:
  - **Hold the trackball click** to talk.
  - **Space** starts talking and a second press stops (the keyboard can only
    report presses, not a key being held).
  - The **Talk button** along the bottom of the screen, by touch.
- Elsewhere the trackball click selects; on the home screen, where the click
  means talk, **Enter** presses the control the trackball has highlighted.
- **Hold the trackball click** for 2 s anywhere but the home screen to bring up
  the screensaver; hold it again, or type any key, to wake.
- Audio is the ES7210 microphone ADC and the MAX98357A speaker amplifier. The
  LoRa radio and GPS are left off.

### Seeed Wio Tracker L2

- ESP32-S3 with 16 MB flash and 8 MB PSRAM, using the S3's own Wi-Fi.
- 320x240 NV3031B LCD (QSPI) with GT911 touch. It has no keyboard, so text is
  typed on the on-screen keyboard; the UI is the T-Deck's compact layout.
- Push-to-talk: **hold the button on GPIO0**, or the **Talk button** on screen.
  The **Wake** button on top is the screen button.
- Home puts the volume up the right-hand side of the activity panel, so the
  panel gets that row's height.
- Audio is the ES8311 DAC into the speaker amplifier and the ES7243E microphone
  ADC, sharing one I2S port. Which ES7243E channel carries the microphone is not
  confirmed yet: `MIC_SLOT` in `main/audio.c` averages both for now, and the
  boot log reports each channel's peak level.
- The battery is read through the ADS1115 ADC; the LoRa radio and GNSS stay off.

### Nicolai Electronics Tanmatsu

- ESP32-P4 with 16 MB flash and 32 MB PSRAM, Wi-Fi on its ESP32-C6 over
  ESP-Hosted. The board support is [badge-bsp](https://github.com/badgeteam/esp32-component-badge-bsp),
  as in wadamesh; the C6 keeps the ESP-Hosted firmware the badge.team launcher
  installed, and is power-cycled through the coprocessor at boot.
- The 800x480 LCD (turned to landscape) and the full keyboard. Typing anywhere
  on the home screen starts a channel message; **Enter** sends it. Arrows and
  Tab move between controls, Esc goes back, and the volume keys set the volume.
  The **Power** button is the screen button, and any key also wakes it.
- **Listen and text only**: the Tanmatsu has no microphone, so the Talk button
  reads *no microphone* and push-to-talk is ignored.
- Audio is the ES8156 DAC into the speaker and the **headphone jack**. Plugging
  in headphones mutes the speaker (the coprocessor switches the amplifier);
  the volume control drives both.
- Flashing this firmware **replaces the badge.team launcher** and its apps on
  the internal flash. [recovery.tanmatsu.cloud](https://recovery.tanmatsu.cloud)
  puts the launcher back.

## Installing

Flash a release from the browser at
**[oumike.github.io/scheff-zello](https://oumike.github.io/scheff-zello/)**
(desktop Chrome or Edge), or write a `scheff-zello-<board>.bin` from
[Releases](https://github.com/oumike/scheff-zello/releases) at offset 0:
`esptool.py write_flash 0x0 scheff-zello-<board>.bin`. The browser flasher
keeps the saved settings unless you choose to erase; the release `.bin` is the
complete image and resets them.

## Building

PlatformIO fetches and manages ESP-IDF itself; no separate install is needed.

```sh
./scripts/build-upload-monitor.sh            # T-Display-P4: build, flash, then monitor
./scripts/build-upload-monitor.sh --tdeck    # T-Deck: build, flash, then monitor
./scripts/build-upload-monitor.sh --wio-tracker-l2   # Wio Tracker L2
./scripts/build-upload-monitor.sh --tanmatsu # Tanmatsu (replaces the launcher)
./scripts/build-upload-monitor.sh -B         # compile only, no device needed
./scripts/build-upload-monitor.sh --port /dev/cu.usbmodem1101
./scripts/build-upload-monitor.sh -E         # erase flash first (wipes settings)
./scripts/build-upload-monitor.sh --help     # every option
```

ESP-IDF's framework and build directories must resolve to paths without spaces.
The helper falls back to `~/.cache/scheff-zello/platformio` if its default core
resolves through a symlink to a path containing spaces. Explicit paths can be
set with `ZELLO_PIO_CORE_DIR` and `ZELLO_BUILD_DIR`.

Host-side correctness regressions run with `python3 -m unittest discover -s tests -v`
(Python 3, a C compiler, Node.js, and the ESP-IDF framework fetched by PlatformIO;
no device is accessed).

The T-Display-P4 is the default board; `--tdeck` selects the T-Deck,
`--wio-tracker-l2` the Wio Tracker L2 and `--tanmatsu` the Tanmatsu. Each board
builds into its own directory. Add `--fullclean` (`-F`) the first time you build
a board, after switching boards, and after changing memory settings in the
`sdkconfig.defaults*` files: PlatformIO otherwise keeps a stale generated linker
script, and a P4 built that way boot-loops. The script says so when it applies.

Settings shared by every board are in `sdkconfig.defaults`; ESP-IDF layers
`sdkconfig.defaults.esp32p4` or `sdkconfig.defaults.esp32s3` on top for the chip
being built. Boards on the same chip are told apart by the "Board" choice in
`main/Kconfig.projbuild`; the Wio build selects it in `sdkconfig.pio.wio.defaults`
and the Tanmatsu build in `sdkconfig.pio.tanmatsu.defaults`.

The script runs `pio` against a separate core directory (`~/.platformio-p4`), so
the pioarduino platform this project needs cannot be replaced by the
`espressif32` versions other projects resolve. It also builds outside the source
tree, because ESP-IDF refuses a build directory whose path contains a space.

Plain `idf.py build` against an ESP-IDF v5.5.4+ checkout works too.

### CI and releases

GitHub Actions (`.github/workflows/`):

- **Build** runs the host tests and builds every board from a clean checkout on
  each push and pull request to `main`; the factory images are kept as run
  artifacts.
- **Release** (run by hand from the Actions tab) builds every board with the
  new version, commits `VERSION`, tags `vX.Y.Z`, publishes the images as a
  GitHub release and redeploys the site. Leave the version blank to release
  `VERSION` as-is, or its next patch if that is already tagged.
- **Pages** publishes `site/` with the latest release's images and an
  esp-web-tools manifest per board (from `site/devices.json`).
  `scripts/build-site.py` splits each image around its data partitions so the
  web flasher leaves NVS and the channel store alone.

`scripts/package-firmware.py` copies a build's merged `firmware.factory.bin`
to `dist/` after checking its bootloader and partition table offsets.

## Setting it up

Three steps, and each one can be done on the screen **or** in the browser — the
other side follows along.

On first boot the device brings up an open Wi-Fi hotspot named
`Scheff-XXXX`. Join it and open `http://192.168.4.1`; there is deliberately no
captive portal, so nothing pops up or takes over the phone's browser. Once the
device is on your network, open `http://<device-ip>` instead; the address is
shown at the bottom of every setup screen. The hotspot shuts down as soon as the
station link gets an IP.

### 1. Wi-Fi

Scan and tap a network, or type the SSID. The device saves the credentials and
reboots once the join succeeds.

### 2. Zello account

|                     | Zello (zello.io)                | Zello Work                   |
| ------------------- | ------------------------------- | ---------------------------- |
| Username / password | yes                             | yes                          |
| Developer token     | **required**                    | not used                     |
| Network name        | not used                        | **required** (the subdomain) |
| Endpoint            | `wss://zello.io/ws`             | `wss://zellowork.io/ws/<network>` |

The public network requires a **developer token**: a JWT you issue at
[developers.zello.com](https://developers.zello.com). It is far too long to type
on a touch keyboard, so paste it into the web config — the account form on the
device says so rather than pretending otherwise.

Neither the password nor the token is ever sent back to the browser. The page
only learns whether one is stored, and leaving either field blank keeps what is
already saved.

### 3. Channel

Zello has no public channel directory to download, so the channel list is your
own: names you add here, plus every channel a successful sign-in confirms.
Names are matched **exactly**, including spaces and capitals. Favorites sort to
the top, then channels by how often you have joined them.

## Using it

The home screen is also the talk screen:

- **Hold the big button** (or the board's talk button) to talk. It is green when the
  channel is yours, red with the talker's name while someone else holds it, and
  amber while the server is allocating the stream. Zello is half duplex, so a
  press is ignored while someone else is talking.
- Turn on **tap to talk** in the web config's Audio section if holding a button
  through a long call is awkward; the same press then toggles transmit.
- The level meter follows the microphone while transmitting and the speaker
  otherwise.
- Channel activity — who joined, who is talking, text messages, errors — scrolls
  in the panel above the volume slider, and in the browser under *Channel
  activity*. An incoming text message also beeps.
- **Text messages** go to the channel from the *Message the channel* field under the
  activity panel: type, then **Send** (or Enter). On the touch-only boards
  (T-Display-P4, Wio Tracker L2) tapping the field opens the on-screen keyboard,
  which hides the talk controls until it closes; on the T-Deck and Tanmatsu just
  start typing on the home screen (on the T-Deck a leading Space still talks). The browser's *Channel activity* can send
  them too.

### Screensaver

After 2 minutes untouched (once set up), or a tap on the board's screen button,
the screen shows a schefflera bouncing slowly round it, carrying the Zello
status and the channel's latest activity (who is talking, or the last two
lines). Audio keeps playing. After 10 more minutes the screen goes dark.

- **Hold the screen button 2 s** to get back to where you were. A tap does
  nothing while the screensaver is up, and lights it again from a dark screen,
  so a pocket can't wake it.
- Push-to-talk is ignored while the screensaver is up or the screen is dark.
- The screen button is **BOOT** on the T-Display-P4, **Wake** on the Wio
  Tracker L2 and **Power** on the Tanmatsu; the T-Deck uses a held trackball
  click (or any key to wake).

## Backup and restore

*Backup* in the web config writes one YAML file holding **every** setting and
the whole channel list:

```yaml
# Scheff for Zello backup: every setting and the channel list.
scheff-backup: 1
settings:
  network: "consumer"
  username: "n0call"
  password: "..."
  auth_token: "eyJ..."
  work_network: ""
  channel: "My Channel"
  wifi_ssid: "HomeWiFi"
  wifi_pass: "..."
  volume: 70
  mic_gain: 30
  auto_connect: true
  ptt_latch: false
channels:
  - name: "My Channel"
    desc: "Weekend net"
    favorite: true
    uses: 12
```

Download it, or write it to a microSD card as `scheff-zello-backup.yaml`. It is
safe to edit by hand: a file may leave out `settings` or `channels` (or any
single setting), and whatever it leaves out stays as it is on the device; an
empty `channels: []` leaves the list alone too. Import reports the line number
of anything it cannot read and then changes nothing. Old XML backups
(`zello-p4-backup.xml`, from before the rename) still import. Restoring a backup with different Wi-Fi
credentials reconnects on the spot.

On the device, **Settings → Backup** has *Back up* (writes
`scheff-zello-backup.yaml` to the card) and *Restore* (asks before importing
the newest backup on the card). A device that isn't set up yet, after an erase
say, offers that newest backup by itself at startup.

**The backup contains your Wi-Fi password, your Zello password and your
developer token in clear text.** Store it accordingly.

## Serial console

115200 baud, `scheff>` prompt. `help` lists everything; the useful ones:

```
status                      Wi-Fi, sign-in state, channel, packet counts
set                         show every setting
set <key> <value>           change one and save it to NVS
wifi <ssid> [password]      join and save a network
login / logout              sign in to Zello now, or disconnect
channel [<name>]            show or join a channel
channels [<filter>]         list the saved channels
ptt [on|off]                toggle transmit
text <message>              send a text message to the channel
log                         recent channel activity
loopback [on|off]           mic to speaker, for audio bring-up
crash [test]                last reset reason and crash reports; 'test' crashes on purpose
```

`set auth_token <jwt>` works, and the console's line limit is sized for one.

## How it fits together

| File | What it does |
| ---- | ------------ |
| `app_main.c` | Boot order: settings, board, display, audio, Zello, Wi-Fi, web, UI, console |
| `board.c` | XL9535 I/O expander: the audio, C6, SD and display power rails; BQ27220 fuel gauge |
| `display.c` | RM69A10 AMOLED over MIPI-DSI, GT9895 touch, LVGL port |
| `audio.c` | ES8311 at 16 kHz mono, 60 ms frames, capture callback and a jitter-buffered playback task |
| `opus_codec.c` | The Opus encoder/decoder pair and Zello's codec header |
| `zello_client.c` | The Channels API: WebSocket, logon, channel status, audio streams both ways |
| `net_wifi.c` | ESP-Hosted bring-up, station, scan/join, the setup hotspot, SNTP |
| `channel_list.c` | The saved channels, as XML on the internal FAT partition |
| `settings.c` | NVS-backed settings, one table describing every field |
| `onboard.c` | The three onboarding steps, shared by the screen and the browser |
| `crash_report.c` | Crash-surviving log ring, core dump and disconnect reports to microSD |
| `web_config.c` | HTTP API, the setup hotspot's lifetime, backup export/import |
| `ui*.c` | LVGL screens: Wi-Fi, account, channels, home/talk, the screensaver |
| `xml_util.c` | The little XML the stored channel list (and old XML backups) need |
| `yaml_util.c` | The little YAML the backup file needs |

### Audio and the protocol

Transmit is fixed at **16 kHz mono Opus in 60 ms packets** at 24 kbit/s, which
is what the codec header advertises in `start_stream`. Receive is not fixed: the
decoder is created at 16 kHz and libopus resamples whatever the sender used, so
a phone streaming at 8 or 24 kHz plays back correctly with nothing extra here.

Voice packets are Zello's binary frames — one type byte, a big-endian stream id
and packet id, then the Opus payload. Both codec directions run on their own
tasks with room for Opus's stack use, so neither the capture task nor the
WebSocket task ever waits on the codec.

Playback buffers two packets (120 ms) before starting and re-buffers when it
drains, which is the usual trade between latency and Wi-Fi jitter.

### libopus

`components/opus/` is libopus 1.4 as an ESP-IDF component: an unmodified subset
of the upstream release with the x86/ARM/MIPS intrinsics, tests and demo tools
removed, built fixed-point (the P4's FPU is single precision, and SILK's float
path leans on doubles). Nothing in the sources is patched, so a newer libopus
drops in by repeating that trim and refreshing the file lists in its
`CMakeLists.txt`.

### Storage

```
nvs       24K   settings
phy_init   4K
factory     6M  application (~2.0 MB used)
storage     4M  FAT, mounted at /data: zello-channels.xml
coredump  256K  ESP-IDF core dump from the last crash
```

### Crash and disconnect reports

Every log line is also kept in a RAM ring buffer that survives a panic or
watchdog reset. After a crash, watchdog or brownout reset, the next boot writes
to the microSD card's `crash/` folder:

- `NNNN-crash.txt`: reset reason, firmware and ELF SHA256, panic reason,
  crashed task and backtrace/registers, and the log leading up to the reset
- `NNNN-core.elf`: the raw core dump, decoded against the matching build's
  `firmware.elf` with `esp-coredump info_corefile -c NNNN-core.elf firmware.elf`

An unexpected drop of a signed-in Zello session (or the server closing it) writes
`NNNN-disconnect.txt` with the socket/TLS error details and the recent log; at
most one every 30 s and 50 per boot. The web config's **Crash reports** section lists
the files on the card and downloads or deletes them, so the card can stay in. With no card in, the core dump stays in
flash until `crash` or the next report saves it.

## Licence

No licence has been chosen for the project code yet. The vendored dependencies
keep their own: `components/opus/` is libopus under its BSD licence
(`components/opus/COPYING`), and `third_party/esp_hosted/` is Espressif's
ESP-Hosted-MCU under Apache 2.0.
