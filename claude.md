# Hardware Abstraction Layer — Project Overview & Migration Plan

> **Purpose of this document:** First half describes the current project. Second half is a precise, step-by-step migration plan for adapting the firmware to the **new hardware setup** (5 CAP1188 sensors with multi-channel use, 34 inputs named `H01`–`H34`). The migration plan is written to be executed by a GitHub Copilot agent.

---

# PART 1 — Project Overview

## What this project is

Firmware for a **Freenove ESP32 WROOM** (dual-core Xtensa LX6 @ 240 MHz) that acts as a **hardware executor** between a Raspberry Pi 5 (game logic) and physical hardware (LED strips + CAP1188 capacitive touch sensors). The ESP32 has **no game logic** — it executes commands received over UART serial and reports hardware events back to the Pi.

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
| `CommandController` | [src/CommandController.cpp](src/CommandController.cpp) | Parses and executes serial commands from the Pi |
| `EventQueue` | [src/EventQueue.cpp](src/EventQueue.cpp) | Thread-safe queue that buffers outgoing serial messages |
| `LedController` | [src/LedController.cpp](src/LedController.cpp) | Controls two NeoPixel LED strips |
| `TouchController` | [src/TouchController.cpp](src/TouchController.cpp) | Manages CAP1188 capacitive touch sensors over I2C |
| `StartupController` | [src/StartupController.cpp](src/StartupController.cpp) | Hardware init and Pi handshake on boot |

## Main Loop (`loop()`)

```
1. commandController.pollSerial()             — Read UART bytes into ring buffer
2. commandController.processCompletedLines()  — Parse & execute complete lines
3. commandController.tick()                   — Advance long-running command animations
4. ledController.tick()                       — Step LED animations
5. eventQueue.flush(EVENTS_PER_FLUSH)         — Send up to 5 queued events over serial
```

## Startup Sequence

1. **LED init** — strips initialized + white pixel sweep animation.
2. **Sensor init** — up to 3 retries to initialize all CAP1188 sensors over I2C.
3. **Handshake loop** — broadcasts `SENSORS READY` (or `SENSORS FAILED [...]`) every 3 s until Pi replies `ACK SENSORS READY`.
4. Sends `HARDWARE INITIALISED` to begin normal operation.

## Serial Protocol (CURRENT)

### Transport
- Baud rate: **115200**, ASCII, line-based (`\n` / `\r` terminated), max 64 chars/line.

### Pi → ESP32 Commands

Grammar: `<ACTION> [<position>] [<extra>] [#<id>]`

Where `<position>` is currently **a single character A–Y**.

| Category | Commands |
|----------|----------|
| **LED** | `SHOW`, `HIDE`, `HIDE_ALL`, `SUCCESS`, `FAIL`, `CONTRACT`, `BLINK`, `STOP_BLINK`, `EXPAND_STEP`, `CONTRACT_STEP`, `MENUE_CHANGE <r,g,b> <range>`, `SEQUENCE_COMPLETED` |
| **Touch** | `EXPECT`, `EXPECT_ANY`, `EXPECT_RELEASE`, `RECALIBRATE`, `RECALIBRATE_ALL`, `VALUE`, `SET_SENSITIVITY <pos> <0-7>` |
| **Utility** | `PING`, `INFO`, `SCAN` |

**Long-running** commands (`SUCCESS`, `CONTRACT`, `MENUE_CHANGE`, `SEQUENCE_COMPLETED`) get an immediate `ACK`, run in the background, and emit `DONE` when finished. If the queue (32 slots) is full, the ESP32 sends `BUSY`.

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

## Current Hardware

| Component | Current setup |
|-----------|---------------|
| LED strips | 2× NeoPixel, 190 LEDs each (GPIO 18, GPIO 25) |
| Logical positions | **25** (`A`–`Y`) |
| Touch sensors | **25× CAP1188**, one input each (only **CS1** / channel 0 enabled) |
| I2C addresses | 25 entries in [include/Config.h](include/Config.h) `SENSOR_I2C_ADDRESSES` |
| I2C bus | GPIO 21 (SDA), GPIO 22 (SCL), 400 kHz |

---

# PART 2 — Migration Plan: New Hardware Setup

## New hardware reality

| Property | Old | **New** |
|----------|-----|---------|
| Number of CAP1188 sensors | 25 | **5** |
| Channels used per sensor | 1 (CS1 only) | **7** (CS1–CS7, i.e. channels 0–6) |
| I2C addresses | 25-entry list | **`0x28, 0x29, 0x2A, 0x2B, 0x2C`** |
| Total logical inputs | 25 | **34** |
| Input naming | `A`–`Y` (1 char) | **`H01`–`H34` (3-char string)** |
| Input-to-hardware map | implicit (index = address index) | **explicit user-defined: each `H01`…`H34` → `(sensor_index, channel)`** |

> 5 sensors × 7 channels = 35 raw channels. The user explicitly wants **34 inputs**, meaning **one (sensor, channel) combination is unused**. The map must be fully user-configurable in [include/Config.h](include/Config.h).

## Hard constraint (do not break!)

**The serial protocol must remain identical** in:
- All command keywords (`SHOW`, `HIDE`, `EXPECT`, `MENUE_CHANGE`, etc.)
- All response keywords (`ACK`, `DONE`, `ERR`, `BUSY`, `TOUCHED`, `TOUCH_RELEASED`, `SCANNED`, `RECALIBRATED`, `VALUE`, `INFO`)
- Message grammar / ordering / `#id` semantics
- Long-running command flow (`ACK` → `DONE`)
- Startup handshake (`SENSORS READY` / `SENSORS FAILED [...]` / `ACK SENSORS READY` / `HARDWARE INITIALISED`)

**The ONLY allowed protocol change:** the `<pos>` token changes from a **single character `A`–`Y`** to a **three-character string `H01`–`H34`**. Everywhere a position is sent or received, in both directions, in commands and events alike (including the comma-separated list inside `SCANNED [...]`).

## Open assumption that must be confirmed before coding

The user described **34 input positions**. The LED system currently has **25 positions (A–Y)** with a hard-coded mapping table to physical LED indices. The user did **not** explicitly say whether the LEDs are also reorganized into 34 positions, but since LED commands take a `<pos>` argument and the only allowed protocol change is renaming positions, **the LED side must also expose 34 positions `H01`–`H34`**. The Copilot agent should:

- Set `LED_POSITION_COUNT = 34`.
- Replace the `LED_MAPPINGS[]` table with a **placeholder 34-entry table** where each entry maps `H01`–`H34` to `{strip, ledIndex}`. Add a `TODO` comment instructing the user to fill in the real LED indices.
- Pre-fill placeholder values that won't crash (e.g. `{ StripId::STRIP1, 0 }` for every entry).

---

## Concrete change list (file by file)

### 1. [include/Config.h](include/Config.h)

#### 1a. Sensor/input counts
```cpp
// REPLACE:
constexpr uint8_t TOUCH_SENSOR_COUNT = 5;   // (was 5 already, keep as physical-sensor count)
// ADD:
constexpr uint8_t TOUCH_CHANNELS_PER_SENSOR = 7;   // CS1..CS7 used (channels 0..6)
constexpr uint8_t INPUT_COUNT = 34;                // Total logical inputs H01..H34
constexpr uint8_t LED_POSITION_COUNT = 34;         // was 25
```

> Note: the existing `TOUCH_SENSOR_COUNT` is already `5` — coincidence of the dev board. Keep its meaning = "number of physical CAP1188 chips". Reuse the symbol.

#### 1b. I2C addresses
```cpp
// REPLACE the existing 25-entry SENSOR_I2C_ADDRESSES[25] with:
constexpr uint8_t SENSOR_I2C_ADDRESSES[TOUCH_SENSOR_COUNT] = {
    0x28, 0x29, 0x2A, 0x2B, 0x2C
};
```

#### 1c. Position string format
```cpp
// Length of a position string including null terminator (e.g. "H01\0")
constexpr uint8_t POSITION_STRING_LENGTH = 4;
```

#### 1d. Input-to-hardware mapping table (NEW)

Add a new struct + table near the sensor-address block. **This is the user-facing configuration the user explicitly asked for.**

```cpp
// ============================================================================
// 10b. INPUT MAPPING: H01..H34  ->  (sensor_index, channel)
// ============================================================================
// sensor_index: 0..4 (corresponds to SENSOR_I2C_ADDRESSES[sensor_index])
// channel:      0..6 (CAP1188 CS1..CS7 inputs)
//
// EDIT THIS TABLE TO MATCH YOUR PHYSICAL WIRING.
// Entry i corresponds to input "H{i+1:02}".
// ============================================================================

struct InputMapping {
    uint8_t sensorIndex;   // 0..TOUCH_SENSOR_COUNT-1
    uint8_t channel;       // 0..TOUCH_CHANNELS_PER_SENSOR-1
};

constexpr InputMapping INPUT_MAPPINGS[INPUT_COUNT] = {
    // H01..H07  -> sensor 0, channels 0..6
    {0,0}, {0,1}, {0,2}, {0,3}, {0,4}, {0,5}, {0,6},
    // H08..H14  -> sensor 1, channels 0..6
    {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, {1,5}, {1,6},
    // H15..H21  -> sensor 2, channels 0..6
    {2,0}, {2,1}, {2,2}, {2,3}, {2,4}, {2,5}, {2,6},
    // H22..H28  -> sensor 3, channels 0..6
    {3,0}, {3,1}, {3,2}, {3,3}, {3,4}, {3,5}, {3,6},
    // H29..H34  -> sensor 4, channels 0..5 (channel 6 of sensor 4 is unused)
    {4,0}, {4,1}, {4,2}, {4,3}, {4,4}, {4,5}
    // TODO (user): edit this default layout to match the real wiring.
};
```

#### 1e. Channel-enable mask (NEW)

Replace `CAP1188_CS1_BIT_MASK` usage with a per-sensor mask computed from `INPUT_MAPPINGS`:

```cpp
// Helper bitmask: bit n set => channel n enabled on the sensor
// Computed at runtime in TouchController::begin() from INPUT_MAPPINGS.
```

(Implementation lives in `TouchController.cpp`; no constant needed in Config.)

#### 1f. Bump protocol/firmware metadata
```cpp
#define FIRMWARE_VERSION "3.0.0"
#define PROTOCOL_VERSION "2"   // keep "2" because protocol semantics are unchanged
```

#### 1g. Buffer sizes

`SERIAL_LINE_MAX_LENGTH = 64` is still fine. Update `SENSOR_LIST_BUFFER_SIZE` because `SCANNED [...]` now lists up to 34 entries of 3 chars + commas:
```cpp
constexpr size_t SENSOR_LIST_BUFFER_SIZE = 160;  // 34 * 4 + slack
```

`EVENT_MESSAGE_BUFFER_SIZE = 96` is still enough for `TOUCHED H34 #4294967295\n` (≈22 chars). Keep.

---

### 2. [include/CommandController.h](include/CommandController.h) and [src/CommandController.cpp](src/CommandController.cpp)

#### 2a. `ParsedCommand` — change position storage
```cpp
struct ParsedCommand {
    CommandAction action;
    bool hasPosition;
    char position[POSITION_STRING_LENGTH];   // e.g. "H01\0"  (was: char position;)
    uint8_t positionIndex;                    // 0..33  (was 0..24)
    bool hasId;
    uint32_t id;
    uint8_t extraValue;
    uint8_t r, g, b, range;
    bool valid;
};
```

#### 2b. Replace the `charToIndex()` helper
Delete `charToIndex(char c)`. Add:
```cpp
// Parses "H01".."H34" (case-insensitive). Writes canonical "H01" form into outStr.
// Returns index 0..33 on success, 255 on failure.
static uint8_t parsePosition(const char* token, char outStr[POSITION_STRING_LENGTH]);

// Inverse: index -> "H01".."H34"
static void indexToPosition(uint8_t index, char outStr[POSITION_STRING_LENGTH]);
```

`parsePosition()` rules:
- Accept exactly 3 chars: leading `H` or `h`, then 2 ASCII digits.
- Numeric value must be in `1..34` (inclusive).
- Normalize the output to upper-case `H` + zero-padded 2 digits.

#### 2c. `parseLine()` — replace single-char position extraction
Old code:
```cpp
cmd.position = (*p >= 'a' && *p <= 'z') ? (*p - 32) : *p;
cmd.positionIndex = charToIndex(cmd.position);
...
p = skipWhitespace(p + 1);
```
New behavior:
- Read the next whitespace-delimited token via `findTokenEnd()`.
- Pass it to `parsePosition()`. If it returns 255 → queue `ERR unknown_position`.
- Advance `p` past the full token.

#### 2d. `executeInstant()` — change every `event.position` argument
Every call like:
```cpp
m_eventQueue.queueAck(actionStr, cmd.position, cmdId);
```
must pass the **string** form. Easiest path:
1. Change `EventQueue::queueXxx(... char position ...)` signatures to take `const char* position`.
2. Update all call sites accordingly.

Where the current code passes `0` to mean "no position", pass `nullptr` or `""` instead.

#### 2e. `queueCommand()` / `tickCommand()`
Same string-position propagation in the long-running command paths.

---

### 3. [include/EventQueue.h](include/EventQueue.h) and [src/EventQueue.cpp](src/EventQueue.cpp)

#### 3a. `Event` struct
```cpp
struct Event {
    EventType type;
    char action[16];
    char position[POSITION_STRING_LENGTH];   // was: char position;
    uint32_t commandId;
    char extra[52];
    bool valid;
};
```

#### 3b. All `queueXxx()` signatures
Change `char position` → `const char* position` (default `nullptr`) for:
`queueAck`, `queueDone`, `queueTouched`, `queueTouchReleased`, `queueRecalibrated`, `queueValue`.

Inside each, store the string into `event.position`:
```cpp
if (position && position[0]) {
    strncpy(event.position, position, POSITION_STRING_LENGTH - 1);
    event.position[POSITION_STRING_LENGTH - 1] = '\0';
} else {
    event.position[0] = '\0';
}
```

#### 3c. `sendEvent()` — change formatting

Old `snprintf(... " %c", event.position)` → new `snprintf(... " %s", event.position)`.

For `RECALIBRATED ALL`, key off `event.position[0] == '\0'` instead of `event.position == 0`.

Example block (for `TOUCHED`):
```cpp
case EventType::TOUCHED:
    length = snprintf(buffer, sizeof(buffer), "TOUCHED %s", event.position);
    break;
```

Keep all keyword strings (`ACK`, `DONE`, `ERR`, `BUSY`, `TOUCHED`, `TOUCH_RELEASED`, `SCANNED`, `RECALIBRATED`, `INFO`, `VALUE`) **exactly the same** — protocol must not change.

---

### 4. [include/TouchController.h](include/TouchController.h) and [src/TouchController.cpp](src/TouchController.cpp)

This file changes the most. Concept shift: **the controller now indexes by INPUT (0..33), not by sensor (0..4)**. Sensor state is stored once per physical sensor; per-input touch/debounce state is stored 34 times.

#### 4a. Reshape internal state
```cpp
// Per physical sensor
struct PhysicalSensor {
    bool active;          // I2C init succeeded
    uint8_t enableMask;   // OR of (1 << channel) for every input mapped to this sensor
    uint8_t lastStatus;   // Cached last-read CAP1188 SENSOR_INPUT_STATUS register
};

// Per logical input (H01..H34)
struct TouchInputState {
    bool currentTouched;
    bool debouncedTouched;
    bool lastReportedTouched;
    uint32_t lastChangeTime;
};

PhysicalSensor m_sensors[TOUCH_SENSOR_COUNT];     // size 5
TouchInputState m_inputs[INPUT_COUNT];            // size 34
ExpectState m_expectDown[INPUT_COUNT];
ExpectState m_expectUp[INPUT_COUNT];
bool m_expectAnyUsed[INPUT_COUNT];
```

#### 4b. `begin()`
1. Initialize Wire as before.
2. For each `INPUT_MAPPINGS[i]`, OR `(1 << channel)` into `m_sensors[sensorIndex].enableMask`.
3. For each physical sensor `s` in `0..TOUCH_SENSOR_COUNT-1`:
   - Probe the I2C address; verify product ID `0x50`.
   - Write `CAP1188_REG_MULTIPLE_TOUCH_CONFIG = 0x00` (allow multi-touch).
   - Write `CAP1188_REG_STANDBY_CONFIG = 0x30`.
   - Write `CAP1188_REG_SENSOR_INPUT_ENABLE = m_sensors[s].enableMask` (instead of `CAP1188_CS1_BIT_MASK`).
   - Mark `active = true` on success.
4. `m_activeSensorCount` = number of sensors successfully initialized.

#### 4c. `pollSensors()`
Now iterates over **inputs**, not sensors. For each input `i`:
1. Look up `(s, ch) = INPUT_MAPPINGS[i]`.
2. If `!m_sensors[s].active`, skip.
3. **Optimization:** read each sensor's `CAP1188_REG_SENSOR_INPUT_STATUS` register **once per poll cycle** and cache in `m_sensors[s].lastStatus`. Use a "stamped" flag or just read it lazily at the top of the loop for the first input mapped to that sensor. Simpler implementation: do a small preliminary loop reading all 5 sensors' status registers first, then iterate inputs using the cached values.
4. `touched = (m_sensors[s].lastStatus >> ch) & 0x01`.
5. Apply the same `currentTouched` / debounce logic as before, but on `m_inputs[i]`.

After a poll cycle, **clear the INT (main-control) bit on each sensor that reported any touch** — same as today's `readRawTouch()` but at the sensor level, not per-channel:
```cpp
if (m_sensors[s].lastStatus != 0) {
    uint8_t mc;
    if (readRegister(addr, CAP1188_REG_MAIN_CONTROL, mc)) {
        writeRegister(addr, CAP1188_REG_MAIN_CONTROL, mc & ~0x01);
    }
}
```

#### 4d. `processDebounce()`
Replace `m_sensors[i]` with `m_inputs[i]`. Replace `indexToLetter(i)` with `indexToPosition(i, buf)`. Change event-queue calls to pass the string.

#### 4e. `recalibrate(uint8_t inputIndex)`
CAP1188's `CALIBRATION_ACTIVE` register is per-sensor (one bit per channel). To recalibrate a single input:
```cpp
auto m = INPUT_MAPPINGS[inputIndex];
return writeRegister(SENSOR_I2C_ADDRESSES[m.sensorIndex],
                     CAP1188_REG_CALIBRATION_ACTIVE,
                     (uint8_t)(1 << m.channel));
```

`recalibrateAll()`: for each active sensor `s`, write its full `enableMask` to `CAP1188_REG_CALIBRATION_ACTIVE`.

#### 4f. `setSensitivity(uint8_t inputIndex, uint8_t level)`
The CAP1188 sensitivity register is **global per sensor** (bits 6:4), not per-channel. Behavior: setting sensitivity on input `H05` sets it for the entire sensor `H05` lives on. Document this in a comment. Use `INPUT_MAPPINGS[inputIndex].sensorIndex` to find the address.

#### 4g. `readSensorValue(uint8_t inputIndex, int8_t& value)`
The CAP1188 has eight delta registers `CAP1188_REG_SENSOR_INPUT_DELTA_1 + ch` (`0x10..0x17`). Read from address `0x10 + channel`:
```cpp
auto m = INPUT_MAPPINGS[inputIndex];
return readRegister(SENSOR_I2C_ADDRESSES[m.sensorIndex],
                    CAP1188_REG_SENSOR_INPUT_DELTA_1 + m.channel,
                    rawValue);
```

#### 4h. `buildActiveSensorList(char* buf, size_t n)`
Now lists **all active INPUTS** as `H01,H02,...`, NOT physical sensors. Iterate `0..INPUT_COUNT-1`; an input is "active" if its parent sensor is active. Output comma-separated `H01`-style tokens. Increase callers' buffer to `SENSOR_LIST_BUFFER_SIZE` (160).

#### 4i. `isSensorActive(uint8_t index)` / `isTouched(uint8_t index)`
Index now means input (0..33). Implement via the input array (and parent-sensor `active` flag).

#### 4j. `getActiveSensorCount()`
Two reasonable interpretations exist; pick the one that matches the startup-handshake check (`getActiveSensorCount() >= TOUCH_SENSOR_COUNT`):
- Keep its meaning = number of **physical sensors** initialized successfully.
- Leave [src/StartupController.cpp](src/StartupController.cpp)'s `>= TOUCH_SENSOR_COUNT` check unchanged.

#### 4k. Remove the old `letterToIndex` / `indexToLetter` static helpers
Replace with the new string-based helpers (or have `TouchController` delegate to the ones in `CommandController`). Keeping a single canonical implementation in [include/CommandController.h](include/CommandController.h) and re-using it everywhere is cleaner.

---

### 5. [include/LedController.h](include/LedController.h) and [src/LedController.cpp](src/LedController.cpp)

#### 5a. Size changes
- `m_positions[LED_POSITION_COUNT]` automatically grows to 34 because the constant changed.
- Update the docblock from "25 logical LED positions (A-Y)" to "34 logical LED positions (H01–H34)".

#### 5b. Mapping table
Replace `LED_MAPPINGS[25]` with a **34-entry placeholder table**:
```cpp
static const LedMapping LED_MAPPINGS[LED_POSITION_COUNT] = {
    // TODO (user): set the correct {strip, ledIndex} for each input H01..H34.
    // Placeholder values below are safe (point to LED 0 of strip 1) but will all
    // light up the same physical LED until you fill in the real wiring.
    {StripId::STRIP1, 0},   // H01
    {StripId::STRIP1, 0},   // H02
    // ... 32 more identical placeholder lines ...
};
```

#### 5c. Public helpers
Remove or repurpose `charToPosition(char c)` / `positionToChar(uint8_t pos)`. They are not directly used by the protocol (`CommandController` already does parsing) — verify with a workspace search and either delete them or convert them to the new string form (`parsePosition` / `indexToPosition`).

The numeric position index passed into `show()`, `hide()`, `success()`, etc. is unchanged in TYPE (`uint8_t`), only the valid range grows from `0..24` → `0..33`. No further changes to method bodies are required.

---

### 6. [src/main.cpp](src/main.cpp)

No structural changes. Sanity-check that the touch task still starts correctly and that there is no leftover reference to old constants.

---

### 7. [src/StartupController.cpp](src/StartupController.cpp)

- `getActiveSensorCount() >= TOUCH_SENSOR_COUNT` — keep as-is (we redefined the meaning of `getActiveSensorCount` to remain "physical sensors initialized").
- `buildActiveSensorList()` now produces a longer string of `H01,H02,...` form. Bump `sensorList[SENSOR_LIST_BUFFER_SIZE]` to the new constant value.
- The handshake messages `SENSORS READY` / `SENSORS FAILED [...]` and the `ACK ...` matcher are protocol — leave their text exactly as-is.

---

## Migration checklist (in execution order)

1. **[include/Config.h](include/Config.h)** — Update counts, addresses, add `InputMapping` + `INPUT_MAPPINGS[34]`, add `POSITION_STRING_LENGTH`, bump `SENSOR_LIST_BUFFER_SIZE`, set `LED_POSITION_COUNT = 34`, bump `FIRMWARE_VERSION`.
2. **[include/CommandController.h](include/CommandController.h)** — Change `ParsedCommand::position` to `char[4]`; declare `parsePosition()` / `indexToPosition()`; remove `charToIndex()`.
3. **[include/EventQueue.h](include/EventQueue.h)** — Change `Event::position` to `char[4]`; change all `queueXxx()` signatures from `char position` to `const char* position`.
4. **[src/EventQueue.cpp](src/EventQueue.cpp)** — Update enqueue helpers to copy string; update `sendEvent()` formatting from `%c` to `%s`; gate `RECALIBRATED ALL` on `position[0] == '\0'`.
5. **[src/CommandController.cpp](src/CommandController.cpp)** — Implement `parsePosition()` / `indexToPosition()`; rewrite position parsing in `parseLine()`; update every event-queue call to pass strings.
6. **[include/TouchController.h](include/TouchController.h)** — Split sensor/input state structs; resize arrays to `INPUT_COUNT`; drop `letterToIndex` / `indexToLetter`.
7. **[src/TouchController.cpp](src/TouchController.cpp)** — Rewrite `begin()` to compute `enableMask` per sensor and enable all channels at once; rewrite `pollSensors()` / `processDebounce()` to iterate inputs; update `recalibrate*`, `setSensitivity`, `readSensorValue`, `buildActiveSensorList` to use `INPUT_MAPPINGS`. Cache per-sensor status register reads to avoid 34× I²C transactions per poll.
8. **[src/LedController.cpp](src/LedController.cpp)** — Replace the 25-entry `LED_MAPPINGS` with the 34-entry placeholder table; update the docblock; update or delete unused `charToPosition`/`positionToChar` helpers.
9. **[src/StartupController.cpp](src/StartupController.cpp)** — Verify `sensorList` buffer is `SENSOR_LIST_BUFFER_SIZE`; no other change.
10. **Compile** (`pio run`) and fix any references the agent missed (use grep for `'A'`, `'Y'`, `A-Y`, `letterToIndex`, `indexToLetter`, `charToIndex`, `CAP1188_CS1_BIT_MASK`, `event.position == 0`, `%c` in event formatting).
11. **Manual verification with the Pi** — send `PING`, `INFO`, `SCAN`, `SHOW H05`, `EXPECT H12`, `RECALIBRATE_ALL`, etc. Confirm responses use `H01`–`H34` and nothing else changed.

## Acceptance criteria

- [ ] `pio run` compiles with no errors and no warnings about narrowing conversions on positions.
- [ ] `SCAN` returns `SCANNED [H01,H02,...,H34]` (all 34 if wiring is correct).
- [ ] `SHOW H05` / `HIDE H05` / `SUCCESS H05` / `EXPECT H05` are accepted; `ACK`, `DONE`, `TOUCHED` etc. echo back as `H05`.
- [ ] Invalid positions like `Z`, `H00`, `H35`, `H99`, single-char `A` produce `ERR unknown_position`.
- [ ] Touching channel 3 of sensor 1 (= `H11` in the default mapping) produces `TOUCHED H11` when an `EXPECT H11` is active.
- [ ] `VALUE H11` returns the delta from the correct channel.
- [ ] `RECALIBRATE H11` only recalibrates channel 3 of sensor 1, not the whole sensor.
- [ ] `RECALIBRATE_ALL` recalibrates every enabled channel on every active sensor.
- [ ] `SET_SENSITIVITY H11 3` writes the global sensitivity register on sensor 1 (document the per-sensor scope in a comment).
- [ ] Startup handshake text (`SENSORS READY` / `SENSORS FAILED [...]` / `HARDWARE INITIALISED`) is **byte-for-byte identical** to before.
- [ ] All other protocol keywords (`ACK`, `DONE`, `ERR`, `BUSY`, `TOUCHED`, `TOUCH_RELEASED`, `SCANNED`, `RECALIBRATED`, `VALUE`, `INFO`) are **byte-for-byte identical**.
- [ ] Long-running command flow (`ACK` immediately, `DONE` later, `BUSY` on overflow) is unchanged.

## Things explicitly NOT changed by this migration

- UART baud rate, line terminator, buffer sizes (except `SENSOR_LIST_BUFFER_SIZE`).
- FreeRTOS task layout (Core 0 = touch, Core 1 = main loop).
- LED animation algorithms, colors, timing.
- EventQueue mutex strategy.
- The set of command keywords or response keywords.
- The `#<id>` command-ID mechanism.
- The number of physical LED strips, their pins, or their lengths.
