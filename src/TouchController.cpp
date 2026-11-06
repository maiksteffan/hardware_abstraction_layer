/**
 * @file TouchController.cpp
 * @brief Implementation of multi-channel CAP1188 touch input controller
 *
 * Indexing model:
 *   - "sensor index" (0..sensorCount-1) addresses one physical CAP1188 chip.
 *   - "input index"  (0..holdCount-1)   addresses one logical H01..H{holdCount} input.
 *   - inputMappings[i] = {sensorIndex, channel} resolves input i to hardware.
 *
 * All three come from the active board profile, so the counts below are the
 * array ceilings (MAX_SENSORS / MAX_HOLDS), not the wiring.
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
    , m_busMutex(xSemaphoreCreateMutex())
    , m_lastAnyAnswer(0)
    , m_handsOffDetectionEnabled(false)
    , m_lastAnyTouched(false)
{
    // Cleared to the ceiling rather than the profile: this runs before the Pi
    // has named a profile, so every slot a later profile could use must be sane.
    for (uint8_t s = 0; s < MAX_SENSORS; s++) {
        m_sensors[s].active = false;
        m_sensors[s].enableMask = 0;
        m_sensors[s].lastStatus = 0;
        m_sensors[s].statusValid = false;        m_sensors[s].degraded = false;
        m_sensors[s].failStreak = 0;
        m_sensors[s].sensitivity = CAP1188_DEFAULT_SENSITIVITY;
        m_sensors[s].lastProbe = 0;    }
    for (uint8_t i = 0; i < MAX_HOLDS; i++) {
        m_inputs[i].currentTouched = false;
        m_inputs[i].debouncedTouched = false;
        m_inputs[i].lastReportedTouched = false;
        m_inputs[i].lastChangeTime = 0;
        m_inputs[i].lastSampleTime = 0;
        m_inputs[i].releaseEvidenceMs = 0;
        m_inputs[i].gripBridgeMs = 0;
        m_inputs[i].pressStartTime = 0;
        m_inputs[i].pressConsumed = false;

        m_expectDown[i] = {false, false, COMMAND_ID_NONE, 0};
        m_expectUp[i] = {false, false, COMMAND_ID_NONE, 0};
    }
    for (uint8_t i = 0; i < EXPECT_ANY_QUEUE_SIZE; i++) {
        m_expectAnyQueue[i].active = false;
        m_expectAnyQueue[i].commandId = COMMAND_ID_NONE;
        m_expectAnyQueue[i].excludeMask.clear();
    }
    memset(m_anyCandidates, 0, sizeof(m_anyCandidates));
    memset(m_pressEdgeRing, 0, sizeof(m_pressEdgeRing));
    m_pendingAnyTouch = {false, false, COMMAND_ID_NONE, 0};
    m_pendingAnyInput = 0;
    m_touchStateRequest = {false, false, COMMAND_ID_NONE, 0};
    m_touchStateInput = 0;
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

    const BoardProfile& profile = activeBoardProfile();

    // Reset sensor state
    m_lastAnyAnswer = millis();
    for (uint8_t s = 0; s < MAX_SENSORS; s++) {
        m_sensors[s].active = false;
        m_sensors[s].enableMask = 0;
        m_sensors[s].lastStatus = 0;
        m_sensors[s].statusValid = false;
        m_sensors[s].degraded = false;
        m_sensors[s].failStreak = 0;
        m_sensors[s].sensitivity = CAP1188_DEFAULT_SENSITIVITY;
        m_sensors[s].lastProbe = millis();
    }

    // Build per-sensor enable mask from the profile's input mappings
    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t s = profile.inputMappings[i].sensorIndex;
        uint8_t ch = profile.inputMappings[i].channel;
        if (s < profile.sensorCount && ch < 8) {
            m_sensors[s].enableMask |= (uint8_t)(1 << ch);
        }
    }

    // Initialize each physical sensor
    m_activeSensorCount = 0;
    for (uint8_t s = 0; s < profile.sensorCount; s++) {
        if (initSensor(s)) {
            m_sensors[s].active = true;
            m_activeSensorCount++;
        }
        delay(10);
    }

    // Reset input state
    for (uint8_t i = 0; i < MAX_HOLDS; i++) {
        m_inputs[i].currentTouched = false;
        m_inputs[i].debouncedTouched = false;
        m_inputs[i].lastReportedTouched = false;
        m_inputs[i].lastChangeTime = 0;
        m_inputs[i].lastSampleTime = 0;
        m_inputs[i].releaseEvidenceMs = 0;
        m_inputs[i].gripBridgeMs = 0;
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
    processPointExpectations();
    processExpectAnyQualification();
    processPendingAnyTouch();
    processHandsOffDetection();
    processTouchStateRequest();
}

bool TouchController::recalibrate(uint8_t inputIndex) {
    const BoardProfile& profile = activeBoardProfile();
    if (inputIndex >= profile.holdCount) return false;
    uint8_t s = profile.inputMappings[inputIndex].sensorIndex;
    uint8_t ch = profile.inputMappings[inputIndex].channel;
    if (s >= profile.sensorCount || !m_sensors[s].active) return false;

    return writeRegister(profile.sensorAddresses[s],
                         CAP1188_REG_CALIBRATION_ACTIVE,
                         (uint8_t)(1 << ch));
}

void TouchController::recalibrateAll() {
    for (uint8_t s = 0; s < activeBoardProfile().sensorCount; s++) {
        if (!m_sensors[s].active) continue;
        writeRegister(activeBoardProfile().sensorAddresses[s],
                      CAP1188_REG_CALIBRATION_ACTIVE,
                      m_sensors[s].enableMask);
    }
}

bool TouchController::setSensitivity(uint8_t inputIndex, uint8_t level) {
    const BoardProfile& profile = activeBoardProfile();
    if (inputIndex >= profile.holdCount) return false;
    if (level > 7) return false;
    uint8_t s = profile.inputMappings[inputIndex].sensorIndex;
    if (s >= profile.sensorCount || !m_sensors[s].active) return false;

    // CAP1188 sensitivity is GLOBAL per chip (bits 6:4 of SENSITIVITY_CONTROL).
    // 0 = 128x (most sensitive), 7 = 1x (least sensitive).
    uint8_t address = profile.sensorAddresses[s];
    uint8_t regValue;
    if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue)) {
        return false;
    }
    regValue = (uint8_t)((regValue & 0x8F) | (level << 4));
    // Publish the wanted level BEFORE writing: the polling core verifies the
    // chip against it and would otherwise "repair" the new level back to the old one.
    uint8_t previous = m_sensors[s].sensitivity;
    m_sensors[s].sensitivity = level;
    if (!writeRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, regValue)) {
        m_sensors[s].sensitivity = previous;
        return false;
    }
    return true;
}

void TouchController::setExpectDown(uint8_t inputIndex, uint32_t commandId) {
    if (inputIndex >= activeBoardProfile().holdCount) return;
    // Publish atomically; only the polling core may inspect/consume a press.
    // Checking a held state here and then arming left a race in which the
    // press edge occurred between those two operations and was lost forever.
    portENTER_CRITICAL(&m_expectationMux);
    PointExpectation& expectation = m_expectDown[inputIndex];
    expectation.commandId = commandId;
    expectation.pending = false;
    expectation.revision++;
    expectation.active = true;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::setExpectUp(uint8_t inputIndex, uint32_t commandId) {
    if (inputIndex >= activeBoardProfile().holdCount) return;

    portENTER_CRITICAL(&m_expectationMux);
    PointExpectation& expectation = m_expectUp[inputIndex];
    expectation.commandId = commandId;
    expectation.pending = false;
    expectation.revision++;
    expectation.active = true;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::clearExpectDown(uint8_t inputIndex) {
    if (inputIndex >= activeBoardProfile().holdCount) return;
    portENTER_CRITICAL(&m_expectationMux);
    m_expectDown[inputIndex].active = false;
    m_expectDown[inputIndex].pending = false;
    m_expectDown[inputIndex].revision++;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::clearExpectUp(uint8_t inputIndex) {
    if (inputIndex >= activeBoardProfile().holdCount) return;
    portENTER_CRITICAL(&m_expectationMux);
    m_expectUp[inputIndex].active = false;
    m_expectUp[inputIndex].pending = false;
    m_expectUp[inputIndex].revision++;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::setExpectAny(uint32_t commandId) {
    HoldMask excludeNothing;
    excludeNothing.clear();
    setExpectAnyExcept(excludeNothing, commandId);
}

void TouchController::setExpectAnyExcept(const HoldMask& excludeMask, uint32_t commandId) {
    portENTER_CRITICAL(&m_expectationMux);
    // Enqueue into circular buffer
    m_expectAnyQueue[m_expectAnyHead].active = true;
    m_expectAnyQueue[m_expectAnyHead].commandId = commandId;
    m_expectAnyQueue[m_expectAnyHead].excludeMask = excludeMask;
    m_expectAnyHead = (m_expectAnyHead + 1) % EXPECT_ANY_QUEUE_SIZE;

    // If head catches tail, advance tail (drop oldest)
    if (m_expectAnyHead == m_expectAnyTail) {
        m_expectAnyTail = (m_expectAnyTail + 1) % EXPECT_ANY_QUEUE_SIZE;
    }
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::clearExpectAny() {
    portENTER_CRITICAL(&m_expectationMux);
    for (uint8_t i = 0; i < EXPECT_ANY_QUEUE_SIZE; i++) {
        m_expectAnyQueue[i].active = false;
        m_expectAnyQueue[i].commandId = COMMAND_ID_NONE;
        m_expectAnyQueue[i].excludeMask.clear();
    }
    m_expectAnyHead = 0;
    m_expectAnyTail = 0;
    m_pendingAnyTouch.active = false;
    m_pendingAnyTouch.revision++;
    // Drop any in-flight qualification candidates as well.
    memset(m_anyCandidates, 0, sizeof(m_anyCandidates));
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::clearAllExpectations() {
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        clearExpectDown(i);
        clearExpectUp(i);
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
    const BoardProfile& profile = activeBoardProfile();
    if (bufferSize == 0) return;

    buffer[0] = '\0';
    size_t pos = 0;
    bool first = true;

    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t s = profile.inputMappings[i].sensorIndex;
        if (s >= profile.sensorCount || !m_sensors[s].active) continue;

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
    const BoardProfile& profile = activeBoardProfile();
    if (bufferSize == 0) return;

    buffer[0] = '\0';
    size_t pos = 0;
    bool first = true;

    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t s = profile.inputMappings[i].sensorIndex;
        if (s >= profile.sensorCount) continue;
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
    const BoardProfile& profile = activeBoardProfile();
    if (inputIndex >= profile.holdCount) return false;
    uint8_t s = profile.inputMappings[inputIndex].sensorIndex;
    if (s >= profile.sensorCount) return false;
    return m_sensors[s].active;
}

bool TouchController::isTouched(uint8_t inputIndex) const {
    if (inputIndex >= activeBoardProfile().holdCount) return false;
    return m_inputs[inputIndex].debouncedTouched;
}

uint8_t TouchController::getActiveSensorCount() const {
    return m_activeSensorCount;
}

bool TouchController::readSensorValue(uint8_t inputIndex, int8_t& value) {
    const BoardProfile& profile = activeBoardProfile();
    if (inputIndex >= profile.holdCount) return false;
    uint8_t s = profile.inputMappings[inputIndex].sensorIndex;
    uint8_t ch = profile.inputMappings[inputIndex].channel;
    if (s >= profile.sensorCount || !m_sensors[s].active) return false;

    uint8_t rawValue;
    if (!readRegister(profile.sensorAddresses[s],
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
    const BoardProfile& profile = activeBoardProfile();
    if (sensorIndex >= profile.sensorCount) return false;
    uint8_t address = profile.sensorAddresses[sensorIndex];
    m_sensors[sensorIndex].lastProbe = millis();

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

    // Never let the chip recalibrate a held touch (see CAP1188_CONFIG1_MAX_DUR_EN).
    uint8_t cfg1;
    if (!readRegister(address, CAP1188_REG_CONFIG1, cfg1)) return false;
    if ((cfg1 & CAP1188_CONFIG1_MAX_DUR_EN) &&
        !writeRegister(address, CAP1188_REG_CONFIG1,
                       (uint8_t)(cfg1 & ~CAP1188_CONFIG1_MAX_DUR_EN))) return false;

    // Apply the chip's current sensitivity (bits 6:4 of SENSITIVITY_CONTROL, global per chip)
    uint8_t sens;
    if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, sens)) return false;
    sens = (uint8_t)((sens & 0x8F) | (m_sensors[sensorIndex].sensitivity << 4));
    if (!writeRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, sens)) return false;

    // Enable exactly the channels referenced by profile.inputMappings for this sensor.
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
    if (sensorIndex >= activeBoardProfile().sensorCount) {
        if (diagSize) snprintf(outDiag, diagSize, "S? FAIL bad_index");
        return false;
    }
    uint8_t address = activeBoardProfile().sensorAddresses[sensorIndex];

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

    // One init sequence for boot, diagnostics and recovery: this path used to
    // duplicate a subset of it and skipped the sensitivity, so a chip first
    // brought up here ran on the chip default.
    bool wasActive = m_sensors[sensorIndex].active;
    if (!initSensor(sensorIndex)) {
        snprintf(outDiag, diagSize, "S%u@0x%02X FAIL init pid=0x%02X",
                 sensorIndex, address, prodId);
        if (wasActive && m_activeSensorCount > 0) m_activeSensorCount--;
        m_sensors[sensorIndex].active = false;
        return false;
    }

    // Read-only snapshot of the chip's own recalibration settings: cfg20 (bit3 =
    // max-duration recal, must be 0), sic22, rc2F, cfg44. Lets a bench session
    // check them against the datasheet.
    uint8_t cfg1 = 0, sic = 0, rc = 0, cfg2 = 0;
    readRegister(address, CAP1188_REG_CONFIG1, cfg1);
    readRegister(address, CAP1188_REG_SENSOR_INPUT_CONFIG, sic);
    readRegister(address, CAP1188_REG_RECAL_CONFIG, rc);
    readRegister(address, CAP1188_REG_CONFIG2, cfg2);
    snprintf(outDiag, diagSize,
             "S%u@0x%02X OK pid=0x%02X mask=0x%02X cfg20=0x%02X sic22=0x%02X rc2F=0x%02X cfg44=0x%02X",
             sensorIndex, address, prodId, m_sensors[sensorIndex].enableMask,
             cfg1, sic, rc, cfg2);
    if (!wasActive) m_activeSensorCount++;
    m_sensors[sensorIndex].active = true;
    m_sensors[sensorIndex].degraded = false;
    m_sensors[sensorIndex].failStreak = 0;
    m_sensors[sensorIndex].lastProbe = millis();
    return true;
}

bool TouchController::lockBus() {
    if (!m_busMutex) return true;
    return xSemaphoreTake(m_busMutex, pdMS_TO_TICKS(I2C_BUS_LOCK_TIMEOUT_MS)) == pdTRUE;
}

void TouchController::unlockBus() {
    if (m_busMutex) xSemaphoreGive(m_busMutex);
}

bool TouchController::readRegisterOnce(uint8_t address, uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom(address, (uint8_t)1) == 1) {
        value = Wire.read();
        return true;
    }
    return false;
}

bool TouchController::writeRegisterOnce(uint8_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

// Each attempt takes the bus lock on its own, so a retry never keeps the other
// core waiting. Failing to get the lock is reported as a failed transfer rather
// than retried: the caller already treats that as "no sample this poll".
bool TouchController::readRegister(uint8_t address, uint8_t reg, uint8_t& value) {
    for (uint8_t attempt = 0; attempt < I2C_RETRY_COUNT; attempt++) {
        if (attempt > 0) delayMicroseconds(I2C_RETRY_DELAY_US);
        if (!lockBus()) return false;
        bool ok = readRegisterOnce(address, reg, value);
        unlockBus();
        if (ok) return true;
    }
    return false;
}

bool TouchController::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    for (uint8_t attempt = 0; attempt < I2C_RETRY_COUNT; attempt++) {
        if (attempt > 0) delayMicroseconds(I2C_RETRY_DELAY_US);
        if (!lockBus()) return false;
        bool ok = writeRegisterOnce(address, reg, value);
        unlockBus();
        if (ok) return true;
    }
    return false;
}

void TouchController::resetBus() {
    if (!lockBus()) return;
    Wire.end();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_CLOCK_SPEED_HZ);
    unlockBus();
}

// Re-applies the init sequence to a chip that never came up (boot failure).
// Chips that dropped off the bus later are not handled here: they stay active,
// are polled every cycle and are re-verified on their first good read.
void TouchController::recoverSensor(uint8_t sensorIndex, uint32_t now) {
    PhysicalSensor& sensor = m_sensors[sensorIndex];
    if (sensor.active || now - sensor.lastProbe < SENSOR_RECOVERY_INTERVAL_MS) return;

    if (!initSensor(sensorIndex)) {
        sensor.lastProbe = now;
        return;
    }
    sensor.active = true;
    sensor.degraded = false;
    sensor.failStreak = 0;
    m_activeSensorCount++;
}

// A silently reset chip answers normally but behaves differently: multi-touch
// blocking comes back (a second simultaneous hold is dropped), every channel is
// enabled and sensitivity reverts to the chip default. Only registers the
// firmware itself wrote are compared. A failed read is ignored here: the status
// polling tracks communication health.
void TouchController::verifySensorConfig(uint8_t sensorIndex) {
    PhysicalSensor& sensor = m_sensors[sensorIndex];
    uint8_t address = activeBoardProfile().sensorAddresses[sensorIndex];
    uint8_t value;

    if (!readRegister(address, CAP1188_REG_MULTIPLE_TOUCH_CONFIG, value)) return;
    bool intact = (value & 0x80) == 0;  // MULT_BLK_EN must stay off

    if (intact) {
        if (!readRegister(address, CAP1188_REG_CONFIG1, value)) return;
        intact = (value & CAP1188_CONFIG1_MAX_DUR_EN) == 0;
    }
    if (intact && sensor.enableMask != 0) {
        if (!readRegister(address, CAP1188_REG_SENSOR_INPUT_ENABLE, value)) return;
        intact = value == sensor.enableMask;
    }
    if (intact) {
        if (!readRegister(address, CAP1188_REG_SENSITIVITY_CONTROL, value)) return;
        intact = ((value >> 4) & 0x07) == sensor.sensitivity;
    }
    if (!intact) initSensor(sensorIndex);
}

void TouchController::pollSensors() {
    const BoardProfile& profile = activeBoardProfile();
    uint32_t now = millis();

    // Step 1: read each active sensor's status register once and cache it.
    // A chip that keeps failing is demoted to single-attempt reads, so a dead
    // chip cannot eat the poll cycle of the healthy ones, but it is still polled
    // every cycle: the first good read resumes sensing at once. A chip that
    // failed at boot is re-initialized periodically and joins when it answers.
    bool anyValid = false;
    for (uint8_t s = 0; s < profile.sensorCount; s++) {
        PhysicalSensor& sensor = m_sensors[s];
        sensor.statusValid = false;
        if (!sensor.active) {
            recoverSensor(s, now);
            continue;
        }

        uint8_t status = 0;
        bool ok = false;
        if (sensor.degraded) {
            if (lockBus()) {
                ok = readRegisterOnce(profile.sensorAddresses[s],
                                      CAP1188_REG_SENSOR_INPUT_STATUS, status);
                unlockBus();
            }
        } else {
            ok = readRegister(profile.sensorAddresses[s],
                              CAP1188_REG_SENSOR_INPUT_STATUS, status);
        }

        if (ok) {
            sensor.lastStatus = status;
            sensor.statusValid = true;
            sensor.failStreak = 0;
            anyValid = true;
            if (sensor.degraded) {
                // The chip may have been power-cycled while it was away:
                // check its configuration now, not at the next 1 s tick.
                sensor.degraded = false;
                sensor.lastProbe = now - SENSOR_VERIFY_INTERVAL_MS;
            }
            if (now - sensor.lastProbe >= SENSOR_VERIFY_INTERVAL_MS) {
                sensor.lastProbe = now;
                verifySensorConfig(s);
            }
        } else if (!sensor.degraded &&
                   ++sensor.failStreak >= SENSOR_DEGRADE_AFTER_FAILED_POLLS) {
            sensor.degraded = true;
        }
    }
    // Only when NOTHING answers is the bus itself suspect (a slave holding SDA
    // low after a mid-transfer reset takes every chip down). One dead chip on a
    // working bus never triggers this.
    if (anyValid || profile.sensorCount == 0) {
        m_lastAnyAnswer = now;
    } else if (now - m_lastAnyAnswer >= I2C_BUS_RESET_AFTER_SILENT_MS) {
        m_lastAnyAnswer = now;
        resetBus();
    }

    // Step 2: update per-input "current" state from cached status registers.
    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t s = profile.inputMappings[i].sensorIndex;
        uint8_t ch = profile.inputMappings[i].channel;
        if (s >= profile.sensorCount || !m_sensors[s].active) continue;
        if (!m_sensors[s].statusValid) {
            // An error supplies NO release evidence, but must not erase the
            // evidence already observed: occasional failures otherwise keep
            // debouncedTouched/pressConsumed latched indefinitely.
            m_inputs[i].lastSampleTime = now;
            continue;
        }

        bool touched = ((m_sensors[s].lastStatus >> ch) & 0x01) != 0;
        TouchInputState& input = m_inputs[i];
        uint32_t elapsed = now - input.lastSampleTime;
        input.lastSampleTime = now;
        // Count actual scan cadence (not an assumed ideal 5ms), but never
        // credit a long unobserved bus stall or a failed poll as a release.
        if (touched) {
            input.releaseEvidenceMs = 0;
            input.gripBridgeMs = 0;
        } else if (!input.currentTouched && input.debouncedTouched) {
            uint32_t credit = elapsed <= TOUCH_RELEASE_MAX_SAMPLE_GAP_MS ? elapsed : 0;

            // A dip of the status bit with the delta still high is a hand that
            // has not left (grip shifting, marginal signal), not a release.
            // Costs one register read per poll, only while a held hold is in
            // its release latch, and only until the bridge budget is spent. A
            // failed read gives no bridge, i.e. the plain latch behavior.
            bool gripping = false;
            if (input.gripBridgeMs < TOUCH_GRIP_BRIDGE_MAX_MS) {
                int8_t delta = 0;
                gripping = readSensorValue(i, delta) && delta >= TOUCH_GRIP_DELTA_MIN;
            }

            if (gripping) {
                uint32_t bridged = input.gripBridgeMs + credit;
                input.gripBridgeMs = static_cast<uint16_t>(
                    bridged < TOUCH_GRIP_BRIDGE_MAX_MS ? bridged : TOUCH_GRIP_BRIDGE_MAX_MS);
                input.releaseEvidenceMs = 0;
            } else {
                uint32_t evidence = input.releaseEvidenceMs + credit;
                input.releaseEvidenceMs = static_cast<uint16_t>(
                    evidence < TOUCH_DEBOUNCE_RELEASE_MS ? evidence : TOUCH_DEBOUNCE_RELEASE_MS);
            }
        }

        if (touched != m_inputs[i].currentTouched) {
            m_inputs[i].currentTouched = touched;
            // A short dropout cancels EXPECT_ANY qualification well before
            // the release latch expires. Resume qualification on re-contact
            // without turning a consumed grab into a second press event.
            if (touched && m_inputs[i].debouncedTouched &&
                !m_inputs[i].pressConsumed &&
                now - m_inputs[i].pressStartTime <= EXPECT_HELD_FRESH_MS) {
                startAnyCandidate(i, now);
            }
            // Only reset debounce timer if the new state differs from debounced state
            // (prevents noise from resetting the timer while holding a touch)
            if (touched != m_inputs[i].debouncedTouched) {
                m_inputs[i].lastChangeTime = now;
            }
        }
    }

    // Step 3: clear the INT bit on every sensor that reported any active channel,
    // so that future touches are detected.
    for (uint8_t s = 0; s < profile.sensorCount; s++) {
        if (!m_sensors[s].active || !m_sensors[s].statusValid) continue;
        if (m_sensors[s].lastStatus == 0) continue;

        uint8_t mc;
        if (readRegister(profile.sensorAddresses[s], CAP1188_REG_MAIN_CONTROL, mc)) {
            writeRegister(profile.sensorAddresses[s],
                          CAP1188_REG_MAIN_CONTROL, (uint8_t)(mc & ~0x01));
        }
    }
}

void TouchController::processDebounce() {
    const BoardProfile& profile = activeBoardProfile();
    uint32_t now = millis();

    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t s = profile.inputMappings[i].sensorIndex;
        if (s >= profile.sensorCount || !m_sensors[s].active) continue;
        if (!m_sensors[s].statusValid) continue;

        TouchInputState& input = m_inputs[i];

        if (input.currentTouched == input.debouncedTouched) continue;

        uint32_t elapsed = now - input.lastChangeTime;
        uint16_t requiredDebounce = input.currentTouched
                                        ? TOUCH_DEBOUNCE_PRESS_MS
                                        : TOUCH_DEBOUNCE_RELEASE_MS;
        if (!input.currentTouched) elapsed = input.releaseEvidenceMs;
        if (elapsed < requiredDebounce) continue;

        input.debouncedTouched = input.currentTouched;
        if (input.debouncedTouched == input.lastReportedTouched) continue;
        input.lastReportedTouched = input.debouncedTouched;

        if (input.debouncedTouched) {
            input.pressStartTime = now;
            input.pressConsumed = false;  // fresh press edge: may satisfy one EXPECT
            recordPressEdge(now);

            // Track every new grab, even between Pi expectations. Otherwise
            // releasing one hold and grabbing the next before re-arming loses
            // that next press forever. No event is emitted until an eligible
            // EXPECT_ANY exists and the normal brush-by checks have passed.
            startAnyCandidate(i, now);
        } else {
            // Debounced release: any pending candidate is stale by now.
            m_anyCandidates[i].active = false;
        }
    }
}

void TouchController::processPointExpectations() {
    if (!m_eventQueue) return;
    const BoardProfile& profile = activeBoardProfile();
    uint32_t now = millis();
    for (uint8_t i = 0; i < profile.holdCount; i++) {
        uint8_t sensor = profile.inputMappings[i].sensorIndex;
        if (sensor >= profile.sensorCount || !m_sensors[sensor].active) continue;
        for (uint8_t direction = 0; direction < 2; direction++) {
            PointExpectation& expectation = direction == 0 ? m_expectDown[i] : m_expectUp[i];
            TouchInputState& input = m_inputs[i];
            portENTER_CRITICAL(&m_expectationMux);
            bool matches = m_sensors[sensor].statusValid && (direction == 0
                ? (input.currentTouched && input.debouncedTouched && !input.pressConsumed &&
                   now - input.pressStartTime <= EXPECT_HELD_FRESH_MS)
                : !input.debouncedTouched);
            if (expectation.active && !expectation.pending && matches) {
                // Claim once, retain until delivery succeeds. A later release
                // or freshness expiry must not erase a touch already detected.
                expectation.pending = true;
                if (direction == 0) input.pressConsumed = true;
            }
            PointExpectation snapshot = expectation;
            portEXIT_CRITICAL(&m_expectationMux);
            if (!snapshot.active || !snapshot.pending) continue;

            char position[POSITION_STRING_LENGTH];
            CommandController::indexToPosition(i, position);
            // EventQueue can wait for a mutex: NEVER call it in a critical section.
            bool queued = direction == 0
                ? m_eventQueue->queueTouched(position, snapshot.commandId, MUTEX_TIMEOUT_TOUCH_QUEUE_MS)
                : m_eventQueue->queueTouchReleased(position, snapshot.commandId, MUTEX_TIMEOUT_TOUCH_QUEUE_MS);
            if (!queued) continue;
            portENTER_CRITICAL(&m_expectationMux);
            if (expectation.revision == snapshot.revision) {
                expectation.active = false;
                expectation.pending = false;
            }
            portEXIT_CRITICAL(&m_expectationMux);
        }
    }
}

bool TouchController::anyInputTouched() const {
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
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

bool TouchController::fireExpectAny(uint8_t inputIndex) {
    if (!m_eventQueue) return false;
    bool claimed = false;
    portENTER_CRITICAL(&m_expectationMux);
    ExpectState& head = m_expectAnyQueue[m_expectAnyTail];
    if (!m_pendingAnyTouch.active && m_expectAnyTail != m_expectAnyHead &&
        head.active && !m_inputs[inputIndex].pressConsumed && !head.excludeMask.test(inputIndex)) {
        m_inputs[inputIndex].pressConsumed = true;
        m_pendingAnyInput = inputIndex;
        m_pendingAnyTouch.commandId = head.commandId;
        m_pendingAnyTouch.revision++;
        m_pendingAnyTouch.active = true;
        head.active = false;
        m_expectAnyTail = (uint8_t)((m_expectAnyTail + 1) % EXPECT_ANY_QUEUE_SIZE);
        claimed = true;
    }
    portEXIT_CRITICAL(&m_expectationMux);
    return claimed;
}

void TouchController::processPendingAnyTouch() {
    if (!m_eventQueue) return;
    portENTER_CRITICAL(&m_expectationMux);
    PointExpectation snapshot = m_pendingAnyTouch;
    uint8_t input = m_pendingAnyInput;
    portEXIT_CRITICAL(&m_expectationMux);
    if (!snapshot.active) return;
    char position[POSITION_STRING_LENGTH];
    CommandController::indexToPosition(input, position);
    if (!m_eventQueue->queueTouched(position, snapshot.commandId, MUTEX_TIMEOUT_TOUCH_QUEUE_MS)) return;
    portENTER_CRITICAL(&m_expectationMux);
    if (m_pendingAnyTouch.revision == snapshot.revision) m_pendingAnyTouch.active = false;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::processExpectAnyQualification() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        AnyCandidate& c = m_anyCandidates[i];
        if (!c.active) continue;

        // EXPECT and EXPECT_ANY share ONE consumption flag for each press.
        // Queue/batch membership must never block a released-and-regrabbed
        // hold, nor allow an EXPECT touch to be reported a second time here.
        if (m_inputs[i].pressConsumed ||
            now - m_inputs[i].pressStartTime > EXPECT_HELD_FRESH_MS) {
            c.active = false;
            continue;
        }

        uint8_t sensor = activeBoardProfile().inputMappings[i].sensorIndex;
        if (sensor >= activeBoardProfile().sensorCount ||
            !m_sensors[sensor].statusValid) continue;

        // ---- 1. Persistence: raw contact must not drop out ----
        if (m_inputs[i].currentTouched) {
            c.untouchedStreak = 0;
        } else if (++c.untouchedStreak >= EXPECT_ANY_DROPOUT_SAMPLES) {
            // Contact vanished: classic brush-by. A real grab produces a new
            // press edge later and re-qualifies from scratch.
            c.active = false;
            continue;
        }

        // Track the raw edge across rearm gaps, but do not perform recording
        // delta I2C traffic during ordinary EXPECT sequence replay. The bus
        // must remain available for the status reads that detect new presses.
        portENTER_CRITICAL(&m_expectationMux);
        bool anyArmed = m_expectAnyTail != m_expectAnyHead;
        portEXIT_CRITICAL(&m_expectationMux);
        if (!anyArmed) continue;

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
        // An excluded hold may become eligible for the NEXT expectation.
        // Keep qualifying it while fresh, rather than silently discarding it
        // when fireExpectAny() refuses the current queue head's exclusion.
        portENTER_CRITICAL(&m_expectationMux);
        bool eligible = !m_pendingAnyTouch.active && m_expectAnyTail != m_expectAnyHead &&
            m_expectAnyQueue[m_expectAnyTail].active &&
            !m_expectAnyQueue[m_expectAnyTail].excludeMask.test(i);
        portEXIT_CRITICAL(&m_expectationMux);
        if (!eligible) {
            continue;
        }

        // Delta check degrades gracefully: if no delta reads succeeded
        // (I2C hiccups) persistence alone decides.
        bool deltaOk = (c.deltaSamples == 0) ||
                       ((uint32_t)c.goodDeltaSamples * 100 >=
                        (uint32_t)c.deltaSamples * EXPECT_ANY_DELTA_GOOD_PCT);

        if (deltaOk && m_inputs[i].currentTouched) {
            if (fireExpectAny(i)) c.active = false;
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

    if (!m_eventQueue) return;
    bool queued = any ? m_eventQueue->queueHandsOn(COMMAND_ID_NONE, MUTEX_TIMEOUT_TOUCH_QUEUE_MS)
                      : m_eventQueue->queueHandsOff(COMMAND_ID_NONE, MUTEX_TIMEOUT_TOUCH_QUEUE_MS);
    // A nonblocking rejection is not an observed transition. Retry on the
    // next poll while the occupancy still differs from the delivered state.
    if (queued) m_lastAnyTouched = any;
}

void TouchController::requestTouchState(uint8_t inputIndex, uint32_t commandId) {
    if (inputIndex >= activeBoardProfile().holdCount) return;
    portENTER_CRITICAL(&m_expectationMux);
    m_touchStateInput = inputIndex;
    m_touchStateRequest.commandId = commandId;
    m_touchStateRequest.revision++;
    m_touchStateRequest.active = true;
    portEXIT_CRITICAL(&m_expectationMux);
}

void TouchController::processTouchStateRequest() {
    if (!m_eventQueue) return;
    portENTER_CRITICAL(&m_expectationMux);
    PointExpectation request = m_touchStateRequest;
    uint8_t inputIndex = m_touchStateInput;
    PointExpectation expectation = m_expectDown[inputIndex];
    portEXIT_CRITICAL(&m_expectationMux);
    if (!request.active) return;
    const BoardProfile& profile = activeBoardProfile();
    uint8_t sensor = profile.inputMappings[inputIndex].sensorIndex;
    if (sensor >= profile.sensorCount) return;
    const TouchInputState& input = m_inputs[inputIndex];
    char position[POSITION_STRING_LENGTH];
    CommandController::indexToPosition(inputIndex, position);
    // All raw/debounce state is owned by this core. No concurrent command-side
    // I2C reads and no clearing/recalibration that could hide the failure.
    char state[sizeof(Event::extra)];
    snprintf(state, sizeof(state),
             "active=%u valid=%u raw=%u held=%u used=%u age_ms=%lu release_ms=%u expect=%u pending=%u",
             static_cast<unsigned>(m_sensors[sensor].active),
             static_cast<unsigned>(m_sensors[sensor].statusValid),
             static_cast<unsigned>(input.currentTouched),
             static_cast<unsigned>(input.debouncedTouched),
             static_cast<unsigned>(input.pressConsumed),
             static_cast<unsigned long>(millis() - input.pressStartTime),
             static_cast<unsigned>(input.releaseEvidenceMs),
             static_cast<unsigned>(expectation.active), static_cast<unsigned>(expectation.pending));
    if (!m_eventQueue->queueTouchState(position, state, request.commandId)) return;
    portENTER_CRITICAL(&m_expectationMux);
    if (m_touchStateRequest.revision == request.revision) m_touchStateRequest.active = false;
    portEXIT_CRITICAL(&m_expectationMux);
}
