/**
 * @file StartupController.h
 * @brief Manages hardware initialization and Pi handshake during setup()
 * 
 * Called from setup() before any FreeRTOS tasks are created.
 * 
 * Step 1 — LED initialization with visual-confirmation sweep
 * Step 2 — Sensor initialization with retries
 * Step 3 — Handshake loop: broadcasts a single status message until the Pi ACKs
 */

#ifndef STARTUP_CONTROLLER_H
#define STARTUP_CONTROLLER_H

#include <Arduino.h>
#include "Config.h"

class LedController;
class TouchController;

// ============================================================================
// StartupController Class
// ============================================================================

class StartupController {
public:
    StartupController(LedController& ledController,
                      TouchController& touchController,
                      HardwareSerial& serial);

    /**
     * @brief Run full startup sequence (blocks until HARDWARE INITIALISED is sent)
     * 
     * 1. Initializes LEDs and plays a visual-confirmation sweep
     * 2. Initializes touch sensors with retries, builds status message
     * 3. Enters handshake loop broadcasting status every 3s until ACK received
     * 4. Sends "HARDWARE INITIALISED" and returns
     */
    void run();

private:
    LedController& m_ledController;
    TouchController& m_touchController;
    HardwareSerial& m_serial;

    // Single status message (e.g. "SENSORS READY" or "SENSORS FAILED [H01,H02]")
    char m_statusMsg[SENSOR_LIST_BUFFER_SIZE + 20];

    // Step 1: LED init + sweep animation
    void initLeds();
    void runLedSweepAnimation();

    // Step 2: Sensor init with retries
    void initSensors();

    // Step 3: Handshake loop
    void handshakeLoop();
    bool isAckMatch(const char* line) const;

    // Non-blocking serial line reader
    static constexpr uint8_t LINE_BUF_SIZE = 80;
    char m_lineBuf[LINE_BUF_SIZE];
    uint8_t m_linePos;
    bool readLine();
};

#endif // STARTUP_CONTROLLER_H
