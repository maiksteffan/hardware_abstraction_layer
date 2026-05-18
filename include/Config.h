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
#define PROTOCOL_VERSION "3"   // v3 = H01..H34 position tokens (3-char)
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
constexpr uint8_t PIN_LED_STRIP_2 = 25;  // GPIO19 - VSPI MISO

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
constexpr uint8_t EXPECT_ANY_QUEUE_SIZE = 8;  // Max concurrent EXPECT_ANY commands

// Flush settings
constexpr uint8_t EVENTS_PER_FLUSH = 5;  // Max events to send per loop iteration

// Serial output buffer
// Must fit "SCANNED [H01,H02,...,H34] #4294967295\n" = ~156 chars worst case.
constexpr size_t EVENT_MESSAGE_BUFFER_SIZE = 224;  // Max chars per event message

// Sensor list buffer (for SCANNED response)
// Holds up to INPUT_COUNT (34) tokens of 3 chars + commas + brackets/null.
constexpr size_t SENSOR_LIST_BUFFER_SIZE = 160;

// Serial wait timeout (milliseconds)
constexpr uint16_t SERIAL_WAIT_TIMEOUT_MS = 3000;

// Mutex timeout values (milliseconds)
constexpr uint16_t MUTEX_TIMEOUT_QUEUE_MS  = 10;
constexpr uint16_t MUTEX_TIMEOUT_SERIAL_MS = 20;
constexpr uint16_t MUTEX_TIMEOUT_FLUSH_MS  = 5;

// ============================================================================
// 6. TOUCH SENSING
// ============================================================================

constexpr uint8_t TOUCH_SENSOR_COUNT = 5;            // Physical CAP1188 chips
constexpr uint8_t TOUCH_CHANNELS_PER_SENSOR = 7;     // CS1..CS7 enabled (channels 0..6)
constexpr uint8_t INPUT_COUNT = 34;                  // Total logical inputs H01..H34

// Length of a position string including null terminator (e.g. "H01\0")
constexpr uint8_t POSITION_STRING_LENGTH = 4;

constexpr uint16_t TOUCH_POLL_INTERVAL_MS = 5;
constexpr uint16_t TOUCH_DEBOUNCE_PRESS_MS = 100;
constexpr uint16_t TOUCH_DEBOUNCE_RELEASE_MS = 100;
constexpr uint16_t TOUCH_INIT_DELAY_MS = 500;
constexpr uint16_t TOUCH_RECAL_DELAY_MS = 1500;

// Startup sequence timing
constexpr uint32_t STARTUP_HANDSHAKE_INTERVAL_MS = 3000;
constexpr uint16_t SENSOR_INIT_DELAY_MS = 1500;
constexpr uint16_t LED_INIT_DELAY_MS = 500;
constexpr uint8_t  SENSOR_INIT_MAX_RETRIES = 3;

// ============================================================================
// 7. LED CONTROL
// ============================================================================

// Strip configuration
constexpr uint8_t LED_POSITION_COUNT = 34;  // Logical positions (H01..H34)

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
constexpr uint8_t LED_SUCCESS_EXPANSION_RADIUS = 4;
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

constexpr uint32_t I2C_CLOCK_SPEED_HZ = 100000;  // 400kHz Fast Mode
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
// 10. SENSOR I2C ADDRESSES (one per physical CAP1188 chip)
// ============================================================================

constexpr uint8_t SENSOR_I2C_ADDRESSES[TOUCH_SENSOR_COUNT] = {
    0x28, 0x29, 0x2A, 0x2B, 0x2C
};

// ============================================================================
// 10b. INPUT MAPPING: H01..H34  ->  (sensor_index, channel)
// ============================================================================
// sensor_index: 0..TOUCH_SENSOR_COUNT-1 (index into SENSOR_I2C_ADDRESSES)
// channel:      0..TOUCH_CHANNELS_PER_SENSOR-1 (CAP1188 CS1..CS7)
//
// EDIT THIS TABLE TO MATCH YOUR PHYSICAL WIRING.
// Entry i corresponds to input "H{i+1:02}" (so INPUT_MAPPINGS[0] is H01).
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

// ============================================================================
// 11. PROTOCOL CONSTANTS
// ============================================================================

constexpr uint32_t COMMAND_ID_NONE = 0xFFFFFFFF;

#endif // CONFIG_H
