/**
 * =============================================================================
 * LED & Touch Controller Firmware v2.1
 * Freenove ESP32 WROOM
 * =============================================================================
 * 
 * Target Hardware:
 *   - Freenove ESP32 WROOM
 *   - Dual-core Xtensa LX6 @ 240MHz
 *   - 4MB Flash, ~520KB SRAM
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

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Initialize serial
    Serial.begin(SERIAL_BAUD_RATE);
    
    // Wait for serial (with timeout)
    uint32_t startTime = millis();
    while (!Serial && (millis() - startTime < 3000)) {
        // Wait
    }
    
    // Initialize components
    eventQueue.begin();
    ledController.begin();
    
    touchController.setEventQueue(&eventQueue);
    touchController.begin();
    
    commandController.begin();
    
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
// Main Loop
// ============================================================================

void loop() {
    // 1. Poll serial for incoming commands from Raspberry Pi
    commandController.pollSerial();
    
    // 2. Process complete command lines
    commandController.processCompletedLines();
    
    // 3. Tick command executor (long-running commands)
    commandController.tick();
    
    // 4. Tick touch controller (poll sensors, debounce, emit events)
    touchController.tick();
    
    // 5. Tick LED controller (animations)
    ledController.tick();
    
    // 6. Flush pending events to serial
    eventQueue.flush(3);
}
