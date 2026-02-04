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
TaskHandle_t ledTaskHandle = nullptr;

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

/**
 * @brief LED animation task (runs on main core)
 * 
 * Handles LED animations independently from serial processing
 * for smooth visual output.
 */
void ledAnimationTask(void* parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t animInterval = pdMS_TO_TICKS(LED_ANIMATION_STEP_MS);
    
    for (;;) {
        ledController.tick();
        vTaskDelayUntil(&lastWakeTime, animInterval);
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
    touchController.begin();
    
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
    
    // Create LED animation task on main core
    xTaskCreatePinnedToCore(
        ledAnimationTask,
        "LEDAnim",
        STACK_SIZE_LED_TASK,
        NULL,
        PRIORITY_LED_TASK,
        &ledTaskHandle,
        CORE_MAIN_LOOP
    );
    
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
    
    // Send pending events over serial
    eventQueue.flush(EVENTS_PER_FLUSH);
    
    // Yield to prevent watchdog timeout
    yield();
}
