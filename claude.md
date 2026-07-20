# Hardware Abstraction Layer — Project Overview

> **Purpose of this document:** Current-state description of the ESP32 firmware. The former migration plan (5× CAP1188 multi-channel, 34 inputs `H01`–`H34`) has been **fully executed** — this document describes the system as it exists today.

## What this project is

Firmware for an **ESP32** that acts as a **hardware executor** between a Raspberry Pi 5 (game logic) and physical hardware (LED strips + CAP1188 capacitive touch sensors) on a climbing board. The ESP32 has **no game logic** — it executes commands received over serial and reports hardware events back to the Pi.

- Firmware version: **1.0.5**, protocol version: **3** (`H01`–`H34` position tokens)

## Build Targets ([platformio.ini](platformio.ini))

| Env | Board | Serial | I2C | LED pins |
|-----|-------|--------|-----|----------|
| `esp32-s3-devkitc-1` (**default**) | ESP32-S3-DevKitC-1-N8R8 (8 MB flash, 8 MB PSRAM) | Native USB-CDC | SDA=GPIO7, SCL=GPIO6 | STRIP1=GPIO17, STRIP2=GPIO18 |
| `esp32dev` (legacy) | Freenove ESP32 WROOM | UART0 | SDA=GPIO21, SCL=GPIO22 | STRIP1=GPIO18, STRIP2=GPIO19 |

Build: `pio run` — Upload: `pio run -t upload` — Alternate board: `pio run -e esp32dev -t upload`. Pin defaults live in [include/Config.h](include/Config.h) and are overridden per-board via `build_flags`.

## Architecture

### Dual-Core FreeRTOS Split

| Core | Responsibility |
|------|---------------|
| **Core 0** | Touch sensor polling task (I2C, every 5 ms via `vTaskDelayUntil`) |
| **Core 1** | Main loop: serial polling, command processing, LED animation ticking, event flushing |

LED animations are kept on Core 1 to avoid race conditions with the NeoPixel buffer.

### Key Classes

| Class | File | Role |
|-------|------|------|
| `CommandController` | [src/CommandController.cpp](src/CommandController.cpp) | Parses and executes serial commands from the Pi; owns position helpers `parsePosition()` / `indexToPosition()` |
| `EventQueue` | [src/EventQueue.cpp](src/EventQueue.cpp) | Thread-safe queue (64 slots) that buffers outgoing serial messages |
| `LedController` | [src/LedController.cpp](src/LedController.cpp) | Controls two NeoPixel LED strips (34 logical positions + mirrors) |
| `TouchController` | [src/TouchController.cpp](src/TouchController.cpp) | Manages 5 CAP1188 chips over I2C, exposes 34 logical inputs |
| `StartupController` | [src/StartupController.cpp](src/StartupController.cpp) | Hardware init, boot diagnostics (`DIAG`), and Pi handshake |

## Main Loop (`loop()`)

```
1. commandController.pollSerial()             — Read serial bytes into ring buffer
2. commandController.processCompletedLines()  — Parse & execute complete lines
3. commandController.tick()                   — Advance long-running command animations
4. ledController.tick()                       — Step LED animations
5. eventQueue.flush(EVENTS_PER_FLUSH)         — Drain the event queue (EVENTS_PER_FLUSH
                                                 = full queue capacity, 64)
```

## Startup Sequence

1. **LED init** — strips initialized + white pixel sweep animation.
2. **Sensor init** — up to 3 retries to initialize all 5 CAP1188 chips over I2C.
3. **Diagnostics** — emits `DIAG I2C_SCAN count=<n> devices=[0x28,...]` plus one `DIAG S<i>@0x<addr> OK|FAIL <reason>` line per sensor (the Pi ignores unknown keywords).
4. **Handshake loop** — broadcasts `SENSORS READY` (all 34 inputs covered) or `SENSORS FAILED [<active input list>]` every **500 ms** until the Pi replies with a matching `ACK ...`. Note: the bracketed list contains the **active** inputs, not the failed ones.
5. Sends `HARDWARE INITIALISED` to begin normal operation.

## Serial Protocol (v3)

### Transport
- Baud rate: **115200**, ASCII, line-based (`\n` / `\r` terminated), max 64 chars/line.
- Positions are three-character tokens **`H01`–`H34`** (case-insensitive on input, canonical upper-case on output).

### Pi → ESP32 Commands

Grammar: `<ACTION> [<position>] [<extra>] [#<id>]`

| Category | Commands |
|----------|----------|
| **LED** | `SHOW`, `HIDE`, `HIDE_ALL`, `SUCCESS`, `FAIL`, `CONTRACT`, `BLINK`, `STOP_BLINK`, `EXPAND_STEP`, `CONTRACT_STEP`, `MENUE_CHANGE <r,g,b> <range>`, `SEQUENCE_COMPLETED`, `DEFEAT_ANIMATION`, `INDICATE_RECORDING` |
| **Touch** | `EXPECT`, `EXPECT_ANY`, `EXPECT_ANY_EXCEPT [<pos>...]`, `EXPECT_RELEASE`, `RECALIBRATE`, `RECALIBRATE_ALL`, `VALUE`, `SET_SENSITIVITY <pos> <0-7>`, `CLEAN_QUEUE`, `HANDSOFF_DETECTION_ON`, `HANDSOFF_DETECTION_OFF` |
| **Utility** | `PING`, `INFO`, `SCAN` |

**Long-running** commands (`SUCCESS`, `CONTRACT`, `MENUE_CHANGE`, `SEQUENCE_COMPLETED`, `DEFEAT_ANIMATION`) get an immediate `ACK`, run in the background, and emit `DONE` when finished. If the command queue (32 slots) is full, the ESP32 sends `BUSY`.

### ESP32 → Pi Responses & Events

| Message | Format |
|---------|--------|
| `ACK` | `ACK <ACTION> [<pos>] [#id]` |
| `DONE` | `DONE <ACTION> [<pos>] [#id]` |
| `ERR` | `ERR <reason> [#id]` |
| `BUSY` | `BUSY [#id]` |
| `TOUCHED` | `TOUCHED <pos> [#id]` |
| `TOUCH_RELEASED` | `TOUCH_RELEASED <pos> [#id]` |
| `SCANNED` | `SCANNED [<pos>,<pos>,...]` |
| `RECALIBRATED` | `RECALIBRATED <pos\|ALL> [#id]` |
| `VALUE` | `VALUE <pos> <delta> [#id]` |
| `INFO` | `INFO firmware=X protocol=Y board=Z` |
| `HANDS_ON` | `HANDS_ON [#id]` — board occupancy rose above 0 (hands-off detection enabled) |
| `HANDS_OFF` | `HANDS_OFF [#id]` — board occupancy dropped to 0 |
| `DIAG` | `DIAG ...` — startup diagnostics only, ignored by the Pi |

## Touch Behavior

- **Polling:** every 5 ms on Core 0; one status-register read per chip per cycle (cached, not per-input).
- **Debounce (asymmetric):** press is **instant** (`TOUCH_DEBOUNCE_PRESS_MS = 0`, fires on first sample); release is latched for **800 ms** of continuous untouched samples (`TOUCH_DEBOUNCE_RELEASE_MS`) — transient re-contacts reset the timer.
- **EXPECT `<pos>`** is instant. A hold that is already touched satisfies a newly armed `EXPECT` only if pressed less than `EXPECT_HELD_FRESH_MS` (2 s) ago; staler presses require release + re-grab. Each press can satisfy only one expectation (`pressConsumed`).
- **EXPECT_ANY qualification (brush-by rejection):** a new touch is reported only after passing a ~150 ms confirmation window (`EXPECT_ANY_CONFIRM_MS`) checking persistence (≤3 consecutive dropout samples), delta strength (delta ≥ 48 in ≥ 70 % of samples), and sweep plausibility (≥3 press edges within 500 ms ⇒ arm-sweep ⇒ decisions deferred 400 ms). Up to 8 concurrent `EXPECT_ANY` commands are queued (`EXPECT_ANY_QUEUE_SIZE`).
- **EXPECT_ANY_EXCEPT** works like `EXPECT_ANY` but excludes the listed positions (bitmask).
- **CLEAN_QUEUE** clears every pending EXPECT / EXPECT_RELEASE / EXPECT_ANY.
- **Hands-off detection:** `HANDSOFF_DETECTION_ON` immediately reports the current state, then emits `HANDS_OFF` / `HANDS_ON` on occupancy transitions.
- **SET_SENSITIVITY** is **global per chip** (CAP1188 limitation) — changing one input affects every input on the same physical sensor. Default sensitivity at init: 3.
- **RECALIBRATE `<pos>`** recalibrates only that input's channel; `RECALIBRATE_ALL` writes each chip's full enable mask.

## Current Hardware

| Component | Setup |
|-----------|-------|
| LED strips | 2× NeoPixel, **260 LEDs each** |
| Logical LED positions | **34** (`H01`–`H34`), each lighting a block of **5 LEDs** (`LED_POSITION_WIDTH`) |
| LED mirrors | `H04` and `H19` drive a second LED on the other strip (`LED_MIRRORS` in [src/LedController.cpp](src/LedController.cpp)) |
| Touch sensors | **5× CAP1188** at I2C addresses `0x28`–`0x2C`, **7 channels each** (CS1–CS7) |
| Logical inputs | **34** (`H01`–`H34`), mapped to `(sensor, channel)` via `INPUT_MAPPINGS[]` in [include/Config.h](include/Config.h) — custom (non-sequential) wiring; one of the 35 raw channels is unused |
| I2C bus | **100 kHz** |

## Key Configuration ([include/Config.h](include/Config.h))

All tunables live in `Config.h`, grouped in numbered sections:

| Constant | Value | Meaning |
|----------|-------|---------|
| `TOUCH_SENSOR_COUNT` | 5 | Physical CAP1188 chips |
| `TOUCH_CHANNELS_PER_SENSOR` | 7 | CS1–CS7 usable per chip |
| `INPUT_COUNT` | 34 | Logical inputs `H01`–`H34` |
| `LED_POSITION_COUNT` | 34 | Logical LED positions |
| `LED_POSITION_WIDTH` | 5 | Physical LEDs lit per position (odd: center ± 2) |
| `INPUT_MAPPINGS[34]` | — | User-editable `H##` → `(sensorIndex, channel)` table |
| `QUEUE_SIZE_COMMANDS` / `QUEUE_SIZE_EVENTS` | 32 / 64 | Command & event queue capacities |
| `EVENTS_PER_FLUSH` | 64 | Full-queue drain per main-loop tick |
| `TOUCH_DEBOUNCE_PRESS_MS` / `RELEASE_MS` | 0 / 800 | Asymmetric debounce |
| `EXPECT_ANY_CONFIRM_MS` | 150 | Brush-by qualification window |
| `EXPECT_HELD_FRESH_MS` | 2000 | Already-held freshness window for `EXPECT` |
| `STARTUP_HANDSHAKE_INTERVAL_MS` | 500 | Status re-broadcast interval |
| `SENSOR_LIST_BUFFER_SIZE` | 160 | `SCANNED` list buffer |
| `EVENT_MESSAGE_BUFFER_SIZE` | 224 | Max chars per outgoing event line |

To change the physical wiring, edit `INPUT_MAPPINGS[]` (touch) and `LED_MAPPINGS[]` / `LED_MIRRORS[]` in [src/LedController.cpp](src/LedController.cpp) (LEDs) — no other code changes needed.
