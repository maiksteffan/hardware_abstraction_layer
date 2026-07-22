/**
 * @file EventQueue.h
 * @brief Thread-safe queue for outgoing serial events
 * 
 * Manages a queue of events to be sent to the Raspberry Pi.
 * Thread-safe for cross-core access using FreeRTOS mutexes.
 */

#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Config.h"

// ============================================================================
// Event Types
// ============================================================================

enum class EventType : uint8_t {
    ACK,            // Command acknowledged
    DONE,           // Long-running command completed
    ERR,            // Error occurred
    BUSY,           // Queue full, retry later
    TOUCHED,        // Touch detected
    TOUCH_RELEASED, // Touch released
    SCANNED,        // Sensor scan complete
    RECALIBRATED,   // Sensor recalibrated
    INFO,           // Firmware info
    VALUE,          // Sensor value response
    HANDS_ON,       // Hands-off detection: board went from untouched to touched
    HANDS_OFF,      // Hands-off detection: board went from touched to untouched
    DIAG            // Diagnostic line (telemetry) — Pi logs and ignores it
};

// ============================================================================
// Event Data Structure
// ============================================================================

struct Event {
    EventType type;
    char action[16];
    char position[POSITION_STRING_LENGTH];  // e.g. "H01\0"; empty string means "no position"
    uint32_t commandId;
    // Holds SCANNED list / ERR reason / VALUE delta. Must fit the full
    // 34-token active-sensor list "H01,H02,...,H34" = 135 chars + null.
    char extra[160];
    bool valid;
};

// ============================================================================
// EventQueue Class
// ============================================================================

class EventQueue {
public:
    EventQueue();
    ~EventQueue();
    
    void begin();
    void flush(uint8_t maxEvents = 5);
    void clear();   // Drop every pending event
    
    bool isFull() const;
    bool isEmpty() const;
    uint8_t count() const;
    
    // Queue event methods (thread-safe, callable from any core)
    bool queueAck(const char* action, const char* position = nullptr, uint32_t commandId = COMMAND_ID_NONE);
    bool queueDone(const char* action, const char* position = nullptr, uint32_t commandId = COMMAND_ID_NONE);
    bool queueError(const char* reason, uint32_t commandId = COMMAND_ID_NONE);
    bool queueBusy(uint32_t commandId = COMMAND_ID_NONE);
    bool queueTouched(const char* position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueTouchReleased(const char* position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueScanned(const char* sensorList, uint32_t commandId = COMMAND_ID_NONE);
    bool queueRecalibrated(const char* position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueInfo(uint32_t commandId = COMMAND_ID_NONE);
    bool queueValue(const char* position, int8_t value, uint32_t commandId = COMMAND_ID_NONE);
    bool queueHandsOn(uint32_t commandId = COMMAND_ID_NONE);
    bool queueHandsOff(uint32_t commandId = COMMAND_ID_NONE);
    // Queues "DIAG <text>" (never carries a #id). Used by touch telemetry.
    bool queueDiag(const char* text);

    // Health/introspection counters (diagnostic tolerance: reads are
    // unsynchronized snapshots)
    uint8_t freeSlots() const { return QUEUE_SIZE_EVENTS - m_count; }
    uint32_t pushFailureCount() const { return m_pushFailures; }
    uint8_t maxDepthSeen() const { return m_maxDepth; }

private:
    Event m_events[QUEUE_SIZE_EVENTS];
    uint8_t m_head;
    uint8_t m_tail;
    volatile uint8_t m_count;
    volatile uint32_t m_pushFailures;  // enqueue attempts rejected (queue full / mutex timeout)
    volatile uint8_t m_maxDepth;       // high-water mark of m_count
    
    SemaphoreHandle_t m_queueMutex;
    SemaphoreHandle_t m_serialMutex;
    
    bool enqueue(const Event& event);
    void sendEvent(const Event& event);
};

#endif // EVENT_QUEUE_H
