/**
 * @file StartupController.cpp
 * @brief Implementation of hardware initialization and Pi handshake
 * 
 * Called from setup() before FreeRTOS tasks are created.
 * Blocks until the Pi acknowledges the sensor status message.
 */

#include "StartupController.h"
#include "LedController.h"
#include "TouchController.h"

// ============================================================================
// Constructor
// ============================================================================

StartupController::StartupController(LedController& ledController,
                                     TouchController& touchController,
                                     HardwareSerial& serial)
    : m_ledController(ledController)
    , m_touchController(touchController)
    , m_serial(serial)
    , m_linePos(0)
{
    m_statusMsg[0] = '\0';
    m_lineBuf[0] = '\0';
}

// ============================================================================
// Public: run()
// ============================================================================

void StartupController::run() {
    // Step 1 — LED initialization + visual-confirmation sweep
    initLeds();

    // Step 2 — Sensor initialization with retries, builds m_statusMsg
    initSensors();

    // Step 3 — Handshake loop (blocks until ACK received)
    handshakeLoop();

    // Signal completion
    m_serial.println("HARDWARE INITIALISED");
}

// ============================================================================
// Step 1: LED Initialization
// ============================================================================

void StartupController::initLeds() {
    m_ledController.begin();
    delay(LED_INIT_DELAY_MS);
    runLedSweepAnimation();
}

void StartupController::runLedSweepAnimation() {
    // Sweep a single white pixel across every LED on both strips.
    // 8 ms per pixel, brightness 80.  Fire-and-forget visual confirmation.

    const uint8_t brightness = 80;

    // Strip 1
    uint16_t count1 = m_ledController.getLedCount(StripId::STRIP1);
    for (uint16_t i = 0; i < count1; i++) {
        if (i > 0) {
            m_ledController.setPixelColor(StripId::STRIP1, i - 1, 0, 0, 0);
        }
        m_ledController.setPixelColor(StripId::STRIP1, i, brightness, brightness, brightness);
        m_ledController.showStrip();
        delay(8);
    }
    // Turn off the last pixel of strip 1
    if (count1 > 0) {
        m_ledController.setPixelColor(StripId::STRIP1, count1 - 1, 0, 0, 0);
    }

    // Strip 2
    uint16_t count2 = m_ledController.getLedCount(StripId::STRIP2);
    for (uint16_t i = 0; i < count2; i++) {
        if (i > 0) {
            m_ledController.setPixelColor(StripId::STRIP2, i - 1, 0, 0, 0);
        }
        m_ledController.setPixelColor(StripId::STRIP2, i, brightness, brightness, brightness);
        m_ledController.showStrip();
        delay(8);
    }

    // Clear everything
    m_ledController.clearAll();
}

// ============================================================================
// Step 2: Sensor Initialization
// ============================================================================

void StartupController::initSensors() {
    bool allFound = false;

    for (uint8_t attempt = 0; attempt < SENSOR_INIT_MAX_RETRIES; attempt++) {
        m_touchController.begin();
        delay(SENSOR_INIT_DELAY_MS);

        if (m_touchController.getActiveSensorCount() >= TOUCH_SENSOR_COUNT) {
            allFound = true;
            break;
        }

        // Pause between retries (not after the last attempt)
        if (attempt + 1 < SENSOR_INIT_MAX_RETRIES) {
            delay(100);
        }
    }

    // ---- Emit diagnostics on the wire (Pi ignores unknown keywords) -------
    // I²C bus scan
    {
        char scanBuf[160];
        uint8_t n = m_touchController.scanI2CBus(scanBuf, sizeof(scanBuf));
        m_serial.print("DIAG I2C_SCAN count=");
        m_serial.print(n);
        m_serial.print(" devices=[");
        m_serial.print(scanBuf);
        m_serial.println("]");
    }
    // Per-sensor init outcome (re-runs init once more to capture the reason
    // each sensor passed/failed). diagInitSensor() updates the active flag
    // accordingly, so this also acts as a final init pass.
    for (uint8_t s = 0; s < TOUCH_SENSOR_COUNT; s++) {
        char diag[96];
        m_touchController.diagInitSensor(s, diag, sizeof(diag));
        m_serial.print("DIAG ");
        m_serial.println(diag);
    }

    // Recount active inputs from the active-list builder (source of truth).
    uint8_t activeCount = 0;
    {
        char tmp[SENSOR_LIST_BUFFER_SIZE];
        m_touchController.buildActiveSensorList(tmp, sizeof(tmp));
        if (tmp[0] != '\0') {
            activeCount = 1;
            for (const char* p = tmp; *p; ++p) if (*p == ',') activeCount++;
        }
    }
    // If every logical input is covered we still report READY, otherwise FAILED.
    allFound = (activeCount >= INPUT_COUNT);

    if (allFound) {
        strncpy(m_statusMsg, "SENSORS READY", sizeof(m_statusMsg) - 1);
        m_statusMsg[sizeof(m_statusMsg) - 1] = '\0';
    } else {
        // Pi protocol: bracketed list contains the *active* inputs, not failed.
        // See pi5_programm/.../esp32_protocol.py::_handle_sensors().
        char activeList[SENSOR_LIST_BUFFER_SIZE];
        m_touchController.buildActiveSensorList(activeList, sizeof(activeList));
        snprintf(m_statusMsg, sizeof(m_statusMsg), "SENSORS FAILED [%s]", activeList);
    }
}

// ============================================================================
// Step 3: Handshake Loop
// ============================================================================

void StartupController::handshakeLoop() {
    // Send the first broadcast immediately
    m_serial.println(m_statusMsg);
    uint32_t lastSendTime = millis();

    for (;;) {
        // Non-blockingly read serial input, checking for ACK
        if (readLine()) {
            if (isAckMatch(m_lineBuf)) {
                return;  // ACK received — caller will send HARDWARE INITIALISED
            }
        }

        // Re-broadcast every STARTUP_HANDSHAKE_INTERVAL_MS
        uint32_t now = millis();
        if (now - lastSendTime >= STARTUP_HANDSHAKE_INTERVAL_MS) {
            m_serial.println(m_statusMsg);
            lastSendTime = now;
        }

        delay(1);  // Yield to avoid tight-spinning
    }
}

bool StartupController::isAckMatch(const char* line) const {
    // Must start with "ACK "
    if (strncmp(line, "ACK ", 4) != 0) return false;

    const char* payload = line + 4;

    if (strcmp(m_statusMsg, "SENSORS READY") == 0) {
        // Exact match required
        return strcmp(payload, "SENSORS READY") == 0;
    }

    // Status starts with "SENSORS FAILED" → prefix match (ignore bracket contents)
    if (strncmp(m_statusMsg, "SENSORS FAILED", 14) == 0) {
        return strncmp(payload, "SENSORS FAILED", 14) == 0;
    }

    return false;
}

// ============================================================================
// Non-blocking Serial Line Reader
// ============================================================================

bool StartupController::readLine() {
    while (m_serial.available()) {
        char c = static_cast<char>(m_serial.read());

        if (c == '\n' || c == '\r') {
            if (m_linePos == 0) continue;  // Skip empty lines from \r\n sequences
            m_lineBuf[m_linePos] = '\0';
            m_linePos = 0;
            return true;   // Full line ready in m_lineBuf
        }

        if (m_linePos < LINE_BUF_SIZE - 1) {
            m_lineBuf[m_linePos++] = c;
        }
        // else: overflow — silently drop extra chars until newline
    }
    return false;
}
