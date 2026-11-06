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
  Fresh grabs are tracked even between `EXPECT_ANY` commands, so releasing
  one hold and grabbing the next before the Pi re-arms does not lose the
  next touch. Unconsumed grabs recover from brief sensor dropouts without
  requiring a full release. The release latch remains 500 ms; it does not
  block another hold's touch. Reusing the **same** consumed hold still
  requires a full debounced release and re-grab.
  Single-hold expectations are evaluated on every sensor tick, avoiding the
  command/press registration race. Confirmed touch/release reports are retried
  if the outgoing queue is temporarily full or its mutex is unavailable;
  `CLEAN_QUEUE` cancels those pending reports too.
  Polling-core event submissions never wait for the queue mutex. Failed I2C
  status reads pause release evidence instead of restarting the whole latch;
  periodic errors therefore cannot keep an otherwise released hold consumed
  forever. Recording delta reads are skipped when no `EXPECT_ANY` is armed.
- **LED engine:** hold→strip mapping with mirrors, solid states and
  non-blocking animations (success pulse, victory/defeat, recording glow).
- **Snake overlay:** both strips chained into one track; `TRACK` reports its
  length and where every hold sits on it, `SNAKE <start> <len> [r,g,b]` paints
  a span of it on top of the holds and `SNAKE_OFF` removes it. The Snake game
  itself runs on the Pi; `INFO` announces support as `features=snake`.
- **Hold-time gauge:** `HOLD_GAUGE <pos> <0-100>` lights a hold's green block
  from the center outward, so a workout can show how much longer the hold must
  stay gripped. The Pi owns the clock and ticks the percentage down; `INFO`
  announces support as `features=holdgauge`.
- **Hold-time release filtering:** countdown code must wait for the debounced
  `TOUCH_RELEASED` event from `EXPECT_RELEASE`, not reset on a raw negative
  sensor reading. The existing release filter requires 500 ms of observed
  no-touch evidence; every valid touch reading resets that evidence. Separate
  short dropouts never add up to a release. Failed I2C reads contribute no
  evidence. Host regressions cover repeated dropouts, arming during a dropout,
  and independent release detection for double-hold steps.
- **Boot handshake:** LED sweep → sensor init → `SENSORS READY` →
  `HARDWARE INITIALISED`, with `DIAG` output for troubleshooting.

| Module | Role |
|---|---|
| `CommandController` | Serial command parsing & execution |
| `EventQueue` | Thread-safe outgoing message queue |
| `LedController` | Position→LED mapping, all animations, snake overlay |
| `SnakeTrack.h` | Track math for `TRACK`/`SNAKE` (pure, host-tested) |
| `HoldGauge.h` | Gauge geometry for `HOLD_GAUGE` (pure, host-tested) |
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
pio test -e native     # host tests, no hardware
```

Native tests cover board profiles, track math and the real touch controller
with simulated CAP1188 registers and time (handoffs, rearm gaps, dropouts,
one-event-per-grab, exclusions, release timing and occupancy transitions).

### Diagnosing an ignored hold

Before recalibrating, send `TOUCH_STATE H05 #123` over the existing serial
connection. It replies `ACK TOUCH_STATE H05 #123`, followed by one
`TOUCH_STATE H05 active=... valid=... raw=... held=... used=... age_ms=... release_ms=... expect=... pending=... #123` line.
Use the affected hold number. The snapshot is built by the polling core,
does not read extra I2C registers, and does not change expectations or touch state.

- `valid=0`: that poll's sensor-status read failed (`raw` is cached).
- `valid=1 raw=0`: the chip did not report contact on that poll.
- `held=1 used=1`: the previous grab is still consumed; observe the release evidence.
- `expect=0`: no single-hold press expectation is armed.
- `expect=1 pending=1`: a touch was detected and is awaiting queue delivery.
- `age_ms>2000` on an unconsumed held input: too old to satisfy a newly armed `EXPECT`.

Only one diagnostic query should be outstanding at a time; a new query replaces
an undelivered snapshot request. Stop the Pi's serial connection before opening
a separate monitor—two readers on one serial port would invalidate the diagnosis.

## Updates in the field

The Pi's admin panel flashes new firmware automatically: it downloads the
`firmware-merged.bin` asset from this repo's latest GitHub release, writes it
with `esptool`, and verifies the version via the `INFO` command. Bump
`FIRMWARE_VERSION` in `include/Config.h` to match the release tag.
