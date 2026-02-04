/**
 * @file EventQueue.h
 * @brief Outgoing event queue for serial communication
 * 
 * Provides a non-blocking queue for outgoing serial messages.
 */

#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <Arduino.h>
#include "Config.h"

// ============================================================================
// Event Types
// ============================================================================

enum class EventType : uint8_t {
    ACK,
    DONE,
    ERR,
    TOUCHED,
    TOUCH_RELEASED,
    SCANNED,
    RECALIBRATED,
    INFO,
    VALUE
};

// ============================================================================
// Event Structure
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
    
    void begin();
    void flush(uint8_t maxEvents = 3);
    
    bool isFull() const;
    bool isEmpty() const;
    uint8_t count() const;
    
    // Event emission methods
    bool queueAck(const char* action, char position = 0, uint32_t commandId = NO_COMMAND_ID);
    bool queueDone(const char* action, char position = 0, uint32_t commandId = NO_COMMAND_ID);
    bool queueError(const char* reason, uint32_t commandId = NO_COMMAND_ID);
    bool queueTouched(char position, uint32_t commandId = NO_COMMAND_ID);
    bool queueTouchReleased(char position, uint32_t commandId = NO_COMMAND_ID);
    bool queueScanned(const char* sensorList, uint32_t commandId = NO_COMMAND_ID);
    bool queueRecalibrated(char position, uint32_t commandId = NO_COMMAND_ID);
    bool queueInfo(uint32_t commandId = NO_COMMAND_ID);
    bool queueValue(char position, int8_t value, uint32_t commandId = NO_COMMAND_ID);

private:
    Event m_queue[EVENT_QUEUE_SIZE];
    uint8_t m_head;
    uint8_t m_tail;
    uint8_t m_count;
    
    bool enqueue(const Event& event);
    void sendEvent(const Event& event);
};

#endif // EVENT_QUEUE_H
