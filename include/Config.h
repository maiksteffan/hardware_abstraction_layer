/**
 * @file Config.h
 * @brief Configuration constants for the LED/Touch controller firmware
 * 
 * Protocol version 2.0 - Event-driven architecture
 * ESP32 WROOM acts as "dumb" hardware executor + event source
 * All game logic resides on Raspberry Pi
 * 
 * Target: Freenove ESP32 WROOM
 *   - CPU: Dual-core Xtensa LX6 @ 240MHz
 *   - Flash: 4MB
 *   - SRAM: ~520KB
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// Firmware Information
// ============================================================================

#define FIRMWARE_VERSION "2.1.0-ESP32"
#define PROTOCOL_VERSION "2"
#define BOARD_TYPE "ESP32_WROOM"

// ============================================================================
// Serial Protocol Configuration
// ============================================================================

constexpr size_t MAX_LINE_LEN = 64;
constexpr uint32_t SERIAL_BAUD_RATE = 115200;

// ============================================================================
// Queue Sizes (ESP32 has ~520KB SRAM - can use larger queues)
// ============================================================================

constexpr uint8_t COMMAND_QUEUE_SIZE = 16;
constexpr uint8_t EVENT_QUEUE_SIZE = 32;

// ============================================================================
// Touch Sensing Configuration
// ============================================================================

constexpr uint16_t TOUCH_POLL_INTERVAL_MS = 5;
constexpr uint16_t DEBOUNCE_TOUCH_MS = 100;    // Time to confirm touch
constexpr uint16_t DEBOUNCE_RELEASE_MS = 100; // Time to confirm release (longer = more stable)
constexpr uint8_t NUM_TOUCH_SENSORS = 25;

// ============================================================================
// LED Configuration (ESP32 WROOM GPIO pins)
// ============================================================================
// Using GPIO pins that are safe for output on ESP32:
// - Avoiding GPIO 0, 2 (boot strapping)
// - Avoiding GPIO 6-11 (connected to flash)
// - Avoiding GPIO 34-39 (input only)
// 
// GPIO 18 & 19: Safe output pins, VSPI pins (good for fast data)
// ============================================================================

constexpr uint8_t NUM_POSITIONS = 25;

constexpr uint8_t STRIP1_PIN = 18;  // GPIO18 - VSPI CLK, good for data output
constexpr uint8_t STRIP2_PIN = 19;  // GPIO19 - VSPI MISO, good for data output

#ifndef NUM_LEDS_STRIP1
#define NUM_LEDS_STRIP1 190
#endif

#ifndef NUM_LEDS_STRIP2
#define NUM_LEDS_STRIP2 190
#endif

constexpr uint8_t LED_BRIGHTNESS = 128;

// Animation settings
constexpr uint8_t SUCCESS_PULSE_COUNT = 2;
constexpr uint16_t SUCCESS_PULSE_STEPS = 20;
constexpr uint16_t ANIMATION_STEP_MS = 25;
constexpr uint16_t BLINK_INTERVAL_MS = 150;

// ============================================================================
// Colors (RGB format)
// ============================================================================

// SHOW = Blue
constexpr uint8_t COLOR_SHOW_R = 0;
constexpr uint8_t COLOR_SHOW_G = 0;
constexpr uint8_t COLOR_SHOW_B = 255;

// SUCCESS = Green
constexpr uint8_t COLOR_SUCCESS_R = 0;
constexpr uint8_t COLOR_SUCCESS_G = 255;
constexpr uint8_t COLOR_SUCCESS_B = 0;

// BLINK = Green (fast blink for "release me")
constexpr uint8_t COLOR_BLINK_R = 0;
constexpr uint8_t COLOR_BLINK_G = 255;
constexpr uint8_t COLOR_BLINK_B = 0;

// FAIL = Red
constexpr uint8_t COLOR_FAIL_R = 255;
constexpr uint8_t COLOR_FAIL_G = 0;
constexpr uint8_t COLOR_FAIL_B = 0;

// OFF = Black
constexpr uint8_t COLOR_OFF_R = 0;
constexpr uint8_t COLOR_OFF_G = 0;
constexpr uint8_t COLOR_OFF_B = 0;

// ============================================================================
// I2C Configuration (ESP32 WROOM)
// ============================================================================
// ESP32 default I2C pins:
//   SDA: GPIO 21
//   SCL: GPIO 22
// ESP32 supports I2C clock up to 1MHz (Fast Mode Plus)
// ============================================================================

constexpr uint8_t I2C_SDA_PIN = 21;  // ESP32 default SDA
constexpr uint8_t I2C_SCL_PIN = 22;  // ESP32 default SCL
constexpr uint32_t I2C_CLOCK_SPEED = 400000;  // 400kHz Fast Mode (ESP32 supports up to 1MHz)
constexpr uint8_t I2C_MAX_RETRIES = 3;
constexpr uint16_t I2C_RETRY_DELAY_US = 100;

// CAP1188 Register addresses
constexpr uint8_t CAP1188_REG_MAIN_CONTROL = 0x00;
constexpr uint8_t CAP1188_REG_SENSOR_INPUT_STATUS = 0x03;
constexpr uint8_t CAP1188_REG_SENSOR_INPUT_DELTA_1 = 0x10;  // Delta count for CS1
constexpr uint8_t CAP1188_REG_SENSITIVITY_CONTROL = 0x1F;
constexpr uint8_t CAP1188_REG_CONFIG1 = 0x20;
constexpr uint8_t CAP1188_REG_SENSOR_INPUT_ENABLE = 0x21;
constexpr uint8_t CAP1188_REG_AVERAGING_SAMPLING = 0x24;
constexpr uint8_t CAP1188_REG_CALIBRATION_ACTIVE = 0x26;
constexpr uint8_t CAP1188_REG_INTERRUPT_ENABLE = 0x27;
constexpr uint8_t CAP1188_REG_REPEAT_ENABLE = 0x28;
constexpr uint8_t CAP1188_REG_MULTIPLE_TOUCH_CONFIG = 0x2A;
constexpr uint8_t CAP1188_REG_SENSOR_THRESHOLD_1 = 0x30;
constexpr uint8_t CAP1188_REG_STANDBY_CONFIG = 0x41;
constexpr uint8_t CAP1188_REG_LED_LINK = 0x72;
constexpr uint8_t CAP1188_REG_PRODUCT_ID = 0xFD;
constexpr uint8_t CAP1188_REG_MANUFACTURER_ID = 0xFE;
constexpr uint8_t C