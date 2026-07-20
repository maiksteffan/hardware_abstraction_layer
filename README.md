# Sequenzboard — ESP32-S3 Hardware Firmware

The **Sequenzboard** is an interactive climbing training board: a wall with
**34 touch-sensitive, LED-illuminated holds** (`H01`–`H34`), driven by a
Raspberry Pi 5 kiosk app and this ESP32-S3 firmware.

> **Detailed context for contributors & AI agents:** see [claude.md](claude.md)
> — full protocol reference, firmware architecture, game logic, and coding
> standards. It is kept up to date with every firmware change.

## The two halves

| | Raspberry Pi 5 (kiosk app) | ESP32-S3 (this firmware) |
|---|---|---|
| Role | **All game logic** | **Hardware executor only** |
| Does | Sequences, turns, timeouts, scoring, UI | Drives LEDs, reads touch sensors |
| Stack | FastAPI backend + Svelte frontend (`RaspberryPI_Kiosk_App/sequenzboard/`) | Arduino / FreeRTOS (PlatformIO) |
| Hardware | Touchscreen kiosk | 2× WS2812 strips (260 LEDs), 5× CAP1188 touch chips (I²C) |

Both talk over USB serial (115200 baud, ASCII lines with `#id` correlation):

```
Raspberry Pi (game logic)          ESP32-S3 (this firmware)
     │  SHOW H05 #12   ──────────►  light hold H05 (purple)
     │  EXPECT H05 #13 ──────────►  arm touch sensor
     │                 ◄──────────  TOUCHED H05 #13   (player grabs hold)
     │  SUCCESS H05 #14──────────►  green pulse animation
     │                 ◄──────────  ACK / DONE
```

## The game (runs on the Pi)

- **Kofferpacken** (memory game): 2–4 players; each replays the growing hold
  sequence and adds 2 new moves. Wrong/missed holds or letting go of the wall
  (hands-off detection) costs a life.
- **Creative mode:** record and replay custom sequences.
- **Workouts:** pre-authored training sequences with difficulty levels and
  highscores.

Sequences are strings like `"H01,H02,H03+H04"` — `+` means a two-hold
(double) step. The Pi turns them into `SHOW`/`EXPECT`/`SUCCESS` commands;
the firmware reports `TOUCHED`/`HANDS_OFF` events back.

## The firmware (this repo)

- **Dual-core FreeRTOS:** Core 0 polls the CAP1188 touch sensors over I²C
  every 5 ms; Core 1 handles serial commands and LED animations; a
  mutex-protected `EventQueue` connects them.
- **Touch intelligence:** asymmetric debouncing, one-shot expectations,
  single-consumption press edges (a held hold can't satisfy two EXPECTs),
  brush-by rejection for recording mode, hands-off/presence events.
- **LED engine:** hold→strip mapping with mirrors, solid states and
  non-blocking animations (success pulse, victory/defeat, recording glow).
- **Boot handshake:** LED sweep → sensor init → `SENSORS READY` →
  `HARDWARE INITIALISED`, with `DIAG` output for troubleshooting.

| Module | Role |
|---|---|
| `CommandController` | Serial command parsing & execution |
| `EventQueue` | Thread-safe outgoing message queue |
| `LedController` | Position→LED mapping and all animations |
| `TouchController` | CAP1188 driver, debounce, touch expectations |
| `StartupController` | Boot sequence & Pi handshake |
| `Config.h` | All pins, timing, colors, and the hold↔sensor wiring table |

## Build & flash

PlatformIO project; default env `esp32-s3-devkitc-1` (legacy `esp32dev` for
the old ESP32 WROOM board).

```bash
pio run                # build
pio run -t upload      # flash over USB
pio device monitor     # serial monitor (115200 baud)
```

## Updates in the field

The Pi's admin panel flashes new firmware automatically: it downloads the
`firmware-merged.bin` asset from this repo's latest GitHub release, writes it
with `esptool`, and verifies the version via the `INFO` command. Bump
`FIRMWARE_VERSION` in `include/Config.h` to match the release tag.
