# Sequenzboard — Full System Context (ESP32 Firmware + Pi Game Logic)

> **Living document.** This file describes the CURRENT state of the whole
> Sequenzboard system so that an AI agent working on the ESP32 firmware has
> complete context: the firmware itself, the serial protocol, and the game
> logic on the Raspberry Pi that drives it.
>
> ⚠ **AUTO-UPDATE RULE (mandatory):** after implementing ANY change to the
> firmware — new commands, changed behavior, new config constants, timing
> changes, version bumps, bug fixes with protocol impact — update the relevant
> sections of this file **in the same commit**. This file must never lag
> behind the code.

---

# 1. System overview

The **Sequenzboard** is an interactive climbing training board:

- A climbing wall with **34 holds** (`H01`–`H34`), each touch-sensitive
  (capacitive) and illuminated by addressable LEDs.
- A **Raspberry Pi 5** runs the kiosk app (touchscreen UI + game logic).
- An **ESP32-S3** (this repository) is the **hardware executor**: it drives
  the LEDs and reads the touch sensors, connected to the Pi via USB serial.

```
┌────────────────────────────┐  USB serial, 115200 baud   ┌──────────────────────────┐
│ Raspberry Pi 5             │  ASCII lines + #id         │ ESP32-S3-DevKitC-1       │
│ • Svelte kiosk frontend    │ ◄────────────────────────► │ • 2× WS2812 strips (260) │
│ • FastAPI backend          │  SHOW/EXPECT/SUCCESS...    │ • 5× CAP1188 (I²C)       │
│ • ALL game logic           │  TOUCHED/ACK/DONE...       │ • NO game logic          │
└────────────────────────────┘                            └──────────────────────────┘
```

**Division of responsibility (never violate this):** the Pi decides *what*
happens (sequences, turns, timeouts, scoring); the ESP32 executes *how*
(light this hold, arm that sensor, report touches). Never add game state,
sequence logic, or gameplay timing decisions to the firmware.

This repo (`maiksteffan/hardware_abstraction_layer`) contains the firmware.
The Pi app lives in `RaspberryPI_Kiosk_App/sequenzboard/` (checkout of the
separate repo `FinnStf/sequenzboard`) — read it for context, do not modify it
from here.

Current versions ([include/Config.h](include/Config.h)):

| Constant | Value |
|---|---|
| `FIRMWARE_VERSION` | `1.0.5` |
| `PROTOCOL_VERSION` | `3` (H01..H34 3-char position tokens) |
| `BOARD_TYPE` | `ESP32_S3_DEVKITC_1` (overridable per build env) |

---

# 2. The ESP32 firmware (this repository)

## 2.1 Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3-DevKitC-1-N8R8 (8 MB flash, 8 MB PSRAM), serial over native USB-CDC |
| LED strips | 2× WS2812/NeoPixel, 260 LEDs each, data on GPIO 18 / GPIO 17 |
| Touch | 5× CAP1188 (I²C addresses 0x28–0x2C), SDA=GPIO7, SCL=GPIO6, 7 channels used per chip |
| Legacy board | classic ESP32 WROOM still buildable via `pio run -e esp32dev` (pins overridden by build flags) |

The wiring table `INPUT_MAPPINGS[34] = {sensorIndex, channel}` in
[include/Config.h](include/Config.h) maps each logical hold `H01`–`H34` to a
physical chip+channel. `LED_MAPPINGS[]` / `LED_MIRRORS[]` in
[src/LedController.cpp](src/LedController.cpp) map each hold to strip
positions (a mirror = second LED block for the same hold on the other strip).

## 2.2 Dual-core FreeRTOS architecture

| Core | Runs | Owns |
|---|---|---|
| **Core 0** | `touchPollingTask` → `TouchController::tick()` every 5 ms (`vTaskDelayUntil`) | ALL I²C traffic |
| **Core 1** | Arduino `loop()` | Serial RX/TX, command execution, ALL NeoPixel writes |

The **only cross-core channel is the `EventQueue`** (FreeRTOS-mutex-protected,
64 slots). Touch code on Core 0 must never call `LedController` or write to
`Serial` directly.

```
loop():                                        // Core 1
  commandController.pollSerial()               // UART bytes -> ring buffer
  commandController.processCompletedLines()    // parse + execute complete lines
  commandController.tick()                     // advance async commands -> DONE
  ledController.tick()                         // step LED animations
  eventQueue.flush(EVENTS_PER_FLUSH)           // drain outgoing events to Serial
```

## 2.3 Modules

| Class | Files | Role |
|---|---|---|
| `CommandController` | [include/CommandController.h](include/CommandController.h), [src/CommandController.cpp](src/CommandController.cpp) | Parses serial lines, executes commands, 32-slot queue for async commands. Owns canonical `parsePosition()`/`indexToPosition()` |
| `EventQueue` | [include/EventQueue.h](include/EventQueue.h), [src/EventQueue.cpp](src/EventQueue.cpp) | Thread-safe ring buffer for all outgoing messages; only writer to `Serial` after startup |
| `LedController` | [include/LedController.h](include/LedController.h), [src/LedController.cpp](src/LedController.cpp) | Hold→LED mapping, solid colors, all animations (non-blocking, stepped from `tick()`) |
| `TouchController` | [include/TouchController.h](include/TouchController.h), [src/TouchController.cpp](src/TouchController.cpp) | CAP1188 driver, debounce, expectation matching, EXPECT_ANY qualification, hands-off detection |
| `StartupController` | [include/StartupController.h](include/StartupController.h), [src/StartupController.cpp](src/StartupController.cpp) | Boot: LED sweep, sensor init + retries, DIAG lines, Pi handshake |
| `main.cpp` | [src/main.cpp](src/main.cpp) | Globals, `setup()`, task creation, main loop |

## 2.4 Startup sequence (blocks inside `setup()`)

1. **LED init** + white pixel sweep (visual boot confirmation).
2. **Sensor init**, up to `SENSOR_INIT_MAX_RETRIES` (3) attempts.
3. **Diagnostics on the wire** (Pi logs, ignores unknown keywords):
   `DIAG I2C_SCAN count=... [0x28,...]`, then one
   `DIAG S<n>@0x<addr> OK/FAIL ...` line per sensor.
4. **Handshake loop:** broadcasts `SENSORS READY` or
   `SENSORS FAILED [H01,...]` every 500 ms until the Pi replies
   `ACK SENSORS READY` / `ACK SENSORS FAILED`.
   ⚠ The list inside `SENSORS FAILED [...]` contains the **active** inputs;
   the Pi computes the failed set as the complement.
5. Sends an unsolicited `INFO firmware=... protocol=... board=...` line
   (boot metadata; the Pi also still queries `INFO` after connecting), then
   `HARDWARE INITIALISED` → touch task is created, normal operation.

## 2.5 Touch pipeline (the tricky part — read before touching TouchController)

Runs on Core 0 every 5 ms:

1. Read each active CAP1188's status register **once** per cycle (cached in
   `PhysicalSensor::lastStatus`) — never per-input reads.
2. Update the 34 logical inputs via `INPUT_MAPPINGS`.
3. Clear the INT bit on chips that reported a touch (keeps latching alive).
4. Debounce → expectation matching → EXPECT_ANY qualification → hands-off.

**Debounce (asymmetric):** press is instant (`TOUCH_DEBOUNCE_PRESS_MS` = 0);
release is latched `TOUCH_DEBOUNCE_RELEASE_MS` = 800 ms — transient
re-contacts keep the touch held.

**Single-consumption rule (`pressConsumed`, critical fix in v1.0.4):** each
debounced press edge may satisfy **exactly one** `EXPECT`/`EXPECT_ANY`. Set
when a `TOUCHED` is reported, cleared only on genuine release + re-grab.
Without it, a continuously held hold satisfied every re-armed `EXPECT` within
the freshness window and alternating two-hold sequences ping-ponged instantly
(false double touches). **Preserve this invariant in any refactor.**

**Already-held EXPECT:** `setExpectDown()` fires `TOUCHED` immediately only if
`debouncedTouched && !pressConsumed && (now - pressStartTime) <= EXPECT_HELD_FRESH_MS`
(2 s). Covers the race where the player grabs the hold just before the Pi's
`EXPECT` arrives.

**EXPECT_ANY qualification (brush-by rejection):** `EXPECT <pos>` is instant
(only the armed hold can fire), but `EXPECT_ANY` arms every hold, so a new
press must survive a 150 ms confirmation window (`AnyCandidate`):
persistence (≥3 consecutive untouched samples cancels), delta consistency
(≥70 % of samples with CAP1188 delta ≥ 48; degrades gracefully on I²C
failures), and sweep plausibility (≥3 press edges in 500 ms = arm sweeping
across the wall → all decisions deferred 400 ms). Qualified candidates whose
queue is momentarily empty stay pending while fresh so the second of two
simultaneous grabs is still reported when the next `EXPECT_ANY` arrives.

**Hands-off detection:** while enabled, emits `HANDS_ON`/`HANDS_OFF` on
board-occupancy transitions; enabling reports the current state immediately.
The 800 ms release latch means `HANDS_OFF` lags the last release by ~0.8 s.
Disabled by `HANDSOFF_DETECTION_OFF` and by `CLEAN_QUEUE`.

## 2.6 LED system

- Each hold lights a block of `LED_POSITION_WIDTH` = 5 LEDs (center ± 2);
  mirrored positions light on both strips.
- Colors (Config.h §8): **SHOW = dark purple (80,0,205)**, SUCCESS/BLINK = green,
  FAIL = red, RECORD glow = dim white (30,30,30).
- Animations (all non-blocking, stepped in `tick()` on Core 1): success
  expansion, contract, blink, sequence-completed pulses, defeat pulses,
  menu-change sweep. Async commands report `DONE` via
  `CommandController::tickCommand()` polling `is...Complete()`.

## 2.7 Build, flash, debug

PlatformIO ([platformio.ini](platformio.ini)); default env
`esp32-s3-devkitc-1`, legacy `esp32dev`.

```bash
pio run                      # build (default env)
pio run -t upload            # flash over USB
pio device monitor           # 115200 baud serial monitor
pio run -e esp32dev          # legacy WROOM board
```

`pio` may not be on PATH → use `~/.platformio/penv/bin/pio`.
Manual smoke test in the monitor: `PING`, `INFO`, `SCAN`, `SHOW H05`,
`EXPECT H05`, `RECALIBRATE_ALL`.

---

# 3. Serial protocol (PROTOCOL_VERSION 3) — DO NOT BREAK

## 3.1 Transport & grammar

- 115200 baud, ASCII, line-based, `\n` terminated (`\r` tolerated on RX).
- Max command line: `SERIAL_LINE_MAX_LENGTH` = 64 chars.
- Positions: 3-char tokens `H01`–`H34` (case-insensitive RX, uppercase TX).
- Grammar: `<ACTION> [<args>] [#<id>]`. `#<id>` is a 32-bit correlation id
  chosen by the Pi; the firmware **echoes the same id** on every
  `ACK`/`DONE`/`ERR`/`BUSY`/event answering that command. Commands without
  `#id` are answered without one (`COMMAND_ID_NONE` = 0xFFFFFFFF internally).

## 3.2 Commands (Pi → ESP32)

### LED

| Command | Reply | Behavior |
|---|---|---|
| `SHOW <pos> [#id]` | `ACK` | Hold on in SHOW color (purple) |
| `HIDE <pos> [#id]` | `ACK` | Hold off |
| `HIDE_ALL [#id]` | `ACK` | Whole board off (also clears recording glow) |
| `FAIL <pos> [#id]` | `ACK` | Hold red |
| `BLINK <pos> [#id]` / `STOP_BLINK <pos> [#id]` | `ACK` | Green blink on/off |
| `EXPAND_STEP <pos> [#id]` / `CONTRACT_STEP <pos> [#id]` | `ACK` | Grow/shrink lit area by 1 LED per side |
| `INDICATE_RECORDING [#id]` | `ACK` | All holds static dim white |
| `SUCCESS <pos> [#id]` | `ACK` → `DONE` | **Async**: green expansion animation |
| `CONTRACT <pos> [#id]` | `ACK` → `DONE` | **Async**: contract expanded area |
| `SEQUENCE_COMPLETED [#id]` | `ACK` → `DONE` | **Async**: full-board victory pulses |
| `DEFEAT_ANIMATION [#id]` | `ACK` → `DONE` | **Async**: red full-board pulses |
| `MENUE_CHANGE <r,g,b> <range> [#id]` | `ACK` → `DONE` | **Async**: color sweep to `<range>` (spelling "MENUE" is intentional) |

Async commands (`SUCCESS`, `CONTRACT`, `SEQUENCE_COMPLETED`,
`DEFEAT_ANIMATION`, `MENUE_CHANGE`) get an immediate `ACK`, run via
`CommandController::tick()`, and emit `DONE` when finished. If the 32-slot
queue is full the firmware answers `BUSY [#id]` and the Pi retries (3×).

### Touch

| Command | Reply | Behavior |
|---|---|---|
| `EXPECT <pos> [#id]` | `ACK`, later `TOUCHED <pos> [#id]` | One-shot press detection. Fires immediately for an already-held input only if fresh (≤2 s) AND not yet consumed (§2.5) |
| `EXPECT_RELEASE <pos> [#id]` | `ACK`, later `TOUCH_RELEASED <pos> [#id]` | One-shot release; fires immediately if already released |
| `EXPECT_ANY [#id]` | `ACK`, later `TOUCHED <pos> [#id]` | First *qualified* new touch anywhere; queued (max 8 concurrent) |
| `EXPECT_ANY_EXCEPT [<pos> ...] [#id]` | like `EXPECT_ANY` | Listed positions excluded |
| `CLEAN_QUEUE [#id]` | `ACK` | Clears ALL expectations + EXPECT_ANY queue, disables hands-off detection |
| `RECALIBRATE <pos> [#id]` | `ACK`, then `RECALIBRATED <pos> [#id]` | Recalibrate one channel |
| `RECALIBRATE_ALL [#id]` | `ACK`, then `RECALIBRATED ALL [#id]` | Recalibrate all enabled channels |
| `VALUE <pos> [#id]` | `VALUE <pos> <delta> [#id]` | Signed delta register (-128..127) |
| `SET_SENSITIVITY <pos> <0-7> [#id]` | `ACK` | 0 = most sensitive. ⚠ Global per CAP1188 chip |
| `HANDSOFF_DETECTION_ON [#id]` | `ACK` + immediate `HANDS_ON`/`HANDS_OFF` | Enable occupancy events, report current state once |
| `HANDSOFF_DETECTION_OFF [#id]` | `ACK` | Disable occupancy events |

### Utility

| Command | Reply |
|---|---|
| `PING [#id]` | `ACK PING [#id]` |
| `INFO [#id]` | `INFO firmware=1.0.5 protocol=3 board=ESP32_S3_DEVKITC_1 [#id]` (also emitted once unsolicited at boot, before `HARDWARE INITIALISED`) |
| `SCAN [#id]` | `SCANNED [H01,H02,...] [#id]` — comma-separated, no spaces, only inputs whose parent sensor initialized |

## 3.3 Responses & events (ESP32 → Pi)

| Message | Format |
|---|---|
| `ACK` | `ACK <ACTION> [<pos>] [#id]` |
| `DONE` | `DONE <ACTION> [<pos>] [#id]` |
| `ERR` | `ERR <reason> [#id]` — reasons: `unknown_action`, `unknown_position`, `bad_format`, `invalid_level`, `command_failed` |
| `BUSY` | `BUSY [#id]` (queue full — Pi retries) |
| `TOUCHED` / `TOUCH_RELEASED` | `TOUCHED <pos> [#id]` |
| `SCANNED` | `SCANNED [<pos>,...] [#id]` |
| `RECALIBRATED` | `RECALIBRATED <pos>\|ALL [#id]` (Pi treats it as the DONE) |
| `VALUE` | `VALUE <pos> <delta> [#id]` |
| `INFO` | `INFO firmware=X protocol=Y board=Z [#id]` |
| `HANDS_ON` / `HANDS_OFF` | occupancy transitions (no position) |
| `SENSORS READY` / `SENSORS FAILED [...]` / `HARDWARE INITIALISED` | startup handshake only |
| `DIAG ...` | boot diagnostics; Pi logs and ignores |

## 3.4 Quirks the Pi compensates for

- **Action-name truncation:** `Event::action` is `char[16]`, so
  `HANDSOFF_DETECTION_ON` echoes as `ACK HANDSOFF_DETECT`. The Pi matches by
  action-name prefix. Don't rely on it — keep new command names ≤ 15 chars.
- **Id-less responses** are matched to the oldest pending command by action.

## 3.5 Known gap

- The Pi can send `GET_SENSITIVITY <pos> #id` (expects
  `SENSITIVITY <pos> <lvl>`); the firmware does **not** implement it yet and
  answers `ERR unknown_action`. Implement additively if needed.

## 3.6 Compatibility rules

- **Never change existing keywords, grammar, ordering or `#id` semantics.**
- Bump `FIRMWARE_VERSION` (semver) on every released change; bump
  `PROTOCOL_VERSION` only when message syntax/semantics change.
- New functionality = new keywords (additive), never repurposed ones.
- The Pi pins `REQUIRED_FIRMWARE_VERSION` and verifies `INFO` after flashing.

---

# 4. The Pi kiosk app & game logic (the counterpart)

Lives in `RaspberryPI_Kiosk_App/sequenzboard/`. The firmware agent does not
modify it, but must understand it — every firmware behavior exists to serve
this game logic.

## 4.1 Architecture

- **Frontend:** Svelte/TypeScript kiosk SPA (`frontend/`), fullscreen on the
  Pi touchscreen, real-time updates via WebSocket.
- **Backend:** FastAPI (`backend/app/main.py`), SQLite storage. Key services
  (`backend/app/services/`):

| Service | Role |
|---|---|
| `esp32_protocol.py` | Serial layer: port discovery (`/dev/ttyUSB*`, `/dev/ttyACM*`, env `SEQUENZBOARD_PORT`), reader thread, `#id` correlation, ACK/DONE futures. `MockProtocol` simulates the ESP32 for dev. `dsrdtr=False, rtscts=False` — no DTR reset |
| `hardware_bridge.py` | Connection façade; background reconnect loop (5 s) |
| `led_service.py` | Typed wrapper per LED command |
| `touch_service.py` | One-shot EXPECT registration, touch callbacks |
| `sequence_runner.py` | Sequence replay state machine (single/double steps, timeouts) |
| `recording_session.py` | Recording/add phase (EXPECT_ANY_EXCEPT loop, chord grouping) |
| `presence_monitor.py` | HANDS_ON/HANDS_OFF lifecycle → turn defeat |
| `firmware_update_service.py` | Flashes this firmware (§5) |

- **Pi-side protocol timeouts:** ACK 5 s; DONE 10 s; INFO 2 s; boot handshake
  15 s (non-fatal). `BUSY` → up to 3 retries, 0.1 s apart.

## 4.2 Game modes

| Mode | Description |
|---|---|
| **Kofferpacken** ("pack the suitcase", memory game) | 2–4 players, 1–3 lives. Player 1 records 2 moves; each next player replays the full sequence, then adds 2 new moves. Failure (timeout / hands off) costs a life. Last player standing wins |
| **Creative mode** | Solo: record a custom sequence, replay it, outcome screen |
| **Workouts** | Pre-authored multi-sequence training sessions with difficulty, pause and per-step timeout settings, optional video guides |
| **Highscores** | Leaderboard of workout completions |

**Sequence data format:** comma-separated steps; a step is 1 or 2 holds
joined by `+`. Example: `"H01,H02,H03+H04,H05"` = three single steps with one
double (two-hold) step in between.

## 4.3 Turn lifecycle (`sequence_runner.py`) — what the firmware sees

**Single step:**
```
HIDE <trailing holds>      (previous holds go dark, one may stay lit as rest hold)
SHOW H01 #12               (purple = next hold)
EXPECT H01 #13             (arm one-shot sensor)
  ← TOUCHED H01 #13        (player grabs)
SUCCESS H01 #14            (async green pulse; ACK → DONE)
→ next step
```

**Double step (`H03+H04`):** `SHOW`+`EXPECT` for both holds, waits for
**both** `TOUCHED` events in any order (250 ms grouping tolerance), then
`SUCCESS` for both.

**Timeouts (Pi-side):** step timeout 30 s default (workout-configurable
0–300 s); idle timeout ~2 s after first press of a step. On timeout: `FAIL`
on the missed hold(s), `DEFEAT_ANIMATION`, `sequence_aborted` broadcast.

**Victory:** all steps done → `SEQUENCE_COMPLETED` (full-board pulses) +
`sequence_complete` broadcast.

**Turn boundaries:** `CLEAN_QUEUE` (drops stale expectations, disables
hands-off detection — the presence monitor re-arms afterwards), `HIDE_ALL`.

## 4.4 Recording / add phase (`recording_session.py`)

1. `INDICATE_RECORDING` → dim-white glow over the whole board (first touch
   clears it with `HIDE_ALL`).
2. Loop: `EXPECT_ANY_EXCEPT <last two holds>` → `TOUCHED <pos>` →
   `SUCCESS <pos>` → re-arm with updated exclusion list.
3. **Chord window 350 ms:** touches close together are grouped into one
   double step; validation rejects 3+ hold chords, consecutive duplicates,
   and occupied-hold reuse (broadcasts `step_rejected`).
4. In Kofferpacken exactly 2 moves are added per turn; persisted only if the
   whole turn succeeds.

The firmware-side EXPECT_ANY qualification (§2.5) exists precisely for this
mode: every hold is armed, so brush-bys during reaching must be filtered.

## 4.5 Presence monitor (`presence_monitor.py`)

- Arms `HANDSOFF_DETECTION_ON` after the **second** `TOUCHED` of a turn.
- On `HANDS_OFF`: starts a ~1.2 s Pi-side grace timer (+ the firmware's
  800 ms release latch ≈ 2 s total). If no `HANDS_ON` arrives in time, the
  turn is failed (`turn_failed {reason: "hands_off"}`).
- After `CLEAN_QUEUE` (which disables detection firmware-side) the monitor
  re-asserts `HANDSOFF_DETECTION_ON`.

## 4.6 Frontend ↔ backend events (for context)

- Client → server (WebSocket): `start_game_turn {sequence}`,
  `start_sequence`, `start_recording`/`stop_recording`, `stop_game_turn`.
- Server → clients: `sequence_state`, `sequence_complete`,
  `sequence_aborted`, `step_recorded {holds}`, `step_rejected`,
  `turn_complete`, `turn_failed {reason}`, plus raw `sensor_down`/`sensor_up`.

## 4.7 Admin panel hardware features (all map to firmware commands)

| Admin feature | REST endpoint | Firmware command |
|---|---|---|
| Sensor list | `GET /api/hardware/sensors` | `SCAN` |
| Read sensor delta | `GET /api/hardware/sensor/{pos}/value` | `VALUE <pos>` |
| Recalibrate one/all | `POST .../recalibrate`, `/api/hardware/calibrate` | `RECALIBRATE`, `RECALIBRATE_ALL` |
| Sensitivity get/set | `GET/POST .../sensitivity` | `GET_SENSITIVITY` (⚠ not implemented in firmware), `SET_SENSITIVITY` |
| LED tests | `/api/hardware/led/*` | `SHOW`, `HIDE_ALL`, `SUCCESS`, `FAIL`, `BLINK`, `SEQUENCE_COMPLETED`, `DEFEAT_ANIMATION` |
| Connection check | `GET /api/hardware/ping` | `PING` |
| Firmware update | admin UI trigger | esptool flash (§5) |

---

# 5. Firmware update flow (how releases reach the board)

The Pi's `firmware_update_service.py` flashes over USB serial with esptool
(no OTA):

1. Checks the latest GitHub release of `maiksteffan/hardware_abstraction_layer`
   and compares `tag_name` (e.g. `v1.0.4`) with the running version from `INFO`.
2. Downloads the release asset **`firmware-merged.bin`**.
3. Releases the serial port and runs
   `python -m esptool --chip auto --port <port> --baud 460800 write_flash 0x0 firmware-merged.bin`.
4. Reconnects, waits for the handshake, sends `INFO`, and **verifies the
   reported version matches the release tag** — mismatch fails verification.

**Release checklist:** bump `FIRMWARE_VERSION` in Config.h to match the git
tag, build, publish `firmware-merged.bin` (merged bootloader + partitions +
app) as the release asset.

---

# 6. Coding standards (follow strictly)

## 6.1 Memory & language

- **No heap allocation after `setup()`.** No `new`/`malloc` in steady state,
  no `String` — fixed-size `char` buffers with `snprintf` and explicit
  null-termination.
- ALL configuration lives in [include/Config.h](include/Config.h) as
  `constexpr` (or `#define` with `#ifndef` guards where build-flag overrides
  are needed). Never hard-code magic numbers in .cpp files.
- Fixed-size arrays sized by Config constants; guard every index
  (`if (i >= INPUT_COUNT) return;`).
- Explicit `uint8_t`/`uint16_t`/`uint32_t`. Positions are input indices
  `0..33` internally, `"H01".."H34"` strings on the wire — convert only via
  `CommandController::parsePosition()` / `indexToPosition()`.

## 6.2 Concurrency

- Core 0 = touch/I²C only; Core 1 = serial/LED only. `EventQueue` is the only
  cross-core path. Never call `LedController` or `Serial` from the touch task.
- Never block: no `delay()` outside `setup()`/startup. Animations advance via
  overflow-safe `millis()` checks (`now - last >= interval`).

## 6.3 Style

- `PascalCase` classes, `camelCase` methods, `m_camelCase` members,
  `UPPER_SNAKE_CASE` constants, `enum class` enums.
- `@file`/`@brief` Doxygen block at the top of every file; *why*-comments for
  non-obvious logic (the debounce/EXPECT_ANY sections show the expected bar).
- One class per .h/.cpp pair; headers in `include/`, sources in `src/`.

## 6.4 Protocol discipline

- Every command answers with exactly one of `ACK`/`ERR`/`BUSY` (+ `DONE` for
  async). No silent failures.
- Echo `#id` faithfully; `COMMAND_ID_NONE` (0xFFFFFFFF) means "no id".
- All output goes through `EventQueue::queueXxx()` — never `Serial.print`
  directly (exception: `StartupController` before tasks exist).

## 6.5 Definition of done for any change

1. `pio run` compiles clean (no new warnings).
2. Protocol-adjacent changes sanity-checked against
   `RaspberryPI_Kiosk_App/sequenzboard/backend/app/services/esp32_protocol.py`
   and `backend/tests/test_esp32_protocol_*.py`.
3. `FIRMWARE_VERSION` bumped.
4. **This file (claude.md) updated** — and README.md if user-facing.
5. `CommandController.h`'s command-list comment kept in sync.

---

# 7. Repository layout

```
hardware_abstraction_layer/
├── platformio.ini            # build envs (esp32-s3-devkitc-1 default, esp32dev legacy)
├── claude.md                 # THIS FILE — update after every change
├── README.md                 # brief project overview
├── include/
│   ├── Config.h              # ALL configuration: pins, timing, colors, INPUT_MAPPINGS
│   ├── CommandController.h
│   ├── EventQueue.h
│   ├── LedController.h
│   ├── StartupController.h
│   └── TouchController.h
├── src/
│   ├── main.cpp              # setup(), loop(), FreeRTOS task creation
│   ├── CommandController.cpp
│   ├── EventQueue.cpp
│   ├── LedController.cpp
│   ├── StartupController.cpp
│   └── TouchController.cpp
└── RaspberryPI_Kiosk_App/    # checkout of the Pi counterpart (FinnStf/sequenzboard)
    └── sequenzboard/         # read for context — do not modify from here
```

---

# 8. Changelog (append entries here when you change the firmware)

| Version | Change |
|---|---|
| 1.0.5 | Startup now emits an unsolicited `INFO` line before `HARDWARE INITIALISED`; SHOW color tuned to dark purple (80,0,205) |
| 1.0.4 | `pressConsumed` single-consumption rule fixes false double touches on re-armed `EXPECT`; SHOW color changed blue → purple |
| 1.0.3 | Prior baseline (S3 board support, EXPECT_ANY qualification, hands-off detection, DIAG boot output) |