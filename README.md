# LED & Touch Controller - Raspberry Pi Implementation Manual

This document describes the serial protocol for communicating with the ESP32 LED & Touch Controller. Use this manual to implement the game logic on a Raspberry Pi.

## Hardware

### Target Board: Freenove ESP32 WROOM

| Specification | Value |
|--------------|-------|
| CPU | Dual-core Xtensa LX6 |
| CPU Speed | 240 MHz |
| Flash | 4 MB |
| SRAM | ~520 KB |
| Framework | Arduino |

### GPIO Pin Assignments

| Function | GPIO Pin | Notes |
|----------|----------|-------|
| LED Strip 1 | GPIO 18 | VSPI CLK - fast data output |
| LED Strip 2 | GPIO 19 | VSPI MISO - fast data output |
| I2C SDA | GPIO 21 | Default ESP32 I2C data |
| I2C SCL | GPIO 22 | Default ESP32 I2C clock |

### I2C Configuration

- **Clock Speed**: 400kHz (Fast Mode)
- **Devices**: 25x CAP1188 capacitive touch sensors

## Overview

- **Communication**: Serial @ 115200 baud
- **Protocol**: ASCII line-based, newline (`\n`) terminated
- **Command IDs**: Optional `#<number>` suffix for request-response correlation

## Quick Start

1. Connect to ESP32 via USB serial at 115200 baud
2. Wait for `READY` message
3. Send commands, receive responses

## Command Reference

### LED Commands

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| SHOW | `SHOW <pos> [#id]` | `ACK SHOW <pos> [#id]` | Light LED blue at position |
| HIDE | `HIDE <pos> [#id]` | `ACK HIDE <pos> [#id]` | Turn off LED |
| SUCCESS | `SUCCESS <pos> [#id]` | `ACK SUCCESS <pos> [#id]` then `DONE SUCCESS <pos> [#id]` | Play green expansion animation |
| FAIL | `FAIL <pos> [#id]` | `ACK FAIL <pos> [#id]` | Show red LED (error indicator) |
| CONTRACT | `CONTRACT <pos> [#id]` | `ACK CONTRACT <pos> [#id]` then `DONE CONTRACT <pos> [#id]` | Animate contraction back to single green LED |
| BLINK | `BLINK <pos> [#id]` | `ACK BLINK <pos> [#id]` | Start fast orange blink ("release me") |
| STOP_BLINK | `STOP_BLINK <pos> [#id]` | `ACK STOP_BLINK <pos> [#id]` | Stop blinking, turn off LED |
| EXPAND_STEP | `EXPAND_STEP <pos> [#id]` | `ACK EXPAND_STEP <pos> [#id]` | Expand lit area by 1 LED on each side |
| CONTRACT_STEP | `CONTRACT_STEP <pos> [#id]` | `ACK CONTRACT_STEP <pos> [#id]` | Contract lit area by 1 LED on each side |
| SEQUENCE_COMPLETED | `SEQUENCE_COMPLETED [#id]` | `ACK SEQUENCE_COMPLETED [#id]` then `DONE SEQUENCE_COMPLETED [#id]` | Play celebration animation |

### Touch Commands

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| EXPECT | `EXPECT <pos> [#id]` | `ACK EXPECT <pos> [#id]` then `TOUCHED <pos> [#id]` | Wait for touch at position |
| EXPECT_RELEASE | `EXPECT_RELEASE <pos> [#id]` | `ACK EXPECT_RELEASE <pos> [#id]` then `TOUCH_RELEASED <pos> [#id]` | Wait for release at position |
| RECALIBRATE | `RECALIBRATE <pos> [#id]` | `ACK RECALIBRATE <pos> [#id]` then `RECALIBRATED <pos> [#id]` | Recalibrate sensor |
| RECALIBRATE_ALL | `RECALIBRATE_ALL [#id]` | `ACK RECALIBRATE_ALL [#id]` then `RECALIBRATED ALL [#id]` | Recalibrate all sensors |
| VALUE | `VALUE <pos> [#id]` | `VALUE <pos> <delta> [#id]` | Get current sensor delta value (-128 to 127) |
| SET_SENSITIVITY | `SET_SENSITIVITY <pos> <level>` | `ACK SET_SENSITIVITY <pos> [#id]` | Set sensor sensitivity (0=most, 7=least) |

### Utility Commands

| Command | Syntax | Response | Description |
|---------|--------|----------|-------------|
| PING | `PING [#id]` | `ACK PING [#id]` | Health check |
| INFO | `INFO [#id]` | `INFO firmware=2.0.0 protocol=2 [#id]` | Get firmware info |
| SCAN | `SCAN [#id]` | `SCANNED [A,B,C,...] [#id]` | List connected sensors |

## Response Types

### Immediate Responses

- `ACK <action> <pos> [#id]` - Command acknowledged and started
- `ERR <reason> [#id]` - Command failed

### Async Events

- `TOUCHED <pos> [#id]` - Touch detected (after EXPECT)
- `TOUCH_RELEASED <pos> [#id]` - Release detected (after EXPECT_RELEASE)
- `DONE <action> <pos> [#id]` - Long-running command completed

## Error Codes

| Error | Description |
|-------|-------------|
| `bad_format` | Malformed command |
| `unknown_action` | Unknown command |
| `unknown_position` | Invalid position (not A-Y) |
| `invalid_level` | Sensitivity level not 0-7 |
| `command_failed` | Hardware operation failed |
| `sensor_inactive` | Sensor not connected/responding |
| `busy` | Command queue full |
| `no_touch_controller` | Touch hardware unavailable |

## Positions

Positions are letters A through Y (25 total):
```
A B C D E F G H I J K L M N O P Q R S T U V W X Y
```

## Example: Playing a Sequence

### Simple Sequential Pattern

```python
import serial
import time

ser = serial.Serial('/dev/ttyUSB0', 115200)
time.sleep(2)  # Wait for Arduino ready

def send_cmd(cmd, wait_for=None):
    ser.write(f"{cmd}\n".encode())
    if wait_for:
        while True:
            line = ser.readline().decode().strip()
            if wait_for in line:
                return line

# Sequence: A, B, C
positions = ['A', 'B', 'C']
cmd_id = 1

for pos in positions:
    # Show LED
    send_cmd(f"SHOW {pos} #{cmd_id}")
    cmd_id += 1
    
    # Wait for touch
    send_cmd(f"EXPECT {pos} #{cmd_id}", f"TOUCHED {pos}")
    cmd_id += 1
    
    # Success animation
    send_cmd(f"SUCCESS {pos} #{cmd_id}", f"DONE SUCCESS {pos}")
    cmd_id += 1
    
    # Blink to signal release
    send_cmd(f"BLINK {pos} #{cmd_id}")
    cmd_id += 1
    
    # Wait for release
    send_cmd(f"EXPECT_RELEASE {pos} #{cmd_id}", f"TOUCH_RELEASED {pos}")
    cmd_id += 1
    
    # Stop blink and hide
    send_cmd(f"STOP_BLINK {pos} #{cmd_id}")
    send_cmd(f"HIDE {pos} #{cmd_id}")
    cmd_id += 2

# Celebration
send_cmd(f"SEQUENCE_COMPLETED #{cmd_id}", "DONE SEQUENCE_COMPLETED")
```

### Simultaneous Touch Pattern (e.g., A+B)

For positions that must be touched simultaneously:

```python
# Show both LEDs
send_cmd(f"SHOW A #{cmd_id}")
send_cmd(f"SHOW B #{cmd_id + 1}")

# Expect both (order doesn't matter)
send_cmd(f"EXPECT A #{cmd_id + 2}")
send_cmd(f"EXPECT B #{cmd_id + 3}")

# Wait for both TOUCHED events (with timeout for simultaneity check)
# The Pi should track timing between touches
```

### Error Handling Pattern

For indicating wrong touches:

```python
# Player touched wrong position
send_cmd(f"FAIL {wrong_pos} #{cmd_id}")  # Show red LED
time.sleep(0.5)
send_cmd(f"HIDE {wrong_pos} #{cmd_id + 1}")  # Turn off
```

### Contract After Success Pattern

For visual feedback before moving to next position:

```python
# Player touched correct position
send_cmd(f"SUCCESS {pos} #{cmd_id}", f"DONE SUCCESS {pos}")  # Expand green
time.sleep(0.3)  # Let player see the expanded success
send_cmd(f"CONTRACT {pos} #{cmd_id + 1}", f"DONE CONTRACT {pos}")  # Contract back
send_cmd(f"HIDE {pos} #{cmd_id + 2}")  # Then hide
```

## Timing Considerations

- **Simultaneity Window**: ~500ms tolerance for "simultaneous" touches
- **Touch Debounce**: 30ms to confirm touch
- **Release Debounce**: 100ms to confirm release (prevents false releases)
- **Animation Duration**: SUCCESS/CONTRACT animation ~125ms (5 steps × 25ms)
- **Poll Interval**: Touch sensors polled every 5ms
- **Blink Interval**: 150ms on/off cycle

## Sensitivity Levels

| Level | Multiplier | Description |
|-------|------------|-------------|
| 0 | 128x | Most sensitive |
| 1 | 64x | |
| 2 | 32x | Default |
| 3 | 16x | |
| 4 | 8x | |
| 5 | 4x | |
| 6 | 2x | |
| 7 | 1x | Least sensitive |

## LED Colors

| State | Color | RGB |
|-------|-------|-----|
| SHOW | Blue | (0, 0, 255) |
| SUCCESS/CONTRACT | Green | (0, 255, 0) |
| FAIL | Red | (255, 0, 0) |
| BLINK | Orange | (255, 100, 0) |
| OFF | Black | (0, 0, 0) |

## Architecture Notes

```
┌─────────────┐     Serial      ┌──────────────┐
│ Raspberry Pi │ ◄──────────────► │   Arduino    │
│ (Game Logic) │    115200 baud  │ (Hardware)   │
└─────────────┘                  └──────────────┘
      │                                │
      │  Commands ────────────────►    │  LED Control
      │                                │  Touch Sensing
      │  ◄