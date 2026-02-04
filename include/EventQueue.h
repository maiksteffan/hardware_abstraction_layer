/**
 * @file EventQueue.h
 * @brief Thread-safe outgoing event queue for serial communication
 * 
 * Provides a non-blocking, thread-safe queue for outgoing serial messages.
 * Uses FreeRTOS mutex for cross-core synchronization (touch events from Core 0,
 * command responses from Core 1).
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
    ACK,
    DONE,
    ERR,
    BUSY,           // New: Flow control when command queue is full
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
// EventQueue Class (Thread-Safe)
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
    
    // Event emission methods (all thread-safe, can be called from any core)
    bool queueAck(const char* action, char position = 0, uint32_t commandId = NO_COMMAND_ID);
    bool queueDone(const char* action, char position = 0, uint32_t commandId = NO_COMMAND_ID);
    bool queueError(const char* reason, uint32_t commandId = NO_COMMAND_ID);
    bool queueBusy(uint32_t commandId = NO_COMMAND_ID);  // New: Flow control response
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
    volatile uint8_t m_count;  // volatile for cross-core visibility
    
    // FreeRTOS synchronization
    SemaphoreHandle_t m_queueMutex;   // Protects queue operations
    SemaphoreHandle_t m_serialMutex;  // Protects serial output
    
    bool enqueue(const Event& event);
    void sendEventOptimized(const Event& event);  // Single-buffer serial output
};

#endif // EVENT_QUEUE_H
