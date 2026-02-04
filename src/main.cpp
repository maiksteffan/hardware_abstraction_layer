/**
 * =============================================================================
 * LED & Touch Controller Firmware v2.2
 * Freenove ESP32 WROOM - Dual-Core FreeRTOS
 * =============================================================================
 * 
 * Target Hardware:
 *   - Freenove ESP32 WROOM
 *   - Dual-core Xtensa LX6 @ 240MHz
 *   - 4MB Flash, ~520KB SRAM
 * 
 * Architecture:
 *   - Core 0: Dedicated task for I2C touch sensor polling (5ms interval)
 *   - Core 1: Main loop (serial, commands) + LED animation task
 * 
 * This firmware implements a hardware executor for LED and touch control.
 * All game logic resides on the Raspberry Pi - the ESP32 is a "dumb" 
 * hardware interface that executes commands and reports events.
 * 
 * GPIO Pin Assignments:
 *   - LED Strip 1: GPIO 18
 *   - LED Strip 2: GPIO 19
 *   - I2C SDA: GPIO 21
 *   - I2C SCL: GPIO 22
 * 
 * Communication: Serial @ 115200 baud, ASCII line-based protocol
 * 
 * See README.md for complete protocol documentation.
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

// Task handles for monitoring (optional)
TaskHandle_t touchTaskHandle = nullptr;
TaskHandle_t ledTaskHandle = nullptr;

// ============================================================================
// FreeRTOS Task: Touch Sensor Polling (Core 0)
// ============================================================================
// This task runs on Core 0 and is dedicated to I2C touch sensor polling.
// Using vTaskDelayUntil() ensures precise 5ms timing regardless of
// how long the polling takes.
// ============================================================================

void touchPollingTask(void* parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t pollInterval = pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS);
    
    for (;;) {
        touchController.tick();
        
        // Use vTaskDelayUntil for precise timing (compensates for execution time)
        vTaskDelayUntil(&lastWakeTime, pollInterval);
    }
}

// ============================================================================
// FreeRTOS Task: LED Animation (Core 1)
// ============================================================================
// This task handles LED animations independently from the main loop.
// Running at ANIMATION_STEP_MS intervals for smooth animations.
// ============================================================================

void ledAnimationTask(void* parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t animInterval = pdMS_TO_TICKS(ANIMATION_STEP_MS);
    
    for (;;) {
        ledController.tick();
        
        vTaskDelayUntil(&lastWakeTime, animInterval);
    }
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Configure serial buffers BEFORE Serial.begin()
    Serial.setRxBufferSize(SERIAL_RX_BUFFER_SIZE);
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_SIZE);
    Serial.begin(SERIAL_BAUD_RATE);
    
    // Wait for serial (with timeout)
    uint32_t startTime = millis();
    while (!Serial && (millis() - startTime < 3000)) {
        delay(10);
    }
    
    // Initialize components
    eventQueue.begin();
    ledController.begin();
    
    touchController.setEventQueue(&eventQueue);
    touchController.begin();
    
    commandController.begin();
    
    // Create touch polling task on Core 0 (dedicated I2C core)
    xTaskCreatePinnedToCore(
        touchPollingTask,           // Task function
        "TouchPoll",                // Task name
        TASK_STACK_SIZE_TOUCH,      // Stack size (bytes)
        NULL,                       // Task parameters
        TASK_PRIORITY_TOUCH,        // Priority
        &touchTaskHandle,           // Task handle
        CORE_TOUCH_POLLING          // Core 0
    );
    
    // Create LED animation task on Core 1
    xTaskCreatePinnedToCore(
        ledAnimationTask,           // Task function
        "LEDAnim",                  // Task name
        TASK_STACK_SIZE_LED,        // Stack size (bytes)
        NULL,                       // Task parameters
        TASK_PRIORITY_LED,          // Priority
        &ledTaskHandle,             // Task handle
        CORE_MAIN                   // Core 1
    );
    
    // Send ready info
    eventQueue.queueInfo(NO_COMMAND_ID);
    eventQueue.flush(1);
    
    // Report active sensors
    char sensorList[64];
    touchController.buildActiveSensorList(sensorList, sizeof(sensorList));
    Serial.print("SCANNED [");
    Serial.print(sensorList);
    Serial.println("]");
    
    Serial.println("READY");
}

// ============================================================================
// Main Loop (Core 1)
// ============================================================================
// The main loop now only handles:
// - Serial communication (receiving commands)
// - Command parsing and processing
// - Flushing events to serial
// 
// Touch polling and LED animations are handled by dedicated FreeRTOS tasks.
// ============================================================================

void loop() {
    // 1. Poll serial for incoming commands from Raspberry Pi
    commandController.pollSerial();
    
    // 2. Process complete command lines
    commandController.processCompletedLines();
    
    // 3. Tick command executor (long-running commands)
    commandController.tick();
    
    // 4. Flush pending events to serial
    eventQueue.flush(5);
    
    // 5. Small yield to prevent watchdog issues and allow other tasks to run
    yield();
}
