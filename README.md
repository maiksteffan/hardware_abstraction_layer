# ESP32 LED & Touch Controller - Serial Protocol

Serial interface for controlling LEDs and reading touch sensors from a Raspberry Pi.

## Connection

| Setting | Value |
|---------|-------|
| Baud Rate | 115200 |
| Format | 8N1 |
| Line Ending | `\n` |


## Command Format

```
COMMAND [position] [params] [#id]
```

- **position**: Input token `H01`-`H34` (34 logical inputs)
- **#id**: Optional ID returned in response for correlation

### Input mapping

The 34 logical inputs (`H01`-`H34`) are spread across multiple CAP1188 chips,
each exposing several channels. The mapping from each input to its physical
`(sensor_index, channel)` is defined by the `INPUT_MAPPINGS[]` table in
[include/Config.h](include/Config.h) and can be edited to match the wiring.

## Startup Sequence

On power-up the ESP32 initializes hardware and performs a handshake with the
Raspberry Pi **before** any FreeRTOS tasks are created. The handshake ensures
both sides agree on hardware state before normal operation begins.

### Protocol

If all sensors are detected:

```
ESP → Pi:   SENSORS READY                  (repeats every 3s until ACK)
Pi  → ESP:  ACK SENSORS READY
ESP → Pi:   HARDWARE INITIALISED
```

If some sensors are missing:

```
ESP → Pi:   SENSORS FAILED [H01,H02,H03]   (repeats every 3s until ACK)
Pi  → ESP:  ACK SENSORS FAILED
ESP → Pi:   HARDWARE INITIALISED
```

Both sides should repeat: the ESP repeats its status message every 3 seconds,
and the Pi should re-send its ACK if it doesn't see `HARDWARE INITIALISED`
within a reasonable timeout.

LED status is not reported because NeoPixel strips provide no electrical
feedback — the startup animation (a white pixel sweep across every LED) serves
as visual-only confirmation.

## Commands

### LED Control

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| SHOW | `SHOW H01 [#id]` | `ACK SHOW H01` | Turn on LED (blue) |
| HIDE | `HIDE H01 [#id]` | `ACK HIDE H01` | Turn off LED |
| HIDE_ALL | `HIDE_ALL [#id]` | `ACK HIDE_ALL` | Turn off all LEDs |
| SUCCESS | `SUCCESS H01 [#id]` | `ACK` → `DONE SUCCESS H01` | Green expansion animation |
| FAIL | `FAIL H01 [#id]` | `ACK FAIL H01` | Red LED (error) |
| CONTRACT | `CONTRACT H01 [#id]` | `ACK` → `DONE CONTRACT H01` | Contract to center |
| BLINK | `BLINK H01 [#id]` | `ACK BLINK H01` | Start blinking (green) |
| STOP_BLINK | `STOP_BLINK H01 [#id]` | `ACK STOP_BLINK H01` | Stop blinking |
| EXPAND_STEP | `EXPAND_STEP H01 [#id]` | `ACK EXPAND_STEP H01` | Expand by 1 LED each side |
| CONTRACT_STEP | `CONTRACT_STEP H01 [#id]` | `ACK CONTRACT_STEP H01` | Shrink by 1 LED each side |
| SEQUENCE_COMPLETED | `SEQUENCE_COMPLETED [#id]` | `ACK` → `DONE` | Celebration animation |
| MENUE_CHANGE | `MENUE_CHANGE r,g,b range` | `ACK MENUE_CHANGE` | Color sweep (e.g. `255,0,0 50`) |
| PLAY_RECORD_ANIMATION | `PLAY_RECORD_ANIMATION [#id]` | `ACK PLAY_RECORD_ANIMATION` | Blink all positions dim white until stopped |
| STOP_RECORD_ANIMATION | `STOP_RECORD_ANIMATION [#id]` | `ACK STOP_RECORD_ANIMATION` | Stop record blink and hide all LEDs |

### Touch Sensing

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| EXPECT | `EXPECT H01 [#id]` | `ACK` → `TOUCHED H01` | Wait for touch |
| EXPECT_ANY | `EXPECT_ANY [#id]` | `ACK` → `TOUCHED <pos>` | Wait for any touch (first hit) |
| EXPECT_ANY_EXCEPT | `EXPECT_ANY_EXCEPT [H## H## ...] [#id]` | `ACK` → `TOUCHED <pos>` | Wait for any touch except the listed inputs |
| EXPECT_RELEASE | `EXPECT_RELEASE H01 [#id]` | `ACK` → `TOUCH_RELEASED H01` | Wait for release |
| RECALIBRATE | `RECALIBRATE H01 [#id]` | `ACK` → `RECALIBRATED H01` | Recalibrate sensor |
| RECALIBRATE_ALL | `RECALIBRATE_ALL [#id]` | `ACK` → `RECALIBRATED ALL` | Recalibrate all |
| VALUE | `VALUE H01 [#id]` | `VALUE H01 <delta>` | Get delta (-128 to 127) |
| SET_SENSITIVITY | `SET_SENSITIVITY H01 <lvl>` | `ACK SET_SENSITIVITY H01` | Set sensitivity (0-7) |
| CLEAN_QUEUE | `CLEAN_QUEUE [#id]` | `ACK CLEAN_QUEUE` | Clear all pending expectations |

`EXPECT_ANY_EXCEPT` behaves exactly like `EXPECT_ANY` but ignores any inputs
listed after the keyword. With no inputs listed it is equivalent to
`EXPECT_ANY`. Example: `EXPECT_ANY_EXCEPT H01 H03 H17 #7` fires `TOUCHED <pos>`
for the first touch on any input other than `H01`, `H03`, or `H17`.

> **Note:** CAP1188 sensitivity is global per chip, so `SET_SENSITIVITY` on one
> input affects every other input mapped to the same physical sensor.

### System

| Command | Syntax | Response |
|---------|--------|----------|
| PING | `PING [#id]` | `ACK PING` |
| INFO | `INFO [#id]` | `INFO firmware=2.3.0 protocol=3 board=ESP32_WROOM` |
| SCAN | `SCAN [#id]` | `SCANNED [H01,H02,H03,...]` |

## Responses

| Response | Meaning |
|----------|---------|
| `ACK <cmd> [pos] [#id]` | Command accepted |
| `DONE <cmd> [pos] [#id]` | Animation complete |
| `TOUCHED <pos> [#id]` | Touch detected |
| `TOUCH_RELEASED <pos> [#id]` | Release detected |
| `BUSY [#id]` | Queue full, retry later |
| `ERR <reason> [#id]` | Command failed |

### Errors

`bad_format` · `unknown_action` · `unknown_position` · `sensor_inactive` · `invalid_level`

## Example

```python
import serial

ser = serial.Serial('/dev/ttyUSB0', 115200)

# Turn on LED at position H01
ser.write(b'SHOW H01 #1\n')
print(ser.readline())  # → ACK SHOW H01 #1

# Wait for user to touch sensor H01
ser.write(b'EXPECT H01 #2\n')
print(ser.readline())  # → ACK EXPECT H01 #2
print(ser.readline())  # → TOUCHED H01 #2  (when user touches)

# Wait for any touch except H01 and H03
ser.write(b'EXPECT_ANY_EXCEPT H01 H03 #3\n')
print(ser.readline())  # → ACK EXPECT_ANY_EXCEPT #3
print(ser.readline())  # → TOUCHED H05 #3  (first non-excluded touch)

# Play success animation
ser.write(b'SUCCESS H01 #4\n')
print(ser.readline())  # → ACK SUCCESS H01 #4
print(ser.readline())  # → DONE SUCCESS H01 #4  (when animation completes)

# Turn off LED
ser.write(b'HIDE H01 #5\n')
print(ser.readline())  # → ACK HIDE H01 #5
```

## Timing

| Parameter | Value |
|-----------|-------|
| Touch debounce | 100ms |
| Animation step | 25ms |
| Blink interval | 150ms |

---

*Firmware v2.3.0 · Protocol v3 · ESP32 WROOM*
