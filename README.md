# claude-desktop-buddy — ESP32 AMOLED port

<img src="image.jpg" width="400" />

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions.

This is a port of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
(originally targeting M5StickC Plus) to four Waveshare ESP32 AMOLED
boards. The BLE wire protocol is unchanged — same pairing, same desktop
apps, just a larger screen.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

## Supported boards

All four run the **same main.cpp / UI** — board-specific wiring, drivers and
canvas→panel scaling are isolated in `src/hw/` + one header per board
under `src/boards/`.

| | [ESP32-S3-Touch-AMOLED-1.8](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8) | [ESP32-S3-Touch-AMOLED-1.75C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C) | [ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16) | [ESP32-S3-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16) |
| --- | --- | --- | --- | --- |
| MCU | ESP32-S3R8 (8 MB OPI PSRAM, 8 MB flash) | same | ESP32-C6FH8 (160 MHz RISC-V single-core, 8 MB flash, **no PSRAM**) | ESP32-S3R8 (8 MB OPI PSRAM, 8 MB flash) |
| Panel | 1.8" **rectangular** 368×448 AMOLED | 1.75" **round** 466×466 AMOLED | 2.16" **rounded-square** 480×480 AMOLED | 2.16" **rounded-square** 480×480 AMOLED (**rotated 90°**) |
| Display driver | SH8601 (QSPI) | CO5300 (QSPI) | SH8601 (QSPI) | CO5300 (QSPI) |
| Touch | FT3168 @ 0x38 | CST92xx @ 0x5A | CST9217 @ 0x5A | CST9217 @ 0x5A |
| GPIO expander | TCA9554 (LCD/TP resets routed through it) | none — resets are direct GPIOs | none — resets are direct GPIOs | none — resets are direct GPIOs |
| RTC | PCF85063 (I²C) | none — software clock synced from desktop | PCF85063 (I²C) | PCF85063 (I²C) |
| IMU | QMI8658 | same | same | same |
| PMU | AXP2101 | same | same | same |
| Audio | ES8311 + amp + speaker | same | ES8311 + ES7210 (output + mic codec) | same |
| Buttons | Key1 (GPIO0 BOOT) + AXP PEK | same (physical layout swapped; corrected in firmware) | three: PWR/IO10/BOOT; PWR is active-HIGH via MOSFET inverter + AXP PWRON | three: PWR/IO18/BOOT; PWR is active-HIGH via BSS138 inverter |
| Canvas → panel | 184×224 canvas → **2× nearest-neighbor** → 368×448 | 184×224 canvas → **1.5× bilinear** → 276×336 centred in 466×466 (black border) | 184×224 canvas → **2× nearest-neighbor** → 368×448 centred at (56, 16) in 480×480 (56 px L/R / 16 px T/B black border) | 184×224 canvas → **2× nearest-neighbor** → 368×448 centred at (56, 16) in 480×480 (56 px L/R / 16 px T/B black border) |

Internal canvas is **184×224** on all four. The 1.75C rounds the content
inside its circular bezel; keeping the logical canvas identical means
UI code, fonts and all buddy rendering are completely board-agnostic.

The firmware targets ESP32-S3 and ESP32-C6 with Arduino framework 3.x via the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then pick the env that matches your board:

```bash
# 1.8" rectangular AMOLED (ESP32-S3)
pio run -e waveshare-esp32s3-touch-amoled-1-8 -t upload

# 1.75C round AMOLED (ESP32-S3)
pio run -e waveshare-esp32s3-touch-amoled-1-75c -t upload

# 2.16" rounded-square AMOLED (ESP32-C6)
pio run -e waveshare-esp32c6-touch-amoled-2-16 -t upload

# 2.16" rounded-square AMOLED (ESP32-S3)
pio run -e waveshare-esp32s3-touch-amoled-2-16 -t upload
```

If you're starting from a previously-flashed device (e.g. the factory
Xiaozhi firmware), wipe it first:

```bash
pio run -e <env> -t erase && pio run -e <env> -t upload
```

LittleFS auto-formats on first boot if the partition isn't recognised.

### Adding another board

1. Add a new header at `src/boards/board_<name>.h` declaring all
   `PIN_*`, `BOARD_HW_W/H`, `BOARD_SAFE_INSET`, and capability flags —
   the existing headers cover ~16 flags between them
   (`BOARD_HAS_PSRAM`, `BOARD_HAS_TCA9554`, `BOARD_HAS_PCF85063`,
   `BOARD_HAS_AXP2101`, `BOARD_HAS_PA_CTRL`, `BOARD_HAS_KEY2`,
   `BOARD_DISPLAY_CO5300`, `BOARD_DISPLAY_LETTERBOX`,
   `BOARD_DISPLAY_OFFSET_X/Y`, `BOARD_DISPLAY_SCALE`,
   `BOARD_DISPLAY_PUSH_STREAMED`, `BOARD_DISPLAY_SH8601_VENDOR_INIT`,
   `BOARD_CO5300_COL_OFFSET`, `BOARD_CO5300_MADCTL`,
   `BOARD_LCD_RST_VIA_PMU`, `BOARD_AXP_PWRON_4S_OFF`,
   `BOARD_AXP_ENABLE_AUX_LDOS`, `BOARD_KEY1_ACTIVE_HIGH`,
   `BOARD_BTN_THIRD`, `BOARD_BTN_SWAP_AB`, `BOARD_TOUCH_CST92XX`).
   Pick the values that match your board.
2. Add a `#elif defined(BOARD_<NAME>)` branch in `src/hw/pins.h`.
3. Add a matching `[env:<name>]` block in `platformio.ini` with the
   `-DBOARD_<NAME>` build flag.

`main.cpp` and `buddies/` stay untouched.

Once running you can also wipe everything from the device itself:
**hold the A button (Key1 on 1.8/1.75C, PWR on the 2.16 boards) →
settings → reset → factory reset → tap twice**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list (advertised as `Claude-XXXX`). macOS will prompt
for Bluetooth permission on first connect; grant it.

The device shows a 6-digit passkey on screen — type it on the desktop to
complete LE Secure Connections bonding. Once paired, the bridge
auto-reconnects whenever both sides are awake.

## Controls

### ESP32-S3 boards (1.8 & 1.75C)

The board has two physical keys. **Key1** is the BOOT button (acts as
"A" in the table). **Key3** is the AXP power key — short-press is "B",
long-press toggles screen off, very-long-press hardware-shuts-down.

|                          | Normal               | Pet         | Info        | Approval    |
| ------------------------ | -------------------- | ----------- | ----------- | ----------- |
| **Key1** (BOOT)          | next screen          | next screen | next screen | **approve** |
| **Key3** (PWR, short)    | scroll transcript    | next page   | next page   | **deny**    |
| **Hold Key1**            | menu                 | menu        | menu        | menu        |
| **Key3** (PWR, ~1s long) | toggle screen off    |             |             |             |
| **Key3** (PWR, ~6s)      | hard power off       |             |             |             |
| **Shake**                | dizzy                |             |             | —           |
| **Face-down**            | nap (energy refills) |             |             |             |

### ESP32-C6-Touch-AMOLED-2.16 controls

The board has three physical keys:
- **PWR** (middle) — primary action / confirm (= A button)
- **IO10** (left) — secondary / back / scroll (= B button)
- **BOOT** (right) — open menu shortcut

|                          | Normal               | Pet         | Info        | Approval    |
| ------------------------ | -------------------- | ----------- | ----------- | ----------- |
| **PWR** (middle)         | next screen          | next screen | next screen | **approve** |
| **IO10** (left, short)   | scroll transcript    | next page   | next page   | **deny**    |
| **Hold PWR**             | menu                 | menu        | menu        | menu        |
| **BOOT** (right)         | open menu (shortcut) | open menu   | open menu   | open menu   |
| **PWR held 4 s**         | power off (AXP cuts ALDO3; press again to wake) |             |             |             |
| **Shake**                | dizzy                |             |             | —           |
| **Face-down**            | nap (energy refills) |             |             |             |

### ESP32-S3-Touch-AMOLED-2.16 controls

The board has three physical keys:
- **PWR** (middle) — primary action / confirm (= A button)
- **IO18** (left) — secondary / back / scroll (= B button)
- **BOOT** (right) — open menu shortcut

|                          | Normal               | Pet         | Info        | Approval    |
| ------------------------ | -------------------- | ----------- | ----------- | ----------- |
| **PWR** (middle)         | next screen          | next screen | next screen | **approve** |
| **IO18** (left, short)   | scroll transcript    | next page   | next page   | **deny**    |
| **Hold PWR**             | menu                 | menu        | menu        | menu        |
| **BOOT** (right)         | open menu (shortcut) | open menu   | open menu   | open menu   |
| **PWR held 4 s**         | power off (AXP cuts ALDO3; press again to wake) |             |             |             |
| **Shake**                | dizzy                |             |             | —           |
| **Face-down**            | nap (energy refills) |             |             |             |

### Touch (all boards)

Touch does exactly one thing: a short stationary tap (not a swipe) → heart
reaction. Buttons are the only path for approve/deny, menu navigation, page
cycling, and transcript scrolling — keeping touch position-independent means
it needs no coordinate remap when the **rotation** setting changes (see
[Settings menu](#settings-menu)).

### Sleep & wake

- **USB plugged** — never auto-offs; the clock face stays visible
- **Battery + clock visible** — auto-off after **5 minutes**
- **Battery + other screens** — auto-off after **30 seconds**
- **Battery + Claude busy** — dims instead of sleeping at the same timeout,
  so the last frame stays visible instead of going blank; wakes back to full
  brightness the moment a session needs attention, finishes, or celebrates
- **Approval prompt up** — never auto-offs

Any key press or screen tap wakes the panel.

## Settings menu

Hold the A button (Key1 on 1.8/1.75C, PWR on the 2.16 boards) to open
**menu → settings**. Short-press A steps through items, B changes the
selected one.

| Item | Values | Notes |
| --- | --- | --- |
| brightness | 0–4 | screen brightness level |
| volume | 0/20/40/60/80/100 | ES8311 codec hardware volume — not a digital scale, so low settings don't lose bit depth |
| sound | on/off | mutes all chirps |
| bluetooth | on/off | stored preference only — the BLE radio stays live either way |
| wifi | on/off | placeholder — no Wi-Fi stack linked yet |
| led | on/off | gates the on-screen attention pulse (these boards have no physical LED) |
| transcript | on/off | HUD/transcript overlay |
| clock rot | auto/portrait/landscape | on-screen clock face orientation (unrelated to panel `rotation`, below) |
| ascii pet | cycles species | includes GIF mode if a character pack is installed |
| uptime | on/off | shows/hides the session + last-session rows on the DEVICE info page |
| rotation | 0/90/180/270 | physical panel rotation, applied live via MADCTL — **CO5300 boards only** (1.75C, S3 2.16"); no-op on the SH8601-based 1.8" and C6 2.16" |
| reset | — | opens the reset submenu (delete char / factory reset) |
| back | — | closes settings |

## Audio cues

Every chirp is a synthesized sine tone (`hwBeep(freqHz, durMs)`) with a 4ms
fade in/out, queued so back-to-back tones play in sequence instead of
clipping each other.

| Event | Tone(s) |
| --- | --- |
| Approve | 2400 Hz / 60 ms |
| Deny | 600 Hz / 60 ms |
| Menu / navigation | 1800 Hz / 30 ms |
| Needs attention (prompt arrives) | 1200 Hz / 80 ms |
| Busy started | 1000 Hz / 40 ms |
| Session finished | 400 Hz / 95 ms → 480 Hz / 35 ms |
| Celebrate (level up) | 2000 Hz / 100 ms |

"Session finished" fires whenever the running-session count *decreases*,
even with other sessions still active — not only when the last one ends.

## Notable differences from the M5StickC original

- **Display layer** — Arduino_GFX + PSRAM Canvas (was M5.Lcd / TFT_eSprite)
- **Attention indicator** — small red pill at top of screen
  (M5 used a GPIO red LED; the AMOLED board has none)
- **Landscape clock removed** — 368×448 is near-square; rotation pointless
- **Battery current not exposed** — XPowersLib / AXP2101 only reports
  voltage, %, and isCharging, never actual current draw. The DEVICE info
  page used to show a `current +0mA` row for this; it's now `last`
  (previous session's duration) instead — see **uptime** in the settings menu
- **Transcript supports CJK** — uses `chill7_h_cjk` font for the HUD lines
  so Chinese / Japanese log entries render legibly
- **Other UI strings stay ASCII** — non-ASCII bytes in `msg`, `promptTool`
  and `promptHint` are replaced with random Matrix-rain symbols rather
  than rendering as garbage glyphs
- **ESP32-S3 2.16" rotation** — the Waveshare ESP32-S3-Touch-AMOLED-2.16 panel is physically mounted rotated from its natural orientation. This used to be a hardcoded `BOARD_CO5300_MADCTL` value in the board header; it's now the **rotation** setting in the settings menu, applied live via the same MADCTL write — switchable without reflashing, and defaults to whatever the board header used to hardcode so existing devices don't flip on first boot of new firmware

## Per-state animations

| State       | Trigger                     | Feel                                      |
| ----------- | --------------------------- | ----------------------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing               |
| `idle`      | connected, nothing urgent   | blinking, looking around                  |
| `busy`      | sessions actively running   | sweating, working                         |
| `attention` | approval pending            | alert, **red top-bar pulses**             |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing                        |
| `dizzy`     | you shook the device        | spiral eyes, wobbling                     |
| `heart`     | approved in under 5s        | floating hearts                           |

Eighteen ASCII species, each with all seven animations. **Settings →
ascii pet** cycles them; choice persists in NVS.

## Custom GIF characters

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window.
The app streams it over BLE and the device switches to GIF mode live.
**Settings → reset → delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96 px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate
loop-by-loop, useful for an idle activity carousel.

GIFs are 96 px wide; up to ~140 px tall keeps the character above the HUD.
The whole folder must fit under 1.8 MB; `gifsicle --lossy=80 -O3 --colors 64`
typically cuts 40–60 %.

See `characters/bufo/` for a working example. If you're iterating on a
character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## Project layout

```
src/
  main.cpp           — loop, state machine, UI screens (board-agnostic)
  buddy.{cpp,h}      — ASCII species dispatch + render helpers
  buddies/           — one file per species, seven anim functions each
  character.{cpp,h}  — GIF decode + render
  ble_bridge.{cpp,h} — Nordic UART service, line-buffered TX/RX
  data.h             — wire protocol, JSON parse, CJK matrixifier
  xfer.h             — folder push receiver
  stats.h            — NVS-backed stats, settings, owner, species choice
  boards/            — one .h per supported board (pins + capability flags)
  hw/                — board HAL (display, input, power, imu, rtc,
                       audio, expander, border). pins.h dispatches on
                       the BOARD_* build flag
lib/
  ES8311/            — vendored Espressif codec driver
  Arduino_DriveBus/  — vendored FT3168 touch driver (1.8)
  Adafruit_XCA9554/  — vendored TCA9554 expander driver (1.8)
characters/          — example GIF character packs
tools/               — generators and converters
docs/superpowers/    — design specs + implementation plans
```

CST92xx touch (1.75C, both 2.16 boards) and PCF85063 RTC (1.8, both
2.16 boards) come in through `SensorLib` via `platformio.ini` lib_deps
rather than being vendored.

## Availability

The BLE API is only available when the Claude desktop apps are in
developer mode (**Help → Troubleshooting → Enable Developer Mode**).
It's intended for makers and developers and isn't an officially
supported product feature.
