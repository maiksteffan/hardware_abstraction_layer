/**
 * =============================================================================
 * LED & Touch Controller Firmware
 * Freenove ESP32 WROOM - Dual-Core FreeRTOS
 * =============================================================================
 * 
 * Target Hardware:
 *   - Freenove ESP32 WROOM (Dual-core Xtensa LX6 @ 240MHz)
 *   - 4MB Flash, ~520KB SRAM
 * 
 * Architecture:
 *   - Core 0: Touch sensor polling task (I2C at configurable interval)
 *   - Core 1: Main loop (serial, commands) + LED animation task
 * 
 * Purpose:
 *   Hardware executor for LED and touch control. All game logic resides
 *   on the Raspberry Pi - the ESP32 executes commands and reports events.
 * 
 * Communication:
 *   Serial ASCII line-based protocol (see Config.h for baud rate)
 * 
 * Configuration:
 *   All hardware settings, timing, and pin assignments are in Config.h
 * =============================================================================
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Config.h"
#include "LedController.h"
#include "TouchController.h"
#include "CommandController.h"
#include "EventQueue.h"

// ============================================================================
// Global Instances
// ============================================================================

EventQueue eventQueue;
LedController ledController;
TouchController touchController;
CommandController commandController(ledController, &touchController, eventQueue);

// Task handles for monitoring
TaskHandle_t touchTaskHandle = nullptr;

// ============================================================================
// FreeRTOS Tasks
// ============================================================================

/**
 * @brief Touch sensor polling task (runs on dedicated core)
 * 
 * Uses vTaskDelayUntil() for precise timing that compensates
 * for execution time variations.
 */
void touchPollingTask(void* parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t pollInterval = pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS);
    
    for (;;) {
        touchController.tick();
        vTaskDelayUntil(&lastWakeTime, pollInterval);
    }
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Initialize serial with configured buffer sizes
    Serial.setRxBufferSize(SERIAL_RX_BUFFER_SIZE);
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_SIZE);
    Serial.begin(SERIAL_BAUD_RATE);
    
    // Wait for serial connection (with timeout)
    uint32_t startTime = millis();
    while (!Serial && (millis() - startTime < SERIAL_WAIT_TIMEOUT_MS)) {
        delay(10);
    }
    
    // Initialize controllers
    eventQueue.begin();
    ledController.begin();
    
    touchController.setEventQueue(&eventQueue);
    
    // Initialize touch sensors, retry until all expected sensors are found
    while (true) {
        touchController.begin();
        
        uint8_t foundCount = touchController.getActiveSensorCount();
        if (foundCount >= EXPECTED_SENSOR_COUNT) {
            break;
        }
        
        Serial.print("Found ");
        Serial.print(foundCount);
        Serial.print("/");
        Serial.print(EXPECTED_SENSOR_COUNT);
        Serial.println(" sensors, retrying...");
        delay(300);  // Brief delay before retry
    }
    
    commandController.begin();
    
    // Create touch polling task on dedicated core
    xTaskCreatePinnedToCore(
        touchPollingTask,
        "TouchPoll",
        STACK_SIZE_TOUCH_TASK,
        NULL,
        PRIORITY_TOUCH_TASK,
        &touchTaskHandle,
        CORE_TOUCH_SENSOR
    );
    
    // Note: LED animation is now handled in main loop to avoid race conditions
    // with command processing. Both modify the NeoPixel buffer which is not thread-safe.
    
    // Send startup information
    eventQueue.queueInfo(COMMAND_ID_NONE);
    eventQueue.flush(1);
    
    // Report detected sensors
    char sensorList[SENSOR_LIST_BUFFER_SIZE];
    touchController.buildActiveSensorList(sensorList, sizeof(sensorList));
    Serial.print("SCANNED [");
    Serial.print(sensorList);
    Serial.println("]");
    
    Serial.println("READY");
}

// ============================================================================
// Main Loop (Core 1)
// ============================================================================

void loop() {
    // Handle incoming serial commands from Raspberry Pi
    commandController.pollSerial();
    commandController.processCompletedLines();
    
    // Advance long-running command execution
    commandController.tick();
    
    // Update LED animations (done here to avoid race conditions with command processing)
    ledController.tick();
    
    // Send pending events over serial
    eventQueue.flush(EVENTS_PER_FLUSH);
    
    // Yield to prevent watchdog timeout
    yield();
}
