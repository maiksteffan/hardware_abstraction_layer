/**
 * @file StartupController.h
 * @brief Manages hardware initialization and Pi handshake during setup()
 * 
 * Called from setup() before any FreeRTOS tasks are created.
 * 
 * Step 1 — Announce INFO, then wait for the Pi to name a board profile
 * Step 2 — LED initialization with visual-confirmation sweep
 * Step 3 — Sensor initialization with retries
 * Step 4 — Handshake loop: broadcasts a single status message until the Pi ACKs
 *
 * The profile must be settled before step 2: LED and sensor init read the
 * wiring tables, so a profile chosen later would come too late to matter.
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
                      Stream& serial);

    /**
     * @brief Run full startup sequence (blocks until HARDWARE INITIALISED is sent)
     *
     * 1. Announces INFO (firmware, protocol, available profiles) and waits up
     *    to BOARD_VERSION_TIMEOUT_MS for the Pi's BOARD_VERSION; falls back to
     *    the default profile on timeout or an unknown slug
     * 2. Initializes LEDs and plays a visual-confirmation sweep
     * 3. Initializes touch sensors with retries, builds status message
     * 4. Enters handshake loop broadcasting status every 3s until ACK received
     * 5. Sends the resolved INFO followed by "HARDWARE INITIALISED" and returns
     */
    void run();

private:
    LedController& m_ledController;
    TouchController& m_touchController;
    Stream& m_serial;

    // Single status message (e.g. "SENSORS READY" or "SENSORS FAILED [H01,H02]")
    //
    // Sized to hold the full active-input list: at 64 bytes the list was
    // silently truncated as soon as a single CAP1188 chip failed (27 active
    // inputs already need 124 characters), and the Pi derives the failed set
    // from exactly this list.
    char m_statusMsg[SENSOR_LIST_BUFFER_SIZE + 32];

    // Step 1: profile negotiation
    void sendInfo(bool withSelection);
    void awaitBoardVersion();
    // Activate `slug`, answering ACK BOARD_VERSION or ERR unknown_board_version.
    void applyBoardVersion(const char* slug);

    // Step 2: LED init + sweep animation
    void initLeds();
    void runLedSweepAnimation();

    // Step 3: Sensor init with retries
    void initSensors();

    // Step 4: Handshake loop
    void handshakeLoop();
    bool isAckMatch(const char* line) const;

    // Discard any buffered/stale RX bytes (boot noise, partial lines) so the
    // handshake starts from a clean line boundary.
    void flushInput();

    // Non-blocking serial line reader
    static constexpr uint8_t LINE_BUF_SIZE = 80;
    char m_lineBuf[LINE_BUF_SIZE];
    uint8_t m_linePos;
    bool readLine();
};

#endif // STARTUP_CONTROLLER_H
