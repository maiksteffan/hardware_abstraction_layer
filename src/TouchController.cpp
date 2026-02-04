/**
 * @file TouchController.cpp
 * @brief Implementation of touch sensor controller for CAP1188 sensors
 */

#include "TouchController.h"
#include "EventQueue.h"

// ============================================================================
// Constructor
// ============================================================================

TouchController::TouchController()
    : m_eventQueue(nullptr)
    , m_lastPollTime(0)
    , m_activeSensorCount(0)
{
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        m_sensors[i].active = false;
        m_sensors[i].currentTouched = false;
        m_sensors[i].debouncedTouched = false;
        m_sensors[i].lastReportedTouched = false;
        m_sensors[i].lastChangeTime = 0;
        
        m_expectDown[i].active = false;
        m_expectDown[i].commandId = COMMAND_ID_NONE;
        m_expectUp[i].active = false;
        m_expectUp[i].commandId = COMMAND_ID_NONE;
    }
}

// ============================================================================
// Public Methods
// ============================================================================

void TouchController::setEventQueue(EventQueue* eventQueue) {
    m_eventQueue = eventQueue;
}

bool TouchController::begin() {
    // ESP32: Initialize I2C with specific pins
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_CLOCK_SPEED_HZ);
    delay(100);
    
    m_activeSensorCount = 0;
    
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        uint8_t address = SENSOR_I2C_ADDRESSES[i];
        
        if (initSensor(address)) {
            m_sensors[i].active = true;
            m_activeSensorCount++;
        } else {
            m_sensors[i].active = false;
        }
        
        m_sensors[i].currentTouched = false;
        m_sensors[i].debouncedTouched = false;
        m_sensors[i].lastReportedTouched = false;
        m_sensors[i].lastChangeTime = 0;
        
        delay(10);
    }
    
    return m_activeSensorCount > 0;
}

void TouchController::tick() {
    uint32_t now = millis();
    
    if (now - m_lastPollTime < TOUCH_POLL_INTERVAL_MS) {
        return;
    }
    m_lastPollTime = now;
    
    pollSensors();
    processDebounce();
}

bool TouchController::recalibrate(uint8_t sensorIndex) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    if (!m_sensors[sensorIndex].active) return false;
    
    uint8_t address = SENSOR_I2C_ADDRESSES[sensorIndex];
    return writeRegister(address, CAP1188_REG_CALIBRATION_ACTIVE, CAP1188_CS1_BIT_MASK);
}

void TouchController::recalibrateAll() {
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        if (m_sensors[i].active) {
            recalibrate(i);
        }
    }
}

bool TouchController::setSensitivity(uint8_t sensorIndex, uint8_t level) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    if (!m_sensors[sensorIndex].active) return false;
    if (level > 7) return false;
    
    uint8_t address = SENSOR_I2C_ADDRESSES[sensorIndex];
    
    // Read current sensitivity register value
    uint8_t regValue;
    if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue)) {
        return false;
    }
    
    // Sensitivity is in bits 6:4 (DELTA_SENSE[2:0])
    // 0 = 128x (most sensitive), 7 = 1x (least sensitive)
    regValue = (regValue & 0x8F) | (level << 4);
    
    return writeRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue);
}

void TouchController::setExpectDown(uint8_t sensorIndex, uint32_t commandId) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return;
    m_expectDown[sensorIndex].active = true;
    m_expectDown[sensorIndex].commandId = commandId;
}

void TouchController::setExpectUp(uint8_t sensorIndex, uint32_t commandId) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return;
    m_expectUp[sensorIndex].active = true;
    m_expectUp[sensorIndex].commandId = commandId;
}

void TouchController::clearExpectDown(uint8_t sensorIndex) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return;
    m_expectDown[sensorIndex].active = false;
    m_expectDown[sensorIndex].commandId = COMMAND_ID_NONE;
}

void TouchController::clearExpectUp(uint8_t sensorIndex) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return;
    m_expectUp[sensorIndex].active = false;
    m_expectUp[sensorIndex].commandId = COMMAND_ID_NONE;
}

void TouchController::buildActiveSensorList(char* buffer, size_t bufferSize) const {
    if (bufferSize == 0) return;
    
    buffer[0] = '\0';
    size_t pos = 0;
    bool first = true;
    
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        if (m_sensors[i].active) {
            size_t needed = first ? 2 : 3;
            if (pos + needed > bufferSize) break;
            
            if (!first) buffer[pos++] = ',';
            buffer[pos++] = indexToLetter(i);
            buffer[pos] = '\0';
            first = false;
        }
    }
}

bool TouchController::isSensorActive(uint8_t sensorIndex) const {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    return m_sensors[sensorIndex].active;
}

bool TouchController::isTouched(uint8_t sensorIndex) const {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    return m_sensors[sensorIndex].debouncedTouched;
}

uint8_t TouchController::getActiveSensorCount() const {
    return m_activeSensorCount;
}

bool TouchController::readSensorValue(uint8_t sensorIndex, int8_t& value) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    if (!m_sensors[sensorIndex].active) return false;
    
    uint8_t address = SENSOR_I2C_ADDRESSES[sensorIndex];
    uint8_t rawValue;
    
    // Read delta count for CS1 (only CS1 is enabled per sensor)
    if (!readRegister(address, CAP1188_REG_SENSOR_INPUT_DELTA_1, rawValue)) {
        return false;
    }
    
    value = static_cast<int8_t>(rawValue);  // Interpret as signed
    return true;
}

// ============================================================================
// Static Utility Methods
// ============================================================================

uint8_t TouchController::letterToIndex(char letter) {
    if (letter >= 'a' && letter <= 'y') letter -= 32;
    if (letter >= 'A' && letter <= 'Y') return letter - 'A';
    return 255;
}

char TouchController::indexToLetter(uint8_t index) {
    if (index < TOUCH_SENSOR_COUNT) return 'A' + index;
    return '?';
}

// ============================================================================
// Private Methods
// ============================================================================

bool TouchController::initSensor(uint8_t address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) return false;
    
    delay(10);
    
    uint8_t prodId;
    if (!readRegister(address, CAP1188_REG_PRODUCT_ID, prodId) || prodId != 0x50) {
        return false;
    }
    
    // Allow multiple touches
    if (!writeRegister(address, CAP1188_REG_MULTIPLE_TOUCH_CONFIG, 0x00)) return false;
    
    // Speed up cycle time
    if (!writeRegister(address, CAP1188_REG_STANDBY_CONFIG, 0x30)) return false;
    
    // Enable only CS1 input
    if (!writeRegister(address, CAP1188_REG_SENSOR_INPUT_ENABLE, CAP1188_CS1_BIT_MASK)) return false;
    
    return true;
}

bool TouchController::readRegister(uint8_t address, uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    
    if (Wire.requestFrom(address, (uint8_t)1) == 1) {
        value = Wire.read();
        return true;
    }
    return false;
}

bool TouchController::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

int8_t TouchController::readRawTouch(uint8_t address) {
    uint8_t status;
    
    if (!readRegister(address, CAP1188_REG_SENSOR_INPUT_STATUS, status)) {
        return -1;  // I2C error - return error state, not "not touched"
    }
    
    bool touched = (status & CAP1188_CS1_BIT_MASK) != 0;
    
    if (touched) {
        uint8_t mainControl;
        if (readRegister(address, CAP1188_REG_MAIN_CONTROL, mainControl)) {
            writeRegister(address, CAP1188_REG_MAIN_CONTROL, mainControl & ~0x01);
        }
    }
    
    return touched ? 1 : 0;
}

void TouchController::pollSensors() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        if (!m_sensors[i].active) continue;
        
        uint8_t address = SENSOR_I2C_ADDRESSES[i];
        bool touched = readRawTouch(address);
        
        if (touched != m_sensors[i].currentTouched) {
            m_sensors[i].currentTouched = touched;
            // Only reset debounce timer if the new state differs from debounced state
            // This prevents noise from resetting the timer while holding a touch
            if (touched != m_sensors[i].debouncedTouched) {
                m_sensors[i].lastChangeTime = now;
            }
        }
    }
}

void TouchController::processDebounce() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        if (!m_sensors[i].active) continue;
        
        TouchSensorState& sensor = m_sensors[i];
        
        // Only update debounced state if current differs AND enough time has passed
        if (sensor.currentTouched != sensor.debouncedTouched) {
            uint32_t elapsed = now - sensor.lastChangeTime;
            
            // Use different debounce times: shorter for touch, longer for release
            uint16_t requiredDebounce = sensor.currentTouched ? TOUCH_DEBOUNCE_PRESS_MS : TOUCH_DEBOUNCE_RELEASE_MS;
            
            if (elapsed >= requiredDebounce) {
                sensor.debouncedTouched = sensor.currentTouched;
                
                if (sensor.debouncedTouched != sensor.lastReportedTouched) {
                    sensor.lastReportedTouched = sensor.debouncedTouched;
                    
                    if (m_eventQueue) {
                        char letter = indexToLetter(i);
                        
                        if (sensor.debouncedTouched) {
                            if (m_expectDown[i].active) {
                                m_eventQueue->queueTouched(letter, m_expectDown[i].commandId);
                                m_expectDown[i].active = false;
                                m_expectDown[i].commandId = COMMAND_ID_NONE;
                            }
                        } else {
                            if (m_expectUp[i].active) {
                                m_eventQueue->queueTouchReleased(letter, m_expectUp[i].commandId);
                                m_expectUp[i].active = false;
                                m_expectUp[i].commandId = COMMAND_ID_NONE;
                            }
                        }
                    }
                }
            }
        }
    }
}
