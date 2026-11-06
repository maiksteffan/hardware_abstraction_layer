/**
 * @file TouchController.h
 * @brief Touch input controller for CAP1188 capacitive sensors over I2C
 *
 * Hardware: the active board profile's CAP1188 chips, each exposing up to
 * TOUCH_CHANNELS_PER_SENSOR channels. The firmware exposes the profile's
 * logical inputs H01..H{holdCount} via its inputMappings[] table.
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
#include <freertos/FreeRTOS.h>
#include "Config.h"

class EventQueue;

// ============================================================================
// Types
// ============================================================================

// State for one physical CAP1188 chip
struct PhysicalSensor {
    bool active;          // I2C init succeeded
    uint8_t enableMask;   // OR of (1 << channel) for every input mapped to this sensor
    uint8_t lastStatus;   // Last-read SENSOR_INPUT_STATUS register (cached per poll cycle)
    bool statusValid;     // Whether lastStatus holds a fresh value
};

// State for one logical input (H01..H{holdCount})
struct TouchInputState {
    bool currentTouched;
    bool debouncedTouched;
    bool lastReportedTouched;
    uint32_t lastChangeTime;
    uint32_t lastSampleTime;      // Includes failed polls; long gaps are never credited
    uint16_t releaseEvidenceMs;   // Untouched evidence; failed reads pause, never restart it
    uint16_t gripBridgeMs;        // Untouched time bridged by a still-high delta (bounded per run)
    uint32_t pressStartTime;   // millis() of the last debounced press edge
    bool pressConsumed;        // Current press already satisfied an EXPECT/EXPECT_ANY;
                               // requires release + re-grab before it can satisfy another
};

struct ExpectState {
    bool active;
    uint32_t commandId;
    HoldMask excludeMask;  // EXPECT_ANY_EXCEPT only: bit i set => input i is excluded
};

// Published by the command core, claimed/retried by the touch core. Revision
// prevents completion of an old event from erasing a concurrently rearmed one.
struct PointExpectation {
    bool active;
    bool pending;
    uint32_t commandId;
    uint32_t revision;
};

// Qualification state for one EXPECT_ANY touch candidate (brush-by filter).
// Every press opens a candidate, even between expectations. Unconsumed grabs
// can restart qualification after a brief raw dropout during the release latch.
// Persistence + delta consistency + sweep plausibility must pass before an
// eligible EXPECT_ANY receives it; pending candidates expire after 2 seconds.
// See Config.h section 6b.
struct AnyCandidate {
    bool active;
    uint32_t startTime;         // Window start (millis)
    uint16_t deltaSamples;      // Successful delta-register reads this window
    uint16_t goodDeltaSamples;  // Samples with delta >= EXPECT_ANY_DELTA_MIN
    uint8_t untouchedStreak;    // Consecutive raw-untouched poll samples
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
    
    // Recalibration (inputIndex = 0..holdCount-1)
    bool recalibrate(uint8_t inputIndex);
    void recalibrateAll();
    
    // Sensitivity control. NOTE: CAP1188 sensitivity is GLOBAL per chip, so
    // setting it on any input affects every other input mapped to the same
    // physical sensor.
    bool setSensitivity(uint8_t inputIndex, uint8_t level);
    
    // Expectations (indexed by input 0..holdCount-1). Point setters publish a
    // request; polling evaluates it within one tick, including already-held /
    // released inputs. Confirmed events persist across outgoing queue pressure.
    void setExpectDown(uint8_t inputIndex, uint32_t commandId);
    void setExpectUp(uint8_t inputIndex, uint32_t commandId);
    void setExpectAny(uint32_t commandId);
    void setExpectAnyExcept(const HoldMask& excludeMask, uint32_t commandId);
    void clearExpectDown(uint8_t inputIndex);
    void clearExpectUp(uint8_t inputIndex);
    void clearExpectAny();
    void clearAllExpectations();  // Clear every pending EXPECT/EXPECT_RELEASE/EXPECT_ANY
    
    // Hands-off detection. While enabled, emits HANDS_OFF when the number of
    // touched inputs drops to 0 and HANDS_ON when it rises above 0 again.
    // Enabling immediately reports the current state once (with commandId).
    void setHandsOffDetection(bool enabled, uint32_t commandId = COMMAND_ID_NONE);
    
    // State queries
    bool isInputActive(uint8_t inputIndex) const;     // input's parent sensor is active
    bool isTouched(uint8_t inputIndex) const;
    uint8_t getActiveSensorCount() const;             // number of physical chips initialized
    void buildActiveSensorList(char* buffer, size_t bufferSize) const;
    void buildFailedInputList(char* buffer, size_t bufferSize) const;  // H## inputs whose parent sensor failed init

    // ---- Diagnostics (used by StartupController to emit DIAG lines) -------
    // Scan the full 7-bit address range and write found addresses (hex) into
    // buffer as e.g. "0x28,0x29,0x2A". Returns the number of devices found.
    // Wire must already be initialized.
    uint8_t scanI2CBus(char* buffer, size_t bufferSize) const;

    // Per-sensor init diagnostic. Performs the same init sequence as
    // initSensor() but writes a human-readable result to outDiag.
    // Examples:
    //   "S0@0x28 OK pid=0x50"
    //   "S0@0x28 FAIL probe"
    //   "S0@0x28 FAIL pid_read"
    //   "S0@0x28 FAIL pid=0x42"
    //   "S0@0x28 FAIL write_mtblk"
    // Returns true iff sensor is now considered active.
    bool diagInitSensor(uint8_t sensorIndex, char* outDiag, size_t diagSize);

    // Sensor value reading (delta count for the input's CAP1188 channel)
    bool readSensorValue(uint8_t inputIndex, int8_t& value);
    // On-demand snapshot built on the polling core; no additional I2C traffic.
    void requestTouchState(uint8_t inputIndex, uint32_t commandId);

private:
    EventQueue* m_eventQueue;
    // Sized to the ceilings, not the active profile: the profile is only known
    // at runtime. Loops and bounds checks use activeBoardProfile().
    PhysicalSensor m_sensors[MAX_SENSORS];
    TouchInputState m_inputs[MAX_HOLDS];
    portMUX_TYPE m_expectationMux = portMUX_INITIALIZER_UNLOCKED;
    PointExpectation m_expectDown[MAX_HOLDS];
    PointExpectation m_expectUp[MAX_HOLDS];
    PointExpectation m_pendingAnyTouch;
    uint8_t m_pendingAnyInput;
    PointExpectation m_touchStateRequest;
    uint8_t m_touchStateInput;
    ExpectState m_expectAnyQueue[EXPECT_ANY_QUEUE_SIZE];
    uint8_t m_expectAnyHead;   // Next write index
    uint8_t m_expectAnyTail;   // Next read index
    AnyCandidate m_anyCandidates[MAX_HOLDS];          // EXPECT_ANY qualification state
    uint32_t m_pressEdgeRing[EXPECT_ANY_EDGE_RING_SIZE];  // Recent press-edge timestamps
    uint8_t m_pressEdgeRingPos;
    uint32_t m_sweepHoldoffUntil;      // EXPECT_ANY decisions deferred until this time
    uint32_t m_lastPollTime;
    uint8_t m_activeSensorCount;
    bool m_handsOffDetectionEnabled;    // HANDSOFF_DETECTION_ON/OFF
    bool m_lastAnyTouched;              // Last reported board-occupancy state
    
    bool initSensor(uint8_t sensorIndex);
    bool readRegister(uint8_t address, uint8_t reg, uint8_t& value);
    bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
    void pollSensors();
    void processDebounce();
    void processPointExpectations();  // Claim states and retry rejected events every tick
    void processPendingAnyTouch();    // Retry an already-qualified EXPECT_ANY event
    void processTouchStateRequest();  // Nonblocking, on-demand diagnostic reply
    void processExpectAnyQualification();  // Advance/finish EXPECT_ANY candidates
    void startAnyCandidate(uint8_t inputIndex, uint32_t now);
    void recordPressEdge(uint32_t now);    // Sweep detection bookkeeping
    bool fireExpectAny(uint8_t inputIndex);  // Claim queue head for reliable TOUCHED delivery
    void processHandsOffDetection();
    bool anyInputTouched() const;
};

#endif // TOUCH_CONTROLLER