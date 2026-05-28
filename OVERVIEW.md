# Project Overview — ESP32 Hardware Abstraction Layer

This firmware turns a **Freenove ESP32 WROOM** into a dumb, reliable
**hardware executor** that sits between a **Raspberry Pi 5** (which holds all
game logic) and the physical hardware (two NeoPixel LED strips + five CAP1188
capacitive touch chips exposing 34 logical inputs `H01`–`H34`).

The ESP32 contains **no game logic**. It:

1. Listens for ASCII commands from the Pi over UART.
2. Drives the LEDs or reads the touch sensors.
3. Reports events (`TOUCHED`, `DONE`, `ACK`, etc.) back to the Pi.

If you only read one thing, read the [Communication](#communication-the-heart-of-the-system) section — it is the heart of the system.

---

## 1. High-level architecture

```
┌────────────────────┐   USB / UART 115200 8N1   ┌──────────────────────────┐
│   Raspberry Pi 5   │ ◄────────────────────────►│   ESP32 WROOM (dual core)│
│  (game logic)      │   ASCII, line-based, \n   │  (hardware executor)     │
└────────────────────┘                           └──────────────────────────┘
                                                   │            │
                                       Core 1 ─────┘            └───── Core 0
                                       Main loop:                      TouchPoll task:
                                       • serial RX/TX                  • CAP1188 over I2C
                                       • command parsing/exec          • debounce
                                       • LED animation tick            • emits TOUCHED events
                                       • event queue flush                via EventQueue
                                                   │            │
                                                   ▼            ▼
                                       ┌─────────────┐   ┌─────────────────┐
                                       │ 2× NeoPixel │   │ 5× CAP1188 I2C  │
                                       │ strips      │   │ (7 ch each → 34)│
                                       └─────────────┘   └─────────────────┘
```

### Dual-core split (FreeRTOS)

| Core   | Task               | Responsibility                                        |
|--------|--------------------|-------------------------------------------------------|
| Core 0 | `touchPollingTask` | Poll CAP1188 chips over I2C every 5 ms, debounce      |
| Core 1 | `loop()`           | Serial RX, command parsing/exec, LED animation tick, event flush |

LED animation runs on Core 1 *with* the command parser so that nothing else
touches the NeoPixel buffer (it isn’t thread-safe). The only thing on Core 0
is touch polling.

---

## 2. File / module map

| Module | Header / Source | Job |
|--------|-----------------|-----|
| Entry point     | [src/main.cpp](src/main.cpp) | `setup()` / `loop()`, FreeRTOS task creation |
| Configuration   | [include/Config.h](include/Config.h) | **All** tunables: pins, timings, buffer sizes, sensor I2C addresses, the `H01`–`H34` → `(sensor, channel)` mapping, colors |
| Startup         | [include/StartupController.h](include/StartupController.h), [src/StartupController.cpp](src/StartupController.cpp) | Boot-time hardware init + Pi handshake |
| Commands        | [include/CommandController.h](include/CommandController.h), [src/CommandController.cpp](src/CommandController.cpp) | Parses serial lines, executes instant commands, runs the queue for long-running ones |
| LEDs            | [include/LedController.h](include/LedController.h), [src/LedController.cpp](src/LedController.cpp) | NeoPixel strips, animations (`SHOW`, `SUCCESS`, `BLINK`, …) |
| Touch sensors   | [include/TouchController.h](include/TouchController.h), [src/TouchController.cpp](src/TouchController.cpp) | I2C poll, debounce, `EXPECT*` expectation tracking |
| Outgoing events | [include/EventQueue.h](include/EventQueue.h), [src/EventQueue.cpp](src/EventQueue.cpp) | Thread-safe queue → serial line formatter |

---

## 3. Configuration model

Everything tunable lives in [include/Config.h](include/Config.h). A few highlights:

- `INPUT_COUNT = 34` — number of logical touch inputs (`H01`–`H34`)
- `LED_POSITION_COUNT = 34` — number of logical LED positions
- `TOUCH_SENSOR_COUNT = 5`, `TOUCHCHANNELS_PER_SENSOR = 7`
- `SENSOR_I2C_ADDRESSES[] = {0x28, 0x29, 0x2A, 0x2B, 0x2C}`
- `INPUT_MAPPINGS[INPUT_COUNT]` — the **user-editable wiring table** that
  maps each `H01`…`H34` to a `(sensorIndex, channel)` pair. Edit this when
  you re-wire the hardware.
- `POSITION_STRING_LENGTH = 4` — every position string is 3 chars + `\0`
  (e.g. `"H07\0"`).

---

## 4. Boot sequence

1. `Serial.begin(115200)` with explicit RX/TX buffer sizes.
2. `eventQueue.begin()` — creates the FreeRTOS mutexes.
3. `touchController.setEventQueue(&eventQueue)`.
4. `startupController.run()` — **blocking**:
   1. LED strips initialized; a white pixel sweep plays as visual confirmation.
   2. Each CAP1188 is initialized over I2C, with up to `SENSOR_INIT_MAX_RETRIES = 3` retries.
   3. Status message is built: `SENSORS READY` or `SENSORS FAILED [H03,H17,…]`.
   4. **Handshake loop:** the ESP repeats the status line every 3 s until the Pi answers `ACK SENSORS READY` / `ACK SENSORS FAILED`. Then it sends `HARDWARE INITIALISED` and returns.
5. `commandController.begin()`.
6. `touchPollingTask` is pinned to Core 0.
7. `loop()` starts on Core 1.

This handshake guarantees both sides are in a known state before any
`SHOW`/`EXPECT`/etc. is sent.

---

## 5. Main loop

Five steps, executed forever on Core 1:

```cpp
void loop() {
    commandController.pollSerial();             // 1. drain UART into ring buffer
    commandController.processCompletedLines();  // 2. parse any complete '\n'-terminated lines and dispatch
    commandController.tick();                   // 3. step long-running commands (SUCCESS/CONTRACT/...)
    ledController.tick();                       // 4. step LED animations (NeoPixel updates)
    eventQueue.flush(EVENTS_PER_FLUSH);         // 5. send up to 5 queued events back to the Pi
    yield();
}
```

Meanwhile on Core 0, `touchPollingTask` calls `touchController.tick()` every
5 ms (`vTaskDelayUntil` for jitter-free timing). When a debounced press or
release matches an outstanding `EXPECT*`, the touch controller enqueues a
`TOUCHED` / `TOUCH_RELEASED` event into the **same** `EventQueue`. Core 1
drains it in step 5.

---

## 6. Communication (the heart of the system)

### 6.1 Wire-level

- UART over USB, **115200 8N1**, ASCII, line-based.
- Line terminator: `\n` (CR is tolerated).
- Max line length: `SERIAL_LINE_MAX_LENGTH = 64` chars.

### 6.2 Grammar

```
Pi → ESP :   <ACTION> [<pos>] [<extra>] [#<id>]
ESP → Pi :   <RESPONSE> [<ACTION>] [<pos>] [<extra>] [#<id>]
```

- `<pos>` is **always** the 3-character form `H01`…`H34` (case-insensitive on input, always upper-case on output).
- `#<id>` is an **optional correlation ID** chosen by the Pi. The ESP echoes it back on every response that originated from that command. This is what lets the Pi pair an asynchronous `DONE` or `TOUCHED` with the request that produced it.
- Tokens are separated by spaces. Order is fixed.

### 6.3 Two kinds of commands

**Instant commands** are executed inline inside `processCompletedLines()` and
produce a single `ACK` (sometimes also a data response like `VALUE` or `SCANNED`).

Examples: `SHOW`, `HIDE`, `HIDE_ALL`, `FAIL`, `BLINK`, `STOP_BLINK`, `EXPAND_STEP`, `CONTRACT_STEP`, `EXPECT`, `EXPECT_ANY`, `EXPECT_RELEASE`, `RECALIBRATE`, `RECALIBRATE_ALL`, `VALUE`, `SET_SENSITIVITY`, `SCAN`, `INFO`, `PING`.

**Long-running commands** are queued into `m_commandQueue[QUEUE_SIZE_COMMANDS = 32]`,
get an immediate `ACK`, are progressed every iteration by
`commandController.tick()`, and emit a final `DONE` when the animation
completes.

Long-running set: `SUCCESS`, `CONTRACT`, `MENUE_CHANGE`, `SEQUENCE_COMPLETED`.

If the command queue is full, the ESP responds with `BUSY` and the Pi is
expected to retry later.

### 6.4 Response vocabulary

| Response | When |
|----------|------|
| `ACK <ACTION> [<pos>] [#id]` | Command accepted |
| `DONE <ACTION> [<pos>] [#id]` | Long-running animation finished |
| `ERR <reason> [#id]` | Parse or execution failure (`bad_format`, `unknown_action`, `unknown_position`, `sensor_inactive`, `invalid_level`) |
| `BUSY [#id]` | Command queue full |
| `TOUCHED <pos> [#id]` | A debounced press matched an `EXPECT`/`EXPECT_ANY` |
| `TOUCH_RELEASED <pos> [#id]` | A debounced release matched an `EXPECT_RELEASE` |
| `SCANNED [<pos>,<pos>,…]` | Reply to `SCAN` |
| `RECALIBRATED <pos\|ALL> [#id]` | Reply to recalibrate commands |
| `VALUE <pos> <delta> [#id]` | Raw CAP1188 delta for that input |
| `INFO firmware=… protocol=… board=…` | Reply to `INFO` |

### 6.5 End-to-end flow: where each byte goes

A single command makes this round trip:

```
Pi writes "EXPECT H07 #42\n"
        │
        ▼
ESP32 UART RX ISR fills the hardware buffer
        │
        ▼
[Core 1] CommandController::pollSerial()
        – drains UART bytes into m_rxBuffer (ring buffer)
        │
        ▼
[Core 1] CommandController::processCompletedLines()
        – extractLine() pulls one '\n'-terminated line
        – parseLine() builds a ParsedCommand:
              action=EXPECT, position="H07", positionIndex=6, id=42
        – executeCommand() decides instant vs. long-running
        – executeInstant() calls touchController.setExpectDown(6, 42)
        – eventQueue.queueAck("EXPECT", "H07", 42)
        │
        ▼
[Core 1] eventQueue.flush() → Serial.print("ACK EXPECT H07 #42\n")

        ... time passes, user touches the pad ...

[Core 0] touchPollingTask → TouchController::tick()
        – I2C read of CAP1188 SENSOR_INPUT_STATUS
        – debounce (press fires INSTANTLY, release latches 800 ms)
        – sees expectDown[6] is active → eventQueue.queueTouched("H07", 42)
                                       → clears expectation
        │
        ▼
[Core 1] eventQueue.flush() → Serial.print("TOUCHED H07 #42\n")
        │
        ▼
Pi reads "TOUCHED H07 #42\n" and correlates by #42
```

The same path is used for `SUCCESS` etc., except `executeCommand()` puts the
command into `m_commandQueue`, immediately ACKs, and `tick()` advances the
animation each loop iteration until it finally enqueues `DONE`.

### 6.6 Thread safety

Two cores both write to the serial port:

- Core 0 wants to send `TOUCHED` events.
- Core 1 wants to send `ACK` / `DONE` / data replies.

To avoid interleaved bytes, **nothing writes to `Serial` directly**. Everything
goes through [EventQueue](include/EventQueue.h):

- `m_queueMutex` protects the ring buffer of `Event` records.
- `m_serialMutex` makes every full line atomic on the wire.
- `EventQueue::flush()` is only ever called from Core 1’s main loop, so even
  though the queue is filled from both cores, draining is single-threaded.

The Event struct itself is fixed-size so no heap allocation happens on the
hot path:

```cpp
struct Event {
    EventType type;
    char action[16];
    char position[POSITION_STRING_LENGTH];   // "H07\0"
    uint32_t commandId;
    char extra[160];   // big enough for "H01,H02,...,H34"
    bool valid;
};
```

### 6.7 Worked examples

**Light an LED, wait for the touch, celebrate:**

```
Pi → ESP :   SHOW H07 #1
ESP → Pi :   ACK SHOW H07 #1

Pi → ESP :   EXPECT H07 #2
ESP → Pi :   ACK EXPECT H07 #2
ESP → Pi :   TOUCHED H07 #2          ← arrives asynchronously from Core 0

Pi → ESP :   SUCCESS H07 #3
ESP → Pi :   ACK SUCCESS H07 #3
ESP → Pi :   DONE SUCCESS H07 #3     ← when the animation finishes

Pi → ESP :   HIDE H07 #4
ESP → Pi :   ACK HIDE H07 #4
```

**Scan which sensors actually came up:**

```
Pi → ESP :   SCAN #9
ESP → Pi :   SCANNED [H01,H02,H03,...,H34] #9
```

**Backpressure:**

```
Pi → ESP :   SUCCESS H05 #77
ESP → Pi :   BUSY #77                 ← command queue full, Pi should retry
```

**Equivalent in Python (see also [README.md](README.md)):**

```python
import serial
ser = serial.Serial('/dev/ttyUSB0', 115200)

ser.write(b'SHOW H07 #1\n');     print(ser.readline())  # ACK SHOW H07 #1
ser.write(b'EXPECT H07 #2\n');   print(ser.readline())  # ACK EXPECT H07 #2
print(ser.readline())                                   # TOUCHED H07 #2
ser.write(b'SUCCESS H07 #3\n');  print(ser.readline())  # ACK SUCCESS H07 #3
print(ser.readline())                                   # DONE SUCCESS H07 #3
```

---

## 7. Touch pipeline in detail

1. **Polling** (Core 0, every 5 ms): for each *active* CAP1188 chip, read
   `SENSOR_INPUT_STATUS` once per cycle and cache it.
2. **Per-input update**: for each logical input `H01`…`H34`, look up
   `INPUT_MAPPINGS[i] = (sensorIndex, channel)`; the “touched” bit is
   `(cachedStatus >> channel) & 1`.
3. **Asymmetric debounce**:
   - **Press**: fires on the first sample (`TOUCH_DEBOUNCE_PRESS_MS = 0`) — sub-poll-cycle response.
   - **Release**: only fires after the sensor reads *untouched* continuously for `TOUCH_DEBOUNCE_RELEASE_MS = 800` ms. Any transient re-touch resets the timer, so brief dropouts don’t look like a release.
4. **Expectation matching**: per-input `m_expectDown[]` / `m_expectUp[]` plus a small ring buffer for `EXPECT_ANY`. When a debounced edge matches, the controller enqueues the corresponding event with the original `#id` and clears the expectation.

> Sensitivity is a **per-chip** setting. `SET_SENSITIVITY H07 5` therefore
> also affects every other input mapped to the same CAP1188 chip as `H07`.

---

## 8. LED pipeline in detail

- Two `Adafruit_NeoPixel` strips on `PIN_LED_STRIP_1 = 18` and
  `PIN_LED_STRIP_2 = 19`, lengths `LED_STRIP_1_LENGTH` / `LED_STRIP_2_LENGTH`
  (default 260 each).
- Each logical position `H01`–`H34` has a `PositionData` record (state,
  animation step, blink phase, expansion radius).
- A `LedMapping` table (in [src/LedController.cpp](src/LedController.cpp))
  maps each logical position to `(strip, ledIndex)`. Animations like
  `SUCCESS` expand outward from that index by `LED_SUCCESS_EXPANSION_RADIUS`.
- `LedController::tick()` is called every main-loop iteration; it pushes a
  new frame to the strips only if `m_needsUpdate` is set (the NeoPixel
  `show()` call is the expensive one, so we batch).
- Long-running animations (`SUCCESS`, `CONTRACT`, `MENUE_CHANGE`,
  `SEQUENCE_COMPLETED`) advance one step per `LED_ANIMATION_STEP_MS = 25` ms.
  When the LED side reports the animation complete, the matching
  `QueuedCommand` in the command controller emits `DONE`.

---

## 9. Error handling cheatsheet

| Situation | ESP responds |
|-----------|--------------|
| Malformed line | `ERR bad_format [#id]` |
| Unknown verb | `ERR unknown_action [#id]` |
| Position not `H01`–`H34` | `ERR unknown_position [#id]` |
| Targeted input’s CAP1188 didn’t init | `ERR sensor_inactive [#id]` |
| `SET_SENSITIVITY` level outside 0–7 | `ERR invalid_level [#id]` |
| Command queue (32) full | `BUSY [#id]` |
| Line exceeds 64 chars | line is dropped, next `\n` re-syncs |

---

## 10. Quick mental model

> The Pi sends a line. Core 1 parses it, either does it now and ACKs, or
> queues it, ACKs, and finishes it later with a DONE. Meanwhile, Core 0
> watches the touch chips and drops `TOUCHED` / `TOUCH_RELEASED` lines into
> the same outgoing queue whenever the Pi had asked it to. The `#id`
> stitches asynchronous replies back to the requests that caused them.

That single paragraph is the whole protocol.
