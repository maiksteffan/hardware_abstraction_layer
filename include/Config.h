/**
 * @file Config.h
 * @brief Central configuration for LED/Touch controller firmware
 * 
 * This file contains ALL configurable values for the firmware.
 * Modify values here to customize behavior without changing code.
 * 
 * Sections:
 *   1. Firmware Metadata
 *   2. Hardware Pins
 *   3. FreeRTOS Tasks
 *   4. Serial Communication
 *   5. Queues & Buffers
 *   6. Touch Sensing
 *   7. LED Control
 *   8. Colors
 *   9. I2C Configuration
 *   10. Sensor Addresses
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// 1. FIRMWARE METADATA
// ============================================================================

#define FIRMWARE_VERSION "2.3.0"
#define PROTOCOL_VERSION "2"
#define BOARD_TYPE "ESP32_WROOM"

// ============================================================================
// 2. HARDWARE PINS (ESP32 WROOM GPIO)
// ============================================================================
// Safe GPIO pins for ESP32:
// - Avoid GPIO 0, 2 (boot strapping)
// - Avoid GPIO 6-11 (flash)
// - Avoid GPIO 34-39 (input only)
// ============================================================================

// LED Strip Data Pins
constexpr uint8_t PIN_LED_STRIP_1 = 18;  // GPIO18 - VSPI CLK
constexpr uint8_t PIN_LED_STRIP_2 = 19;  // GPIO19 - VSPI MISO

// I2C Pins
constexpr uint8_t PIN_I2C_SDA = 21;  // Default ESP32 SDA
constexpr uint8_t PIN_I2C_SCL = 22;  // Default ESP32 SCL

// ============================================================================
// 3. FREERTOS TASK CONFIGURATION
// ============================================================================

// Core assignments
constexpr uint8_t CORE_TOUCH_SENSOR = 0;  // Core 0: I2C touch polling
constexpr uint8_t CORE_MAIN_LOOP    = 1;  // Core 1: Serial, LED, commands

// Task stack sizes (bytes)
constexpr uint32_t STACK_SIZE_TOUCH_TASK = 4096;
constexpr uint32_t STACK_SIZE_LED_TASK   = 4096;

// Task priorities (higher = more important)
constexpr uint8_t PRIORITY_TOUCH_TASK = 2;
constexpr uint8_t PRIORITY_LED_TASK   = 1;

// ============================================================================
// 4. SERIAL COMMUNICATION
// ============================================================================

constexpr uint32_t SERIAL_BAUD_RATE = 115200;
constexpr size_t SERIAL_RX_BUFFER_SIZE = 256;
constexpr size_t SERIAL_TX_BUFFER_SIZE = 256;
constexpr size_t SERIAL_LINE_MAX_LENGTH = 64;
constexpr uint16_t SERIAL_STARTUP_WAIT_MS = 3000;  // Max wait for serial ready
constexpr uint16_t SERIAL_LINE_TIMEOUT_MS = 50;    // Timeout to complete partial line

// ============================================================================
// 5. QUEUES & BUFFERS
// ============================================================================

// Queue capacities
constexpr uint8_t QUEUE_SIZE_COMMANDS = 32;
constexpr uint8_t QUEUE_SIZE_EVENTS   = 64;

// Flush settings
constexpr uint8_t EVENTS_PER_FLUSH = 5;  // Max events to send per loop iteration

// Serial output buffer
constexpr size_t EVENT_MESSAGE_BUFFER_SIZE = 96;  // Max chars per event message

// Sensor list buffer (for SCANNED response)
constexpr size_t SENSOR_LIST_BUFFER_SIZE = 64;

// Serial wait timeout (milliseconds)
constexpr uint16_t SERIAL_WAIT_TIMEOUT_MS = 3000;

// Mutex timeout values (milliseconds)
constexpr uint16_t MUTEX_TIMEOUT_QUEUE_MS  = 10;
constexpr uint16_t MUTEX_TIMEOUT_SERIAL_MS = 20;
constexpr uint16_t MUTEX_TIMEOUT_FLUSH_MS  = 5;

// ============================================================================
// 6. TOUCH SENSING
// ============================================================================

constexpr uint8_t TOUCH_SENSOR_COUNT = 25;  // Total sensors (A-Y)
constexpr uint16_t TOUCH_POLL_INTERVAL_MS = 5;
constexpr uint16_t TOUCH_DEBOUNCE_PRESS_MS = 100;
constexpr uint16_t TOUCH_DEBOUNCE_RELEASE_MS = 100;
constexpr uint16_t TOUCH_INIT_DELAY_MS = 500;
constexpr uint16_t TOUCH_RECAL_DELAY_MS = 1500;

// ============================================================================
// 7. LED CONTROL
// ============================================================================

// Strip configuration
constexpr uint8_t LED_POSITION_COUNT = 25;  // Logical positions (A-Y)

#ifndef LED_STRIP_1_LENGTH
#define LED_STRIP_1_LENGTH 190
#endif

#ifndef LED_STRIP_2_LENGTH
#define LED_STRIP_2_LENGTH 190
#endif

constexpr uint8_t LED_BRIGHTNESS_DEFAULT = 128;  // 0-255

// Animation timing (milliseconds)
constexpr uint16_t LED_ANIMATION_STEP_MS = 25;
constexpr uint16_t LED_BLINK_INTERVAL_MS = 150;
constexpr uint16_t LED_SEQUENCE_STEP_MS = 10;
constexpr uint16_t LED_MENU_CHANGE_STEP_MS = 1;

// Animation parameters
constexpr uint8_t LED_SUCCESS_EXPANSION_RADIUS = 5;
constexpr uint8_t LED_SEQUENCE_PULSE_COUNT = 2;
constexpr uint16_t LED_SEQUENCE_PULSE_STEPS = 20;
constexpr uint8_t LED_SEQUENCE_MAX_BRIGHTNESS = 40;

// ============================================================================
// 8. COLORS (RGB format, 0-255 per channel)
// ============================================================================

// State: SHOW (default active state)
constexpr uint8_t COLOR_SHOW_R = 0;
constexpr uint8_t COLOR_SHOW_G = 0;
constexpr uint8_t COLOR_SHOW_B = 255;    // Blue

// State: SUCCESS (correct action)
constexpr uint8_t COLOR_SUCCESS_R = 0;
constexpr uint8_t COLOR_SUCCESS_G = 255;  // Green
constexpr uint8_t COLOR_SUCCESS_B = 0;

// State: BLINK (waiting for release)
constexpr uint8_t COLOR_BLINK_R = 0;
constexpr uint8_t COLOR_BLINK_G = 255;    // Green
constexpr uint8_t COLOR_BLINK_B = 0;

// State: FAIL (error indicator)
constexpr uint8_t COLOR_FAIL_R = 255;     // Red
constexpr uint8_t COLOR_FAIL_G = 0;
constexpr uint8_t COLOR_FAIL_B = 0;

// State: OFF
constexpr uint8_t COLOR_OFF_R = 0;
constexpr uint8_t COLOR_OFF_G = 0;
constexpr uint8_t COLOR_OFF_B = 0;

// ============================================================================
// 9. I2C CONFIGURATION
// ============================================================================

constexpr uint32_t I2C_CLOCK_SPEED_HZ = 400000;  // 400kHz Fast Mode
constexpr uint8_t I2C_RETRY_COUNT = 3;
constexpr uint16_t I2C_RETRY_DELAY_US = 100;

// ============================================================================
// CAP1188 TOUCH SENSOR REGISTERS
// ============================================================================

constexpr uint8_t CAP1188_REG_MAIN_CONTROL = 0x00;
constexpr uint8_t CAP1188_REG_SENSOR_INPUT_STATUS = 0x03;
constexpr uint8_t CAP1188_REG_SENSOR_INPUT_DELTA_1 = 0x10;
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
constexpr uint8_t CAP1188_REG_REVISION = 0xFF;

// CAP1188 default values
constexpr uint8_t CAP1188_CS1_BIT_MASK = 0x01;
constexpr uint8_t CAP1188_DEFAULT_SENSITIVITY = 0;
constexpr uint8_t CAP1188_DEFAULT_THRESHOLD = 0x10;
constexpr uint8_t CAP1188_DEFAULT_AVERAGING = 0x25;

// ============================================================================
// 10. SENSOR I2C ADDRESSES (A-Y mapping)
// ============================================================================

constexpr uint8_t SENSOR_I2C_ADDRESSES[TOUCH_SENSOR_COUNT] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x3F,  // A-E
    0x1A, 0x28, 0x29, 0x2A, 0x0E,  // F-J
    0x0F, 0x18, 0x19, 0x3C, 0x2F,  // K-O
    0x38, 0x0D, 0x0C, 0x0B, 0x3E,  // P-T
    0x2C, 0x3D, 0x08, 0x09, 0x0A   // U-Y
};

// ============================================================================
// 11. PROTOCOL CONSTANTS
// ============================================================================

constexpr uint32_t COMMAND_ID_NONE = 0xFFFFFFFF;

#endif // CONFIG_H
