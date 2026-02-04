/**
 * @file TouchController.h
 * @brief Touch sensor controller for 25 CAP1188 capacitive touch sensors over I2C
 * 
 * Protocol v2: Event-driven architecture
 * - Always polls sensors
 * - Debounces touch inputs
 * - Emits TOUCHED/TOUCH_RELEASED events when expectations are fulfilled
 */

#ifndef TOUCH_CONTROLLER_H
#define TOUCH_CONTROLLER_H

#include <Arduino.h>
#include <Wire.h>
#include "Config.h"

class EventQueue;

// ============================================================================
// Types
// ============================================================================

struct TouchSensorState {
    bool active;
    bool currentTouched;
    bool debouncedTouched;
    bool lastReportedTouched;
    uint32_t lastChangeTime;
};

struct ExpectState {
    bool active;
    uint32_t commandId;
};

// ============================================================================
// TouchController Class
// ============================================================================

class TouchController {
public:
    TouchController();
    
    void setEventQueue(EventQueue* eventQueue);
    bool begin();
    void tick();
    
    // Recalibration
    bool recalibrate(uint8_t sensorIndex);
    void recalibrateAll();
    
    // Sensitivity control
    bool setSensitivity(uint8_t sensorIndex, uint8_t level);
    
    // Expectations
    void setExpectDown(uint8_t sensorIndex, uint32_t commandId);
    void setExpectUp(uint8_t sensorIndex, uint32_t commandId);
    void clearExpectDown(uint8_t sensorIndex);
    void clearExpectUp(uint8_t sensorIndex);
    
    // State queries
    bool isSensorActive(uint8_t sensorIndex) const;
    bool isTouched(uint8_t sensorIndex) const;
    uint8_t getActiveSensorCount() const;
    void buildActiveSensorList(char* buffer, size_t bufferSize) const;
    
    // Sensor value reading
    bool readSensorValue(uint8_t sensorIndex, int8_t& value);
    
    // Utilities
    static uint8_t letterToIndex(char letter);
    static char indexToLetter(uint8_t index);

private:
    EventQueue* m_eventQueue;
    TouchSensorState m_sensors[NUM_TOUCH_SENSORS];
    ExpectState m_expectDown[NUM_TOUCH_SENSORS];
    ExpectState m_expectUp[NUM_TOUCH_SENSORS];
    uint32_t m_lastPollTime;
    uint8_t m_activeSensorCount;
    
    bool initSensor(uint8_t address);
    bool readRegister(uint8_t address, uint8_t reg, uint8_t& value);
    bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
    int8_t readRawTouch(uint8_t address);  // Returns -1 on error, 0 = not touched, 1 = touched
    void pollSensors();
    void processDebounce();
};

#endif // TOUCH_CONTROLLER_H
