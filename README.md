# Scheff for Zello

**Scheff for Zello** (`scheff-zello`) is a [Zello](https://zello.com) push-to-talk client for the **LilyGO T-Display-P4**
(ESP32-P4 + ESP32-C6), the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3) and the
**Seeed Wio Tracker L2** (ESP32-S3), built on ESP-IDF with an LVGL touch UI.

It signs in to the [Zello Channels API](https://github.com/zelloptt/zello-channel-api)
over a WebSocket, joins one channel, and carries voice both ways as Opus through
the board's own microphone and speaker — no extra hardware. Everything is configurable from the touch screen or from a browser.

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
- Push-to-talk is the **BOOT button**, or the big button on screen.

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
- Audio is the ES7210 microphone ADC and the MAX98357A speaker amplifier. The
  LoRa radio and GPS are left off.

### Seeed Wio Tracker L2

- ESP32-S3 with 16 MB flash and 8 MB PSRAM, using the S3's own Wi-Fi.
- 320x240 NV3031B LCD (QSPI) with GT911 touch. It has no keyboard, so text is
  typed on the on-screen keyboard; the UI is the T-Deck's compact layout.
- Push-to-talk: **hold the button on GPIO0**, or the **Talk button** on screen.
- Audio is the ES8311 DAC into the speaker amplifier and the ES7243E microphone
  ADC, sharing one I2S port. Which ES7243E channel carries the microphone is not
  confirmed yet: `MIC_SLOT` in `main/audio.c` averages both for now, and the
  boot log reports each channel's peak level.
- The battery is read through the ADS1115 ADC; the LoRa radio and GNSS stay off.

## Building

PlatformIO fetches and manages ESP-IDF itself; no separate install is needed.

```sh
./scripts/build-upload-monitor.sh            # T-Display-P4: build, flash, then monitor
./scripts/build-upload-monitor.sh --tdeck    # T-Deck: build, flash, then monitor
./scripts/build-upload-monitor.sh --wio-tracker-l2   # Wio Tracker L2
./scripts/build-upload-monitor.sh -B         # compile only, no device needed
./scripts/build-upload-monitor.sh --port /dev/cu.usbmodem1101
./scripts/build-upload-monitor.sh -E         # erase flash first (wipes settings)
./scripts/build-upload-monitor.sh --help     # every option
```

The T-Display-P4 is the default board; `--tdeck` selects the T-Deck and
`--wio-tracker-l2` the Wio Tracker L2. Each board
builds into its own directory. Add `--fullclean` (`-F`) the first time you build
a board, after switching boards, and after changing memory settings in the
`sdkconfig.defaults*` files: PlatformIO otherwise keeps a stale generated linker
script, and a P4 built that way boot-loops. The script says so when it applies.

Settings shared by both boards are in `sdkconfig.defaults`; ESP-IDF layers
`sdkconfig.defaults.esp32p4` or `sdkconfig.defaults.esp32s3` on top for the chip
being built. The two ESP32-S3 boards are told apart by the "Board" choice in
`main/Kconfig.projbuild`; the Wio build selects it in `sdkconfig.pio.wio.defaults`.

The script runs `pio` against a separate core directory (`~/.platformio-p4`), so
the pioarduino platform this project needs cannot be replaced by the
`espressif32` versions other projects resolve. It also builds outside the source
tree, because ESP-IDF refuses a build directory whose path contains a space.

Plain `idf.py build` against an ESP-IDF v5.5.4+ checkout works too.

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

- **Hold the big button** (or the BOOT button) to talk. It is green when the
  channel is yours, red with the talker's name while someone else holds it, and
  amber while the server is allocating the stream. Zello is half duplex, so a
  press is ignored while someone else is talking.
- Turn on **tap to talk** in the web config's Audio section if holding a button
  through a long call is awkward; the same press then toggles transmit.
- The level meter follows the microphone while transmitting and the speaker
  otherwise.
- Channel activity — who joined, who is talking, text messages, errors — scrolls
  in the panel above the volume slider, and in the browser under *Channel
  activity*, which can also send text messages.

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
| `web_config.c` | HTTP API, the setup hotspot's lifetime, backup export/import |
| `ui*.c` | LVGL screens: Wi-Fi, account, channels, home/talk |
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
```

## Licence

No licence has been chosen for the project code yet. The vendored dependencies
keep their own: `components/opus/` is libopus under its BSD licence
(`components/opus/COPYING`), and `third_party/esp_hosted/` is Espressif's
ESP-Hosted-MCU under Apache 2.0.
