/**
 * @file TouchController.cpp
 * @brief Implementation of multi-channel CAP1188 touch input controller
 *
 * Indexing model:
 *   - "sensor index" (0..TOUCH_SENSOR_COUNT-1) addresses one physical CAP1188 chip.
 *   - "input index"  (0..INPUT_COUNT-1)        addresses one logical H01..H35 input.
 *   - INPUT_MAPPINGS[i] = {sensorIndex, channel} resolves input i to hardware.
 *
 * Public API methods take input indices (NOT sensor indices).
 */

#include "TouchController.h"
#include "CommandController.h"   // for indexToPosition()
#include "EventQueue.h"

// ============================================================================
// Constructor
// ============================================================================

TouchController::TouchController()
    : m_eventQueue(nullptr)
    , m_expectAnyHead(0)
    , m_expectAnyTail(0)
    , m_pressEdgeRingPos(0)
    , m_sweepHoldoffUntil(0)
    , m_lastPollTime(0)
    , m_activeSensorCount(0)
    , m_handsOffDetectionEnabled(false)
    , m_lastAnyTouched(false)
{
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        m_sensors[s].active = false;
        m_sensors[s].enableMask = 0;
        m_sensors[s].lastStatus = 0;
        m_sensors[s].statusValid = false;
    }
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        m_inputs[i].currentTouched = false;
        m_inputs[i].debouncedTouched = false;
        m_inputs[i].lastReportedTouched = false;
        m_inputs[i].lastChangeTime = 0;
        m_inputs[i].pressStartTime = 0;
        m_inputs[i].pressConsumed = false;

        m_expectDown[i].active = false;
        m_expectDown[i].commandId = COMMAND_ID_NONE;
        m_expectDown[i].excludeMask = 0;
        m_expectUp[i].active = false;
        m_expectUp[i].commandId = COMMAND_ID_NONE;
        m_expectUp[i].excludeMask = 0;
    }
    for (uint8_t i = 0; i < EXPECT_ANY_QUEUE_SIZE; i++) {
        m_expectAnyQueue[i].active = false;
        m_expectAnyQueue[i].commandId = COMMAND_ID_NONE;
        m_expectAnyQueue[i].excludeMask = 0;
    }
    memset(m_expectAnyUsed, false, sizeof(m_expectAnyUsed));
    memset(m_anyCandidates, 0, sizeof(m_anyCandidates));
    memset(m_pressEdgeRing, 0, sizeof(m_pressEdgeRing));
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

    // Reset sensor state
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        m_sensors[s].active = false;
        m_sensors[s].enableMask = 0;
        m_sensors[s].lastStatus = 0;
        m_sensors[s].statusValid = false;
    }

    // Build per-sensor enable mask from INPUT_MAPPINGS
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t s = INPUT_MAPPINGS[i].sensorIndex;
        uint8_t ch = INPUT_MAPPINGS[i].channel;
        if (s < TOUCH_SENSOR_COUNT && ch < 8) {
            m_sensors[s].enableMask |= (uint8_t)(1 << ch);
        }
    }

    // Initialize each physical sensor
    m_activeSensorCount = 0;
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        if (initSensor(s)) {
            m_sensors[s].active = true;
            m_activeSensorCount++;
        }
        delay(10);
    }

    // Reset input state
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        m_inputs[i].currentTouched = false;
        m_inputs[i].debouncedTouched = false;
        m_inputs[i].lastReportedTouched = false;
        m_inputs[i].lastChangeTime = 0;
        m_inputs[i].pressStartTime = 0;
        m_inputs[i].pressConsumed = false;
    }
    memset(m_anyCandidates, 0, sizeof(m_anyCandidates));
    memset(m_pressEdgeRing, 0, sizeof(m_pressEdgeRing));
    m_pressEdgeRingPos = 0;
    m_sweepHoldoffUntil = 0;

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
    processExpectAnyQualification();
    processHandsOffDetection();
}

bool TouchController::recalibrate(uint8_t inputIndex) {
    if (inputIndex >= INPUT_COUNT) return false;
    uint8_t s = INPUT_MAPPINGS[inputIndex].sensorIndex;
    uint8_t ch = INPUT_MAPPINGS[inputIndex].channel;
    if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) return false;

    return writeRegister(SENSOR_I2C_ADDRESSES[s],
                         CAP1188_REG_CALIBRATION_ACTIVE,
                         (uint8_t)(1 << ch));
}

void TouchController::recalibrateAll() {
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        if (!m_sensors[s].active) continue;
        writeRegister(SENSOR_I2C_ADDRESSES[s],
                      CAP1188_REG_CALIBRATION_ACTIVE,
                      m_sensors[s].enableMask);
    }
}

bool TouchController::setSensitivity(uint8_t inputIndex, uint8_t level) {
    if (inputIndex >= INPUT_COUNT) return false;
    if (level > 7) return false;
    uint8_t s = INPUT_MAPPINGS[inputIndex].sensorIndex;
    if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) return false;

    // CAP1188 sensitivity is GLOBAL per chip (bits 6:4 of SENSITIVITY_CONTROL).
    // 0 = 128x (most sensitive), 7 = 1x (least sensitive).
    uint8_t address = SENSOR_I2C_ADDRESSES[s];
    uint8_t regValue;
    if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue)) {
        return false;
    }
    regValue = (uint8_t)((regValue & 0x8F) | (level << 4));
    return writeRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue);
}

void TouchController::setExpectDown(uint8_t inputIndex, uint32_t commandId) {
    if (inputIndex >= INPUT_COUNT) return;

    // If the input is ALREADY held when the EXPECT arrives, there will never
    // be a press edge. Report TOUCHED immediately - but only if the grab is
    // FRESH (within EXPECT_HELD_FRESH_MS) AND this press has not already
    // satisfied a previous EXPECT/EXPECT_ANY. Each press edge may only be
    // consumed ONCE: without this, a sequence alternating between two holds
    // that stay held ping-pongs instantly (the same uninterrupted grab
    // "answers" every re-armed EXPECT), falsely advancing the sequence.
    // This still covers the legit race of a player grabbing the hold a
    // moment before the Pi's EXPECT arrives, while stale or already-counted
    // holds must be released and re-grabbed to count.
    if (m_inputs[inputIndex].debouncedTouched && m_eventQueue &&
        !m_inputs[inputIndex].pressConsumed &&
        (millis() - m_inputs[inputIndex].pressStartTime) <= EXPECT_HELD_FRESH_MS) {
        m_inputs[inputIndex].pressConsumed = true;
        char posStr[POSITION_STRING_LENGTH];
        CommandController::indexToPosition(inputIndex, posStr);
        m_eventQueue->queueTouched(posStr, commandId);
        return;
    }

    m_expectDown[inputIndex].active = true;
    m_expectDown[inputIndex].commandId = commandId;
}

void TouchController::setExpectUp(uint8_t inputIndex, uint32_t commandId) {
    if (inputIndex >= INPUT_COUNT) return;

    // Symmetric to setExpectDown: if the input is already released, there
    // will never be a release edge - report TOUCH_RELEASED immediately.
    if (!m_inputs[inputIndex].debouncedTouched && m_eventQueue) {
        char posStr[POSITION_STRING_LENGTH];
        CommandController::indexToPosition(inputIndex, posStr);
        m_eventQueue->queueTouchReleased(posStr, commandId);
        return;
    }

    m_expectUp[inputIndex].active = true;
    m_expectUp[inputIndex].commandId = commandId;
}

void TouchController::clearExpectDown(uint8_t inputIndex) {
    if (inputIndex >= INPUT_COUNT) return;
    m_expectDown[inputIndex].active = false;
    m_expectDown[inputIndex].commandId = COMMAND_ID_NONE;
}

void TouchController::clearExpectUp(uint8_t inputIndex) {
    if (inputIndex >= INPUT_COUNT) return;
    m_expectUp[inputIndex].active = false;
    m_expectUp[inputIndex].commandId = COMMAND_ID_NONE;
}

void TouchController::setExpectAny(uint32_t commandId) {
    setExpectAnyExcept(0, commandId);
}

void TouchController::setExpectAnyExcept(uint64_t excludeMask, uint32_t commandId) {
    // Enqueue into circular buffer
    m_expectAnyQueue[m_expectAnyHead].active = true;
    m_expectAnyQueue[m_expectAnyHead].commandId = commandId;
    m_expectAnyQueue[m_expectAnyHead].excludeMask = excludeMask;
    m_expectAnyHead = (m_expectAnyHead + 1) % EXPECT_ANY_QUEUE_SIZE;

    // If head catches tail, advance tail (drop oldest)
    if (m_expectAnyHead == m_expectAnyTail) {
        m_expectAnyTail = (m_expectAnyTail + 1) % EXPECT_ANY_QUEUE_SIZE;
    }
}

void TouchController::clearExpectAny() {
    for (uint8_t i = 0; i < EXPECT_ANY_QUEUE_SIZE; i++) {
        m_expectAnyQueue[i].active = false;
        m_expectAnyQueue[i].commandId = COMMAND_ID_NONE;
        m_expectAnyQueue[i].excludeMask = 0;
    }
    m_expectAnyHead = 0;
    m_expectAnyTail = 0;
    memset(m_expectAnyUsed, false, sizeof(m_expectAnyUsed));
    // Drop any in-flight qualification candidates as well.
    memset(m_anyCandidates, 0, sizeof(m_anyCandidates));
}

void TouchController::clearAllExpectations() {
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        m_expectDown[i].active = false;
        m_expectDown[i].commandId = COMMAND_ID_NONE;
        m_expectUp[i].active = false;
        m_expectUp[i].commandId = COMMAND_ID_NONE;
    }
    clearExpectAny();
}

void TouchController::setHandsOffDetection(bool enabled, uint32_t commandId) {
    m_handsOffDetectionEnabled = enabled;
    if (!enabled) return;

    // Report the current state immediately so the Pi never waits for a
    // transition that already happened (e.g. enabling while board is empty).
    m_lastAnyTouched = anyInputTouched();
    if (m_eventQueue) {
        if (m_lastAnyTouched) {
            m_eventQueue->queueHandsOn(commandId);
        } else {
            m_eventQueue->queueHandsOff(commandId);
        }
    }
}

void TouchController::buildActiveSensorList(char* buffer, size_t bufferSize) const {
    if (bufferSize == 0) return;

    buffer[0] = '\0';
    size_t pos = 0;
    bool first = true;

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t s = INPUT_MAPPINGS[i].sensorIndex;
        if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) continue;

        char tok[POSITION_STRING_LENGTH];
        CommandController::indexToPosition(i, tok);

        // Need: comma (if not first) + 3 chars + null
        size_t needed = (first ? 3 : 4) + 1;
        if (pos + needed > bufferSize) break;

        if (!first) buffer[pos++] = ',';
        buffer[pos++] = tok[0];
        buffer[pos++] = tok[1];
        buffer[pos++] = tok[2];
        buffer[pos] = '\0';
        first = false;
    }
}

void TouchController::buildFailedInputList(char* buffer, size_t bufferSize) const {
    if (bufferSize == 0) return;

    buffer[0] = '\0';
    size_t pos = 0;
    bool first = true;

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t s = INPUT_MAPPINGS[i].sensorIndex;
        if (s >= TOUCH_SENSOR_COUNT) continue;
        if (m_sensors[s].active) continue;  // only list inputs whose sensor FAILED

        char tok[POSITION_STRING_LENGTH];
        CommandController::indexToPosition(i, tok);

        size_t needed = (first ? 3 : 4) + 1;
        if (pos + needed > bufferSize) break;

        if (!first) buffer[pos++] = ',';
        buffer[pos++] = tok[0];
        buffer[pos++] = tok[1];
        buffer[pos++] = tok[2];
        buffer[pos] = '\0';
        first = false;
    }
}

bool TouchController::isInputActive(uint8_t inputIndex) const {
    if (inputIndex >= INPUT_COUNT) return false;
    uint8_t s = INPUT_MAPPINGS[inputIndex].sensorIndex;
    if (s >= TOUCH_SENSOR_COUNT) return false;
    return m_sensors[s].active;
}

bool TouchController::isTouched(uint8_t inputIndex) const {
    if (inputIndex >= INPUT_COUNT) return false;
    return m_inputs[inputIndex].debouncedTouched;
}

uint8_t TouchController::getActiveSensorCount() const {
    return m_activeSensorCount;
}

bool TouchController::readSensorValue(uint8_t inputIndex, int8_t& value) {
    if (inputIndex >= INPUT_COUNT) return false;
    uint8_t s = INPUT_MAPPINGS[inputIndex].sensorIndex;
    uint8_t ch = INPUT_MAPPINGS[inputIndex].channel;
    if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) return false;

    uint8_t rawValue;
    if (!readRegister(SENSOR_I2C_ADDRESSES[s],
                      (uint8_t)(CAP1188_REG_SENSOR_INPUT_DELTA_1 + ch),
                      rawValue)) {
        return false;
    }
    value = static_cast<int8_t>(rawValue);  // Interpret as signed
    return true;
}

// ============================================================================
// Private Methods
// ============================================================================

bool TouchController::initSensor(uint8_t sensorIndex) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) return false;
    uint8_t address = SENSOR_I2C_ADDRESSES[sensorIndex];

    // NOTE: We deliberately do NOT do a bare address-only probe here.
    // Some CAP1188 board variants / level shifters don't ACK an empty
    // beginTransmission/endTransmission cleanly. Instead we go straight to
    // reading the product-ID register; that exercises a real read transaction
    // and is the most reliable "is the chip alive" check.

    uint8_t prodId;
    if (!readRegister(address, CAP1188_REG_PRODUCT_ID, prodId)) {
        return false;
    }
    // Accept any nonzero/non-0xFF product ID. CAP1188 = 0x50, CAP1166 = 0x51.
    // 0x00 / 0xFF usually mean "no device / floating bus".
    if (prodId == 0x00 || prodId == 0xFF) {
        return false;
    }

    // Allow multiple simultaneous touches
    if (!writeRegister(address, CAP1188_REG_MULTIPLE_TOUCH_CONFIG, 0x00)) return false;

    // Speed up cycle time
    if (!writeRegister(address, CAP1188_REG_STANDBY_CONFIG, 0x30)) return false;

    // Apply default sensitivity (bits 6:4 of SENSITIVITY_CONTROL, global per chip)
    uint8_t sens;
    if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, sens)) return false;
    sens = (uint8_t)((sens & 0x8F) | (CAP1188_DEFAULT_SENSITIVITY << 4));
    if (!writeRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, sens)) return false;

    // Enable exactly the channels referenced by INPUT_MAPPINGS for this sensor.
    uint8_t mask = m_sensors[sensorIndex].enableMask;
    if (mask == 0) {
        // No inputs mapped to this sensor - still considered "init succeeded".
        return true;
    }
    if (!writeRegister(address, CAP1188_REG_SENSOR_INPUT_ENABLE, mask)) return false;

    return true;
}

// ----------------------------------------------------------------------------
// Diagnostics
// ----------------------------------------------------------------------------

uint8_t TouchController::scanI2CBus(char* buffer, size_t bufferSize) const {
    if (bufferSize > 0) buffer[0] = '\0';
    size_t pos = 0;
    uint8_t found = 0;
    bool first = true;

    // Standard 7-bit I²C scan range
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            found++;
            // "0xAA" = 4 chars, plus optional comma, plus null
            if (pos + (first ? 4 : 5) + 1 <= bufferSize) {
                if (!first) buffer[pos++] = ',';
                pos += snprintf(buffer + pos, bufferSize - pos, "0x%02X", addr);
                first = false;
            }
        }
    }
    return found;
}

bool TouchController::diagInitSensor(uint8_t sensorIndex, char* outDiag, size_t diagSize) {
    if (sensorIndex >= TOUCH_SENSOR_COUNT) {
        if (diagSize) snprintf(outDiag, diagSize, "S? FAIL bad_index");
        return false;
    }
    uint8_t address = SENSOR_I2C_ADDRESSES[sensorIndex];

    uint8_t prodId = 0;
    if (!readRegister(address, CAP1188_REG_PRODUCT_ID, prodId)) {
        snprintf(outDiag, diagSize, "S%u@0x%02X FAIL pid_read", sensorIndex, address);
        m_sensors[sensorIndex].active = false;
        return false;
    }
    if (prodId == 0x00 || prodId == 0xFF) {
        snprintf(outDiag, diagSize, "S%u@0x%02X FAIL pid=0x%02X", sensorIndex, address, prodId);
        m_sensors[sensorIndex].active = false;
        return false;
    }

    if (!writeRegister(address, CAP1188_REG_MULTIPLE_TOUCH_CONFIG, 0x00)) {
        snprintf(outDiag, diagSize, "S%u@0x%02X FAIL write_mtblk pid=0x%02X",
                 sensorIndex, address, prodId);
        m_sensors[sensorIndex].active = false;
        return false;
    }
    if (!writeRegister(address, CAP1188_REG_STANDBY_CONFIG, 0x30)) {
        snprintf(outDiag, diagSize, "S%u@0x%02X FAIL write_standby pid=0x%02X",
                 sensorIndex, address, prodId);
        m_sensors[sensorIndex].active = false;
        return false;
    }

    uint8_t mask = m_sensors[sensorIndex].enableMask;
    if (mask != 0) {
        if (!writeRegister(address, CAP1188_REG_SENSOR_INPUT_ENABLE, mask)) {
            snprintf(outDiag, diagSize, "S%u@0x%02X FAIL write_enable pid=0x%02X",
                     sensorIndex, address, prodId);
            m_sensors[sensorIndex].active = false;
            return false;
        }
    }

    snprintf(outDiag, diagSize, "S%u@0x%02X OK pid=0x%02X mask=0x%02X",
             sensorIndex, address, prodId, mask);
    m_sensors[sensorIndex].active = true;
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

void TouchController::pollSensors() {
    uint32_t now = millis();

    // Step 1: read each active sensor's status register once and cache it.
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        m_sensors[s].statusValid = false;
        if (!m_sensors[s].active) continue;

        uint8_t status;
        if (readRegister(SENSOR_I2C_ADDRESSES[s],
                         CAP1188_REG_SENSOR_INPUT_STATUS, status)) {
            m_sensors[s].lastStatus = status;
            m_sensors[s].statusValid = true;
        }
    }

    // Step 2: update per-input "current" state from cached status registers.
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t s = INPUT_MAPPINGS[i].sensorIndex;
        uint8_t ch = INPUT_MAPPINGS[i].channel;
        if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) continue;
        if (!m_sensors[s].statusValid) continue;  // I2C read failed this cycle

        bool touched = ((m_sensors[s].lastStatus >> ch) & 0x01) != 0;

        if (touched != m_inputs[i].currentTouched) {
            m_inputs[i].currentTouched = touched;
            // Only reset debounce timer if the new state differs from debounced state
            // (prevents noise from resetting the timer while holding a touch)
            if (touched != m_inputs[i].debouncedTouched) {
                m_inputs[i].lastChangeTime = now;
            }
        }
    }

    // Step 3: clear the INT bit on every sensor that reported any active channel,
    // so that future touches are detected.
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        if (!m_sensors[s].active || !m_sensors[s].statusValid) continue;
        if (m_sensors[s].lastStatus == 0) continue;

        uint8_t mc;
        if (readRegister(SENSOR_I2C_ADDRESSES[s], CAP1188_REG_MAIN_CONTROL, mc)) {
            writeRegister(SENSOR_I2C_ADDRESSES[s],
                          CAP1188_REG_MAIN_CONTROL, (uint8_t)(mc & ~0x01));
        }
    }
}

void TouchController::processDebounce() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t s = INPUT_MAPPINGS[i].sensorIndex;
        if (s >= TOUCH_SENSOR_COUNT || !m_sensors[s].active) continue;

        TouchInputState& input = m_inputs[i];

        if (input.currentTouched == input.debouncedTouched) continue;

        uint32_t elapsed = now - input.lastChangeTime;
        uint16_t requiredDebounce = input.currentTouched
                                        ? TOUCH_DEBOUNCE_PRESS_MS
                                        : TOUCH_DEBOUNCE_RELEASE_MS;
        if (elapsed < requiredDebounce) continue;

        input.debouncedTouched = input.currentTouched;
        if (input.debouncedTouched == input.lastReportedTouched) continue;
        input.lastReportedTouched = input.debouncedTouched;

        if (!m_eventQueue) continue;

        char posStr[POSITION_STRING_LENGTH];
        CommandController::indexToPosition(i, posStr);

        if (input.debouncedTouched) {
            input.pressStartTime = now;
            input.pressConsumed = false;  // fresh press edge: may satisfy one EXPECT
            recordPressEdge(now);

            // EXPECT_ANY: do NOT report immediately. Open a qualification
            // window that rejects brush-by contacts; the touch is reported
            // by processExpectAnyQualification() once it proves to be a
            // sustained grab. (EXPECT <pos> below stays instant - with a
            // single armed hold a false positive is impossible.)
            if (m_expectAnyTail != m_expectAnyHead &&
                m_expectAnyQueue[m_expectAnyTail].active &&
                !m_expectAnyUsed[i])
            {
                startAnyCandidate(i, now);
            }
            if (m_expectDown[i].active) {
                input.pressConsumed = true;
                m_eventQueue->queueTouched(posStr, m_expectDown[i].commandId);
                m_expectDown[i].active = false;
                m_expectDown[i].commandId = COMMAND_ID_NONE;
            }
        } else {
            // Debounced release: any pending candidate is stale by now.
            m_anyCandidates[i].active = false;
            if (m_expectUp[i].active) {
                m_eventQueue->queueTouchReleased(posStr, m_expectUp[i].commandId);
                m_expectUp[i].active = false;
                m_expectUp[i].commandId = COMMAND_ID_NONE;
            }
        }
    }
}

bool TouchController::anyInputTouched() const {
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        if (m_inputs[i].debouncedTouched) return true;
    }
    return false;
}

// ============================================================================
// EXPECT_ANY qualification (brush-by rejection - see Config.h section 6b)
// ============================================================================

void TouchController::startAnyCandidate(uint8_t inputIndex, uint32_t now) {
    AnyCandidate& c = m_anyCandidates[inputIndex];
    c.active = true;
    c.startTime = now;
    c.deltaSamples = 0;
    c.goodDeltaSamples = 0;
    c.untouchedStreak = 0;
}

void TouchController::recordPressEdge(uint32_t now) {
    m_pressEdgeRing[m_pressEdgeRingPos] = now;
    m_pressEdgeRingPos = (uint8_t)((m_pressEdgeRingPos + 1) % EXPECT_ANY_EDGE_RING_SIZE);

    // Count press edges inside the sweep window. 3+ new contacts in half a
    // second cannot be intentional grabs (max 2 hands) - treat as an arm
    // sweeping across the wall and defer all EXPECT_ANY decisions.
    uint8_t recent = 0;
    for (uint8_t k = 0; k < EXPECT_ANY_EDGE_RING_SIZE; k++) {
        if (m_pressEdgeRing[k] != 0 &&
            now - m_pressEdgeRing[k] <= EXPECT_ANY_SWEEP_WINDOW_MS) {
            recent++;
        }
    }
    if (recent >= EXPECT_ANY_SWEEP_TOUCH_COUNT) {
        m_sweepHoldoffUntil = now + EXPECT_ANY_SWEEP_HOLDOFF_MS;
    }
}

void TouchController::fireExpectAny(uint8_t inputIndex) {
    if (!m_eventQueue) return;
    if (m_expectAnyTail == m_expectAnyHead) return;  // queue empty

    ExpectState& head = m_expectAnyQueue[m_expectAnyTail];
    if (!head.active || m_expectAnyUsed[inputIndex]) return;
    if ((head.excludeMask >> inputIndex) & (uint64_t)1) return;  // EXPECT_ANY_EXCEPT

    char posStr[POSITION_STRING_LENGTH];
    CommandController::indexToPosition(inputIndex, posStr);
    m_inputs[inputIndex].pressConsumed = true;
    m_eventQueue->queueTouched(posStr, head.commandId);

    head.active = false;
    head.commandId = COMMAND_ID_NONE;
    m_expectAnyTail = (uint8_t)((m_expectAnyTail + 1) % EXPECT_ANY_QUEUE_SIZE);
    m_expectAnyUsed[inputIndex] = true;

    // Reset used mask when queue is empty
    if (m_expectAnyTail == m_expectAnyHead) {
        memset(m_expectAnyUsed, false, sizeof(m_expectAnyUsed));
    }
}

void TouchController::processExpectAnyQualification() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        AnyCandidate& c = m_anyCandidates[i];
        if (!c.active) continue;

        // Input already reported for the current EXPECT_ANY batch.
        if (m_expectAnyTail != m_expectAnyHead && m_expectAnyUsed[i]) {
            c.active = false;
            continue;
        }

        // ---- 1. Persistence: raw contact must not drop out ----
        if (m_inputs[i].currentTouched) {
            c.untouchedStreak = 0;
        } else if (++c.untouchedStreak >= EXPECT_ANY_DROPOUT_SAMPLES) {
            // Contact vanished: classic brush-by. A real grab produces a new
            // press edge later and re-qualifies from scratch.
            c.active = false;
            continue;
        }

        // ---- 2. Delta consistency: sample grab strength ----
        int8_t delta;
        if (readSensorValue(i, delta)) {
            c.deltaSamples++;
            if (delta >= EXPECT_ANY_DELTA_MIN) c.goodDeltaSamples++;
        }

        // ---- 3. Decide at the end of the confirmation window ----
        if (now - c.startTime < EXPECT_ANY_CONFIRM_MS) continue;
        if (now < m_sweepHoldoffUntil) continue;  // arm sweep: defer decision

        // Queue empty right now (e.g. two simultaneous grabs but the Pi
        // queues EXPECT_ANY one at a time, so the second command only
        // arrives after the first TOUCHED). Keep the qualified candidate
        // PENDING while the grab is still fresh - it fires on the tick
        // after the next EXPECT_ANY is queued. Without this the second
        // simultaneous touch would never be reported (no new press edge).
        if (m_expectAnyTail == m_expectAnyHead) {
            if (now - m_inputs[i].pressStartTime > EXPECT_HELD_FRESH_MS) {
                c.active = false;  // stale - require a fresh re-grab
            }
            continue;
        }

        // Delta check degrades gracefully: if no delta reads succeeded
        // (I2C hiccups) persistence alone decides.
        bool deltaOk = (c.deltaSamples == 0) ||
                       ((uint32_t)c.goodDeltaSamples * 100 >=
                        (uint32_t)c.deltaSamples * EXPECT_ANY_DELTA_GOOD_PCT);

        if (deltaOk && m_inputs[i].currentTouched) {
            fireExpectAny(i);
            c.active = false;
        } else if (m_inputs[i].currentTouched) {
            // Still in contact but delta was not consistently grab-like yet
            // (e.g. hand still settling onto the hold). Restart the window
            // instead of rejecting - a sustained grab passes eventually.
            c.startTime = now;
            c.deltaSamples = 0;
            c.goodDeltaSamples = 0;
            c.untouchedStreak = 0;
        } else {
            c.active = false;
        }
    }
}

void TouchController::processHandsOffDetection() {
    if (!m_handsOffDetectionEnabled) return;

    bool any = anyInputTouched();
    if (any == m_lastAnyTouched) return;
    m_lastAnyTouched = any;

    if (!m_eventQueue) return;
    if (any) {
        m_eventQueue->queueHandsOn();
    } else {
        m_eventQueue->queueHandsOff();
    }
}
