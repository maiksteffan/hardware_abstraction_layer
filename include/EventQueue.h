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
    VALUE           // Sensor value response
};

// ============================================================================
// Event Data Structure
// ============================================================================

struct Event {
    EventType type;
    char action[16];
    char position;
    uint32_t commandId;
    char extra[52];
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
    
    bool isFull() const;
    bool isEmpty() const;
    uint8_t count() const;
    
    // Queue event methods (thread-safe, callable from any core)
    bool queueAck(const char* action, char position = 0, uint32_t commandId = COMMAND_ID_NONE);
    bool queueDone(const char* action, char position = 0, uint32_t commandId = COMMAND_ID_NONE);
    bool queueError(const char* reason, uint32_t commandId = COMMAND_ID_NONE);
    bool queueBusy(uint32_t commandId = COMMAND_ID_NONE);
    bool queueTouched(char position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueTouchReleased(char position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueScanned(const char* sensorList, uint32_t commandId = COMMAND_ID_NONE);
    bool queueRecalibrated(char position, uint32_t commandId = COMMAND_ID_NONE);
    bool queueInfo(uint32_t commandId = COMMAND_ID_NONE);
    bool queueValue(char position, int8_t value, uint32_t commandId = COMMAND_ID_NONE);

private:
    Event m_events[QUEUE_SIZE_EVENTS];
    uint8_t m_head;
    uint8_t m_tail;
    volatile uint8_t m_count;
    
    SemaphoreHandle_t m_queueMutex;
    SemaphoreHandle_t m_serialMutex;
    
    bool enqueue(const Event& event);
    void sendEvent(const Event& event);
};

#endif // EVENT_QUEUE_H
