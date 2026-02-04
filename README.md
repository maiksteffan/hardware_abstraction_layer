# ESP32 LED & Touch Controller - Serial Protocol

Serial interface for controlling LEDs and reading touch sensors from a Raspberry Pi.

## Connection

| Setting | Value |
|---------|-------|
| Baud Rate | 115200 |
| Format | 8N1 |
| Line Ending | `\n` |

**Startup**: Wait for `READY` message before sending commands.

## Command Format

```
COMMAND [position] [params] [#id]
```

- **position**: Letter `A`-`Y` (25 positions)
- **#id**: Optional ID returned in response for correlation

## Commands

### LED Control

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| SHOW | `SHOW A [#id]` | `ACK SHOW A` | Turn on LED (blue) |
| HIDE | `HIDE A [#id]` | `ACK HIDE A` | Turn off LED |
| HIDE_ALL | `HIDE_ALL [#id]` | `ACK HIDE_ALL` | Turn off all LEDs |
| SUCCESS | `SUCCESS A [#id]` | `ACK` → `DONE SUCCESS A` | Green expansion animation |
| FAIL | `FAIL A [#id]` | `ACK FAIL A` | Red LED (error) |
| CONTRACT | `CONTRACT A [#id]` | `ACK` → `DONE CONTRACT A` | Contract to center |
| BLINK | `BLINK A [#id]` | `ACK BLINK A` | Start blinking (green) |
| STOP_BLINK | `STOP_BLINK A [#id]` | `ACK STOP_BLINK A` | Stop blinking |
| EXPAND_STEP | `EXPAND_STEP A [#id]` | `ACK EXPAND_STEP A` | Expand by 1 LED each side |
| CONTRACT_STEP | `CONTRACT_STEP A [#id]` | `ACK CONTRACT_STEP A` | Shrink by 1 LED each side |
| SEQUENCE_COMPLETED | `SEQUENCE_COMPLETED [#id]` | `ACK` → `DONE` | Celebration animation |
| MENUE_CHANGE | `MENUE_CHANGE r,g,b range` | `ACK MENUE_CHANGE` | Color sweep (e.g. `255,0,0 50`) |

### Touch Sensing

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| EXPECT | `EXPECT A [#id]` | `ACK` → `TOUCHED A` | Wait for touch |
| EXPECT_RELEASE | `EXPECT_RELEASE A [#id]` | `ACK` → `TOUCH_RELEASED A` | Wait for release |
| RECALIBRATE | `RECALIBRATE A [#id]` | `ACK` → `RECALIBRATED A` | Recalibrate sensor |
| RECALIBRATE_ALL | `RECALIBRATE_ALL [#id]` | `ACK` → `RECALIBRATED ALL` | Recalibrate all |
| VALUE | `VALUE A [#id]` | `VALUE A <delta>` | Get delta (-128 to 127) |
| SET_SENSITIVITY | `SET_SENSITIVITY A <lvl>` | `ACK SET_SENSITIVITY A` | Set sensitivity (0-7) |

### System

| Command | Syntax | Response |
|---------|--------|----------|
| PING | `PING [#id]` | `ACK PING` |
| INFO | `INFO [#id]` | `INFO firmware=2.3.0 protocol=2 board=ESP32_WROOM` |
| SCAN | `SCAN [#id]` | `SCANNED [A,B,C,...]` |

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
ser.readline()  # Wait for READY

ser.write(b'SHOW A #1\n')       # Turn on LED
ser.write(b'EXPECT A #2\n')     # Wait for touch
# → TOUCHED A #2
ser.write(b'SUCCESS A #3\n')    # Play success animation
# → DONE SUCCESS A #3
ser.write(b'HIDE A #4\n')       # Turn off
```

## Timing

| Parameter | Value |
|-----------|-------|
| Touch debounce | 100ms |
| Animation step | 25ms |
| Blink interval | 150ms |

---

*Firmware v2.3.0 · Protocol v2 · ESP32 WROOM*
