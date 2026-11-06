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
| `FIRMWARE_VERSION` | `1.3.0` |
| `PROTOCOL_VERSION` | `3` (H01..H{holdCount} 3-char position tokens, per active board profile) |
| `BOARD_TYPE` | `ESP32_S3_DEVKITC_1` (overridable per build env) |

---

# 2. The ESP32 firmware (this repository)

## 2.1 Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3-DevKitC-1-N8R8 (8 MB flash, 8 MB PSRAM), serial over native USB-CDC |
| LED strips | 2× WS2812/NeoPixel, 260 LEDs each, data on GPIO 18 / GPIO 17 |
| Touch | 5× CAP1188 (I²C addresses 0x28–0x2C), SDA=GPIO7, SCL=GPIO6, up to 8 channels per chip as selected by the profile (v2 H32 uses CS8) |
| Legacy board | classic ESP32 WROOM still buildable via `pio run -e esp32dev` (pins overridden by build flags) |

### Board profiles

One binary serves every physical board build. Everything that differs between
builds — hold count, sensor count and I²C addresses, `inputMappings[]`
(hold → chip+channel), `ledMappings[]` / `ledMirrors[]` (hold → strip position;
a mirror = second LED block for the same hold on the other strip), LED width
and strip lengths — lives in a `BoardProfile`.

Profiles are declared in [src/BoardProfiles.cpp](src/BoardProfiles.cpp), the
types and ceilings in [include/BoardProfile.h](include/BoardProfile.h). Read
the active one with `activeBoardProfile()`; **never** hard-code a hold or
sensor count. Per-hold arrays are sized `MAX_HOLDS` (99, the cap implied by the
4-byte position token) and per-sensor arrays `MAX_SENSORS`, while every loop
bound and range check uses the active profile's counts.

Adding a board version = one entry in `PROFILES[]`. Rewiring an existing board
is a **new slug**, not an edit: the Pi has no other way to tell two wirings of
`v1` apart.

The Pi names the profile during the startup handshake; until then the default
profile is active. See `docs/Board_Version_Firmware_Profiles.md` in the
Sequenzboard repo.

## 2.2 Dual-core FreeRTOS architecture

| Core | Runs | Owns |
|---|---|---|
| **Core 0** | `touchPollingTask` → `TouchController::tick()` every 5 ms (`vTaskDelayUntil`) | ALL I²C traffic |
| **Core 1** | Arduino `loop()` | Serial RX/TX, command execution, ALL NeoPixel writes |

Outgoing cross-core events use the **`EventQueue`** (FreeRTOS-mutex-protected,
64 slots). Command-side expectation registration shares a short
`m_expectationMux` critical section with the polling core; no I²C or event
enqueue operation may run inside that critical section. Touch code on Core 0
must never call `LedController` or write to `Serial` directly.

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

0. **`ESP32 BOOT`** banner, then **profile negotiation** — see below. Must come
   before LED and sensor init, which read the per-profile wiring tables.
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
5. Sends the resolved `INFO` line — now including `boardVersion=` / `holds=` —
   then `HARDWARE INITIALISED` → touch task is created, normal operation.
   (The Pi also still queries `INFO` after connecting.)

### Profile negotiation (step 0)

```
ESP32 -> ESP32 BOOT
ESP32 -> INFO firmware=1.1.0 protocol=3 board=ESP32_S3_DEVKITC_1 profiles=v1 default=v1
Pi    -> BOARD_VERSION v1
ESP32 -> ACK BOARD_VERSION v1 holds=34
```

- The Pi has up to `BOARD_VERSION_TIMEOUT_MS` (3 s) to answer. On timeout the
  **default profile** stays active and boot continues — that is the normal path
  for a dev flash with no Pi attached.
- Unknown slug → `ERR unknown_board_version <slug> [<available>]`, the default
  profile stays active, and the later `INFO` carries `requested=<slug>
  mismatch=1`. The board boots wrong-but-diagnosable rather than not at all.
- Any other line during the wait re-triggers the `INFO` banner, so a
  late-connecting Pi can still drive the exchange.
- `buildBoardInfoLine()` in [src/BoardProfiles.cpp](src/BoardProfiles.cpp) is the
  single source of the `INFO` format — the banner and the `INFO` command
  response must not drift, because the Pi parses both.

## 2.5 Touch pipeline (the tricky part — read before touching TouchController)

Runs on Core 0 every 5 ms:

1. Read each active CAP1188's status register **once** per cycle (cached in
   `PhysicalSensor::lastStatus`) — never per-input reads.
2. Update the profile's logical inputs via `inputMappings`.
3. Clear the INT bit on chips that reported a touch (keeps latching alive).
4. Debounce → expectation matching → EXPECT_ANY qualification → hands-off.

**Debounce (asymmetric):** press is instant (`TOUCH_DEBOUNCE_PRESS_MS` = 0);
release is latched `TOUCH_DEBOUNCE_RELEASE_MS` = 500 ms — transient
re-contacts keep the touch held. Release timers are per hold, so releasing
one hold never blocks another hold's press. Since 1.2.5, release uses
`releaseEvidenceMs`: a valid untouched poll contributes its actual sample
interval (only if ≤ `TOUCH_RELEASE_MAX_SAMPLE_GAP_MS`, 50 ms), a valid touched
poll resets the evidence, and a failed status read
pauses it without erasing progress. This avoids indefinite consumption lockout
under periodic I2C errors. Missing samples and long bus gaps cannot confirm a
release from stale cached state. A consumed
hold still needs the full release latch before re-grabbing it can count again.

**Single-consumption rule (`pressConsumed`, critical fix in v1.0.4):** each
debounced press edge may satisfy **exactly one** `EXPECT`/`EXPECT_ANY`. Set
when a `TOUCHED` is reported, cleared only on genuine release + re-grab.
Without it, a continuously held hold satisfied every re-armed `EXPECT` within
the freshness window and alternating two-hold sequences ping-ponged instantly
(false double touches). **Preserve this invariant in any refactor.**
Both expectation types now use this same flag; there is no separate
queue-batch used mask to accidentally prevent reuse after a genuine release.

**Single-hold expectations (since 1.2.4):** `setExpectDown()` and
`setExpectUp()` atomically publish a request; `processPointExpectations()`
evaluates it on every sensor tick, not only on edges. An already-held EXPECT
fires within one poll only if its status read is valid,
`currentTouched && debouncedTouched && !pressConsumed` and the press is at most
`EXPECT_HELD_FRESH_MS` (2 s) old. A raw-untouched hold still inside the release
latch cannot satisfy a new EXPECT. EXPECT_RELEASE likewise handles an already
debounced-released input on the next valid poll. This removes the old
check-then-arm race between command registration and a press on the other core.

**Reliable touch reports:** a matched EXPECT/EXPECT_RELEASE is retained as a
pending report until `EventQueue` accepts it; a qualified EXPECT_ANY similarly
reserves its expectation and retains one pending report while further candidates
wait. Queue-full or queue-mutex timeouts do not silently consume/drop the event.
A claimed grab stays consumed to avoid duplicate reports, even if delivery is
delayed beyond release or freshness expiry. Per-request revision counters keep
completion of an old delivery from clearing a concurrently rearmed command,
including commands without ids. Replacing/clearing an expectation cancels its
undelivered point report; `CLEAN_QUEUE` also drops pending ANY reports.
Since 1.2.5, polling-core submissions use `MUTEX_TIMEOUT_TOUCH_QUEUE_MS = 0`:
event queue contention never adds 10 ms per pending report to the sensor tick.
Point/ANY reports retry without blocking, as do current occupancy transitions.
Command-side queue calls keep their existing timeout.

**EXPECT_ANY qualification (brush-by rejection):** `EXPECT <pos>` is instant
(only the armed hold can fire), but `EXPECT_ANY` arms every hold, so a new
press must survive a 150 ms confirmation window (`AnyCandidate`):
persistence (≥3 consecutive untouched samples cancels), delta consistency
(≥70 % of samples with CAP1188 delta ≥ 48; degrades gracefully on I²C
failures), and sweep plausibility (≥3 press edges in 500 ms = arm sweeping
across the wall → all decisions deferred 400 ms). Every press opens a candidate,
even while no expectation is queued, so a new hold grabbed during a Pi rearm
gap is not lost. Fresh candidates stay pending when the queue is empty or the
current expectation excludes their hold, and can satisfy a later eligible
expectation within `EXPECT_HELD_FRESH_MS` (2 s from the original press).
After a raw dropout cancels qualification, re-contact during the release latch
restarts it only for fresh, unconsumed grabs; consumed grabs never fire twice.
`CLEAN_QUEUE` still discards all pending candidates. No event is emitted without
an eligible expectation, and status-read failures cannot qualify cached contact.
Raw candidate tracking continues during rearm gaps, but delta-register sampling
is skipped when the EXPECT_ANY queue is empty, keeping recording-only I2C work
out of ordinary single-hold sequence replay.

**On-demand diagnosis (since 1.2.5):** `TOUCH_STATE <pos> [#id]` gets ACK then
one polling-core snapshot of sensor-active/read-valid/raw/debounced/consumed
state, press age, observed release evidence, and single-hold expectation/pending
flags. It adds no I2C reads, never recalibrates or consumes a grab, and retries
delivery with zero mutex wait. Query before recalibrating a blocked hold to
distinguish sensor silence, invalid I2C reads, consumption and delivery gating.
Only one diagnostic request should be outstanding; a newer request replaces
an undelivered one. Existing protocol messages/ids are unchanged.

**Hands-off detection:** while enabled, emits `HANDS_ON`/`HANDS_OFF` on
board-occupancy transitions; enabling reports the current state immediately.
The 500 ms release latch means `HANDS_OFF` lags the last release by ~0.5 s.
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

### Hold-time gauge

A hold's core and its success rings form one unbroken run of physical LEDs
around the mapped index, so `HOLD_GAUGE <pos> <percent>` can light that run
from the center outward: 100 % is the full green block, 0 % the center LED
alone. The geometry lives in the pure, host-tested
[include/HoldGauge.h](include/HoldGauge.h) — with a 5-LED core and 4 rings a
hold has 7 levels (1/3/5/7/9/11/13 LEDs).

Workouts can require a hold to stay gripped for a minimum time; the Pi ticks
the remaining percentage down and the block shrinks with it. The value is
**absolute**, not relative like `CONTRACT_STEP`, so a dropped command is
corrected by the next one, and the firmware keeps no timer: it never decides
when the hold time is over. `PositionState::GAUGE` is a static state — `SHOW`,
`SUCCESS`, `FAIL`, `HIDE` and `HIDE_ALL` clear it like an expanded block, and
the snake repaints over it as usual.

### Track & snake overlay

For the Snake game both strips are chained into one line, the **track**
([include/SnakeTrack.h](include/SnakeTrack.h), pure and host-tested): strip 2
backwards, then strip 1, so with `L1`/`L2` the active profile's strip
lengths, track index `t < L2` is `(STRIP2, L2-1-t)` and `t >= L2` is
`(STRIP1, t-L2)`; the track is `L1+L2` long. A profile without strip 2
(`dev-5hold`) is just strip 1.

`SNAKE` paints a span of it as an **overlay** on top of the positions — the
backend owns the snake, the firmware only paints where it is told. The
backend moves it up to ~40×/s, so a move is cheap: LEDs the snake leaves go
dark, the positions whose block (core, expansion rings, mirror) touches them
are repainted from their state (`repaintPosition()`; `PositionData` keeps
the core color because `SHOWN` may be purple, red after `FAIL` or green after
`CONTRACT`), and `update()` repaints the snake before every `show()`, so
running animations (success expansion, blink, full-board pulses) never draw
over it. `HIDE_ALL` drops the overlay too.

## 2.7 Build, flash, debug

PlatformIO ([platformio.ini](platformio.ini)); default env
`esp32-s3-devkitc-1`, legacy `esp32dev`.

```bash
pio run                      # build (default env)
pio run -t upload            # flash over USB
pio device monitor           # 115200 baud serial monitor
pio run -e esp32dev          # legacy WROOM board
pio test -e native           # host tests: profiles, INFO, track math, touch transitions
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
| `HOLD_GAUGE <pos> <0-100> [#id]` | `ACK` | Green block scaled to that share of its full width (hold-time gauge, see 2.6). Absolute; 0 leaves the center LED on. Out of range → `ERR invalid_level`, missing value → `ERR bad_format`. Firmware with `features=holdgauge` only |
| `INDICATE_RECORDING [#id]` | `ACK` | All holds static dim white |
| `SUCCESS <pos> [#id]` | `ACK` → `DONE` | **Async**: green expansion animation |
| `CONTRACT <pos> [#id]` | `ACK` → `DONE` | **Async**: contract expanded area |
| `SEQUENCE_COMPLETED [#id]` | `ACK` → `DONE` | **Async**: full-board victory pulses |
| `DEFEAT_ANIMATION [#id]` | `ACK` → `DONE` | **Async**: red full-board pulses |
| `MENUE_CHANGE <r,g,b> <range> [#id]` | `ACK` → `DONE` | **Async**: color sweep to `<range>` (spelling "MENUE" is intentional) |
| `SNAKE <start> <len> [r,g,b] [#id]` | `ACK SNAKE [#id]` | Overlay track span `[start, start+len)` (see 2.6), default red `255,0,0`; clamped to the track, `start` may be negative. Moves the existing overlay. Malformed → `ERR bad_format` |
| `SNAKE_OFF [#id]` | `ACK SNAKE_OFF [#id]` | Remove the overlay, restoring what it covered |

Async commands (`SUCCESS`, `CONTRACT`, `SEQUENCE_COMPLETED`,
`DEFEAT_ANIMATION`, `MENUE_CHANGE`) get an immediate `ACK`, run via
`CommandController::tick()`, and emit `DONE` when finished. If the 32-slot
queue is full the firmware answers `BUSY [#id]` and the Pi retries (3×).

### Touch

| Command | Reply | Behavior |
|---|---|---|
| `EXPECT <pos> [#id]` | `ACK`, later `TOUCHED <pos> [#id]` | One-shot press detection. Evaluated each poll; an already-held input must have valid raw contact, be fresh (≤2 s) AND not yet consumed (§2.5). Matched reports retry on outgoing queue pressure |
| `EXPECT_RELEASE <pos> [#id]` | `ACK`, later `TOUCH_RELEASED <pos> [#id]` | One-shot release; an already-released input reports on the next valid poll. Matched reports retry on outgoing queue pressure |
| `EXPECT_ANY [#id]` | `ACK`, later `TOUCHED <pos> [#id]` | First *qualified* new touch anywhere; queued (max 8 concurrent) |
| `EXPECT_ANY_EXCEPT [<pos> ...] [#id]` | like `EXPECT_ANY` | Listed positions excluded |
| `CLEAN_QUEUE [#id]` | `ACK` | Clears ALL expectations + EXPECT_ANY queue, disables hands-off detection |
| `RECALIBRATE <pos> [#id]` | `ACK`, then `RECALIBRATED <pos> [#id]` | Recalibrate one channel |
| `RECALIBRATE_ALL [#id]` | `ACK`, then `RECALIBRATED ALL [#id]` | Recalibrate all enabled channels |
| `VALUE <pos> [#id]` | `VALUE <pos> <delta> [#id]` | Signed delta register (-128..127) |
| `TOUCH_STATE <pos> [#id]` | `ACK`, then `TOUCH_STATE <pos> active=<0/1> valid=<0/1> raw=<0/1> held=<0/1> used=<0/1> age_ms=<n> release_ms=<n> expect=<0/1> pending=<0/1> [#id]` | Non-mutating polling-core diagnostic; `raw` is cached when `valid=0`; `expect`/`pending` refer to single-hold EXPECT. One outstanding request, newest replaces undelivered request |
| `SET_SENSITIVITY <pos> <0-7> [#id]` | `ACK` | 0 = most sensitive. ⚠ Global per CAP1188 chip |
| `HANDSOFF_DETECTION_ON [#id]` | `ACK` + immediate `HANDS_ON`/`HANDS_OFF` | Enable occupancy events, report current state once |
| `HANDSOFF_DETECTION_OFF [#id]` | `ACK` | Disable occupancy events |

### Utility

| Command | Reply |
|---|---|
| `PING [#id]` | `ACK PING [#id]` |
| `INFO [#id]` | `INFO firmware=1.3.0 protocol=3 board=ESP32_S3_DEVKITC_1 profiles=v1,v2,dev-5hold default=v1 features=snake,holdgauge boardVersion=v1 holds=34 [#id]` (also emitted unsolicited at boot: once as a banner without `boardVersion`/`holds`, once resolved before `HARDWARE INITIALISED`) |
| `BOARD_VERSION <slug>` | `ACK BOARD_VERSION <slug> holds=<n>` — **boot only**, see 2.4. Unknown slug → `ERR unknown_board_version <slug> [<available>]`; missing slug → `ERR bad_format` |
| `SCAN [#id]` | `SCANNED [H01,H02,...] [#id]` — comma-separated, no spaces, only inputs whose parent sensor initialized |
| `TRACK [#id]` | `ACK TRACK [#id]`, `TRACK length=<L1+L2> width=<ledPositionWidth>`, one `TRACK_HOLD <pos> <t>` per hold (`<t>,<t_mirror>` for a mirrored hold, primary first), `DONE TRACK [#id]`. Written straight to serial after draining the event queue (too long for one event, too many lines for the queue); touch events may interleave between lines |

## 3.3 Responses & events (ESP32 → Pi)

| Message | Format |
|---|---|
| `ACK` | `ACK <ACTION> [<pos>] [#id]` |
| `DONE` | `DONE <ACTION> [<pos>] [#id]` |
| `ERR` | `ERR <reason> [#id]` — reasons: `unknown_action`, `unknown_position`, `bad_format`, `invalid_level`, `command_failed`, `unknown_board_version` (boot only) |
| `BUSY` | `BUSY [#id]` (queue full — Pi retries) |
| `TOUCHED` / `TOUCH_RELEASED` | `TOUCHED <pos> [#id]` |
| `SCANNED` | `SCANNED [<pos>,...] [#id]` |
| `RECALIBRATED` | `RECALIBRATED <pos>\|ALL [#id]` (Pi treats it as the DONE) |
| `VALUE` | `VALUE <pos> <delta> [#id]` |
| `TOUCH_STATE` | `TOUCH_STATE <pos> active=<0/1> valid=<0/1> raw=<0/1> held=<0/1> used=<0/1> age_ms=<n> release_ms=<n> expect=<0/1> pending=<0/1> [#id]` — requested diagnostics only |
| `INFO` | `INFO firmware=X protocol=Y board=Z profiles=<csv> default=<slug> features=<csv> [boardVersion=<slug> holds=<n>] [requested=<slug> mismatch=1] [#id]` — `features` lists optional command families (`snake` = TRACK/SNAKE/SNAKE_OFF, since 1.2.0; `holdgauge` = HOLD_GAUGE, since 1.3.0) |
| `TRACK` / `TRACK_HOLD` | reply lines of `TRACK` only, between its `ACK` and `DONE` |
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
- New functionality = new keywords (additive), never repurposed ones. New
  `INFO` fields are additive too: the Pi ignores keys it does not know, and
  firmware older than 1.1.0 simply omits the profile keys.
- The Pi pins `REQUIRED_FIRMWARE_VERSION` and verifies `INFO` after flashing.
  This is being replaced by a profile-membership check — see
  `docs/Board_Version_Firmware_Profiles.md` in the Sequenzboard repo.

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

**Minimum hold time (Pi-side, workouts only):** a workout step may require its
hold(s) to stay gripped for a whole number of seconds. The step's holds carry
it in the sequence text (`H05@3`), so only the Pi ever knows about it. After
the touch the Pi plays `SUCCESS`, then ticks `HOLD_GAUGE <pos> <percent>` down
while watching `EXPECT_RELEASE`; letting go restarts the time, and the next
hold is only shown once it is served. Steps without a hold time behave exactly
as before — no `HOLD_GAUGE` is sent at all.

**Required dropout handling:** countdown code must restart only on the debounced
`TOUCH_RELEASED` reply, never on the first raw no-touch sample. The existing
500 ms release-evidence filter (§2.5) discards all accumulated negative evidence
on any valid positive touch sample, so separate brief dropouts cannot add up to
a release. I2C read failures contribute no evidence. Native touch regressions
verify repeated 5–495 ms dropouts over multiple seconds, arming `EXPECT_RELEASE`
during a dropout, and independent filtering of double-hold steps. These tests
verify existing firmware behavior, not the Pi countdown implementation or
physical sensor stability; no production behavior or firmware version changed.

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
  500 ms release latch ≈ 1.7 s total). If no `HANDS_ON` arrives in time, the
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
- Board-independent configuration lives in [include/Config.h](include/Config.h)
  as `constexpr` (or `#define` with `#ifndef` guards where build-flag overrides
  are needed); board-specific wiring lives in a `BoardProfile`. Never hard-code
  magic numbers in .cpp files.
- Fixed-size arrays sized by the `MAX_*` ceilings in
  [include/BoardProfile.h](include/BoardProfile.h); guard every index against
  the *active profile* (`if (i >= activeBoardProfile().holdCount) return;`).
- Explicit `uint8_t`/`uint16_t`/`uint32_t`. Positions are input indices
  `0..holdCount-1` internally, `"H01".."H99"` strings on the wire — convert
  only via `CommandController::parsePosition()` / `indexToPosition()`.

## 6.2 Concurrency

- Core 0 = touch/I²C only; Core 1 = serial/LED only. Outgoing events cross via
  `EventQueue`; expectation requests share `m_expectationMux`. Never hold that
  critical section across I²C or EventQueue calls. Never call `LedController`
  or `Serial` from the touch task.
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
│   ├── Config.h              # board-independent config: pins, timing, colors
│   ├── BoardProfile.h        # per-board wiring types, ceilings, profile registry API
│   ├── SnakeTrack.h          # both strips as one track (TRACK/SNAKE), pure, host-tested
│   ├── CommandController.h
│   ├── EventQueue.h
│   ├── LedController.h
│   ├── StartupController.h
│   └── TouchController.h
├── src/
│   ├── main.cpp              # setup(), loop(), FreeRTOS task creation
│   ├── BoardProfiles.cpp     # the board profiles this firmware ships
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
| 1.3.0 | Hold-time gauge: new `HOLD_GAUGE <pos> <0-100>` lights a hold's green block from the center outward (core + success rings as one run of LEDs, 7 levels on a 5-LED core), so the Pi can show how much longer a hold must stay gripped during a workout step. Absolute value, no firmware-side timer; new static `PositionState::GAUGE` that SHOW/SUCCESS/FAIL/HIDE/HIDE_ALL clear and the snake repaints over. Geometry in the pure, host-tested `HoldGauge.h`; `INFO` gains `features=holdgauge` |
| 1.2.5 | Address replay lockout paths missed by zero-latency host mocks: periodic I2C failures now pause rather than restart observed release evidence, while valid re-contact still resets it; polling-core touch/release/occupancy delivery uses zero-wait mutex acquisition and retries; skip EXPECT_ANY delta reads while no ANY request is queued. Add non-mutating `TOUCH_STATE <pos>` diagnostic to expose the actual sensor/consumption/expectation gate before recalibration. Add periodic-error, mutex-time, replay-I2C, release-evidence and diagnostic regressions. Preserve 500 ms evidence threshold, single-consumption and existing protocol messages. Physical cause on the reported board remains unverified |
| 1.2.4 | Fix missed single-hold touches at sequence start/mid-sequence: evaluate EXPECT/EXPECT_RELEASE on every sensor tick with synchronized registration instead of a command-side check-then-arm race; do not accept raw-untouched release-latched holds. Retain matched touch/release and qualified EXPECT_ANY reports across event queue rejection, with revision-safe completion and CLEAN_QUEUE cancellation. Preserve single-consumption, 2 s freshness, 500 ms release latch, protocol grammar and ids. Add event-delivery and expectation lifecycle regressions |
| 1.2.3 | Fix missed touches during hold handoffs: qualify fresh grabs across EXPECT_ANY rearm gaps, recover unconsumed candidates after raw dropouts, retain fresh excluded candidates for later expectations, and share `pressConsumed` between EXPECT and EXPECT_ANY instead of a batch-wide used mask. Missing I²C status reads no longer confirm releases. Preserve the working-tree 500 ms release latch. Correct CAP1188 channel ceiling to 8 so v2 H32/CS8 is enabled. Native touch-controller regression tests; protocol grammar and ids unchanged |
| 1.2.0 | Snake support. New `TRACK` (reports the LED track: both strips chained, strip 2 backwards then strip 1, plus every hold's track index), `SNAKE <start> <len> [r,g,b]` (free track span as an overlay on top of the positions) and `SNAKE_OFF`; `HIDE_ALL` also clears the overlay. `INFO` gains `features=snake`. Track math in the pure `SnakeTrack.h`, host-tested |
| 1.1.3 | Default CAP1188 sensitivity changed 3 → 2 (`CAP1188_DEFAULT_SENSITIVITY`, more sensitive) |
| 1.1.2 | LED data pins moved into the board profile (`ledPin1`/`ledPin2`). Two board builds on the same MCU do not share their LED wiring, so the pin could not stay in the per-MCU build flags: `dev-5hold` drives its single strip from GPIO25, and driving the WROOM default 19 left it dark, boot sweep included. That profile's holds now sit on STRIP1 — the strip that exists — with STRIP2 length 0 |
| 1.1.1 | Release builds one merged binary per MCU: `firmware-merged.bin` (ESP32-S3) and `firmware-merged-esp32.bin` (classic ESP32 WROOM), each verified in CI against the `chip_id` in its image header. The `esp32dev` env now overrides the S3 pin defaults (I2C 21/22, LED 18/19) — GPIO 6/7 are SPI-flash pins on the WROOM, so the sensor scan came back empty |
| 1.1.0 | Board profiles: one binary serves several board builds. New `BOARD_VERSION <slug>` boot command, `INFO` gains `profiles=`/`default=`/`boardVersion=`/`holds=`/`requested=`/`mismatch=`, wiring tables moved from `Config.h` + `LedController.cpp` into `PROFILES[]`. `excludeMask` is now a `HoldMask` bitset (a `uint64_t` would have capped the board at 64 holds). Fixes `m_statusMsg` truncating the `SENSORS FAILED [...]` list at 64 bytes |
| 1.0.5 | Startup now emits an unsolicited `INFO` line before `HARDWARE INITIALISED`; SHOW color tuned to dark purple (80,0,205) |
| 1.0.4 | `pressConsumed` single-consumption rule fixes false double touches on re-armed `EXPECT`; SHOW color changed blue → purple |
| 1.0.3 | Prior baseline (S3 board support, EXPECT_ANY qualification, hands-off detection, DIAG boot output) |
