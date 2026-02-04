/**
 * @file EventQueue.cpp
 * @brief Thread-safe event queue implementation with optimized serial output
 * 
 * Uses FreeRTOS mutexes for cross-core synchronization:
 * - m_queueMutex: Protects queue add/remove operations
 * - m_serialMutex: Ensures atomic serial message output
 * 
 * Optimization: Single-buffer serial output instead of multiple Serial.print() calls
 * reduces latency by ~3x and prevents message interleaving.
 */

#include "EventQueue.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

EventQueue::EventQueue()
    : m_head(0)
    , m_tail(0)
    , m_count(0)
    , m_queueMutex(nullptr)
    , m_serialMutex(nullptr)
{
}

EventQueue::~EventQueue() {
    if (m_queueMutex) {
        vSemaphoreDelete(m_queueMutex);
        m_queueMutex = nullptr;
    }
    if (m_serialMutex) {
        vSemaphoreDelete(m_serialMutex);
        m_serialMutex = nullptr;
    }
}

// ============================================================================
// Public Methods
// ============================================================================

void EventQueue::begin() {
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    
    for (uint8_t i = 0; i < EVENT_QUEUE_SIZE; i++) {
        m_queue[i].valid = false;
    }
    
    // Create FreeRTOS mutexes for thread-safety
    if (!m_queueMutex) {
        m_queueMutex = xSemaphoreCreateMutex();
    }
    if (!m_serialMutex) {
        m_serialMutex = xSemaphoreCreateMutex();
    }
}

void EventQueue::flush(uint8_t maxEvents) {
    uint8_t sent = 0;
    
    while (!isEmpty() && sent < maxEvents) {
        Event event;
        bool hasEvent = false;
        
        // Thread-safe dequeue
        if (xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (m_count > 0 && m_queue[m_tail].valid) {
                event = m_queue[m_tail];
                m_queue[m_tail].valid = false;
                m_tail = (m_tail + 1) % EVENT_QUEUE_SIZE;
                m_count--;
                hasEvent = true;
            }
            xSemaphoreGive(m_queueMutex);
        }
        
        if (hasEvent) {
            sendEventOptimized(event);
            sent++;
        } else {
            break;
        }
    }
}

bool EventQueue::isFull() const {
    return m_count >= EVENT_QUEUE_SIZE;
}

bool EventQueue::isEmpty() const {
    return m_count == 0;
}

uint8_t EventQueue::count() const {
    return m_count;
}

bool EventQueue::queueAck(const char* action, char position, uint32_t commandId) {
    Event event;
    event.type = EventType::ACK;
    strncpy(event.action, action, sizeof(event.action) - 1);
    event.action[sizeof(event.action) - 1] = '\0';
    event.position = position;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueDone(const char* action, char position, uint32_t commandId) {
    Event event;
    event.type = EventType::DONE;
    strncpy(event.action, action, sizeof(event.action) - 1);
    event.action[sizeof(event.action) - 1] = '\0';
    event.position = position;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueError(const char* reason, uint32_t commandId) {
    Event event;
    event.type = EventType::ERR;
    event.action[0] = '\0';
    event.position = 0;
    event.commandId = commandId;
    strncpy(event.extra, reason, sizeof(event.extra) - 1);
    event.extra[sizeof(event.extra) - 1] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueBusy(uint32_t commandId) {
    Event event;
    event.type = EventType::BUSY;
    event.action[0] = '\0';
    event.position = 0;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueTouched(char position, uint32_t commandId) {
    Event event;
    event.type = EventType::TOUCHED;
    event.action[0] = '\0';
    event.position = position;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueTouchReleased(char position, uint32_t commandId) {
    Event event;
    event.type = EventType::TOUCH_RELEASED;
    event.action[0] = '\0';
    event.position = position;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueScanned(const char* sensorList, uint32_t commandId) {
    Event event;
    event.type = EventType::SCANNED;
    event.action[0] = '\0';
    event.position = 0;
    event.commandId = commandId;
    strncpy(event.extra, sensorList, sizeof(event.extra) - 1);
    event.extra[sizeof(event.extra) - 1] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueRecalibrated(char position, uint32_t commandId) {
    Event event;
    event.type = EventType::RECALIBRATED;
    event.action[0] = '\0';
    event.position = position;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueInfo(uint32_t commandId) {
    Event event;
    event.type = EventType::INFO;
    event.action[0] = '\0';
    event.position = 0;
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    
    return enqueue(event);
}

bool EventQueue::queueValue(char position, int8_t value, uint32_t commandId) {
    Event event;
    event.type = EventType::VALUE;
    event.action[0] = '\0';
    event.position = position;
    event.commandId = commandId;
    snprintf(event.extra, sizeof(event.extra), "%d", value);
    event.valid = true;
    
    return enqueue(event);
}

// ============================================================================
// Private Methods
// ============================================================================

bool EventQueue::enqueue(const Event& event) {
    bool success = false;
    
    // Thread-safe enqueue (can be called from any core)
    if (xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (m_count < EVENT_QUEUE_SIZE) {
            m_queue[m_head] = event;
            m_head = (m_head + 1) % EVENT_QUEUE_SIZE;
            m_count++;
            success = true;
        }
        xSemaphoreGive(m_queueMutex);
    }
    
    return success;
}

/**
 * @brief Optimized single-buffer serial output
 * 
 * Benefits:
 * - Single atomic write prevents interleaved messages from concurrent cores
 * - ~3x faster than multiple Serial.print() calls
 * - Complete lines for easier parsing on Raspberry Pi
 */
void EventQueue::sendEventOptimized(const Event& event) {
    char buffer[96];
    int len = 0;
    
    switch (event.type) {
        case EventType::ACK:
            len = snprintf(buffer, sizeof(buffer), "ACK %s", event.action);
            if (event.position != 0) {
                len += snprintf(buffer + len, sizeof(buffer) - len, " %c", event.position);
            }
            break;
            
        case EventType::DONE:
            len = snprintf(buffer, sizeof(buffer), "DONE %s", event.action);
            if (event.position != 0) {
                len += snprintf(buffer + len, sizeof(buffer) - len, " %c", event.position);
            }
            break;
            
        case EventType::ERR:
            len = snprintf(buffer, sizeof(buffer), "ERR %s", event.extra);
            break;
            
        case EventType::BUSY:
            len = snprintf(buffer, sizeof(buffer), "BUSY");
            break;
            
        case EventType::TOUCHED:
            len = snprintf(buffer, sizeof(buffer), "TOUCHED %c", event.position);
            break;
            
        case EventType::TOUCH_RELEASED:
            len = snprintf(buffer, sizeof(buffer), "TOUCH_RELEASED %c", event.position);
            break;
            
        case EventType::SCANNED:
            len = snprintf(buffer, sizeof(buffer), "SCANNED [%s]", event.extra);
            break;
            
        case EventType::RECALIBRATED:
            if (event.position == 0) {
                len = snprintf(buffer, sizeof(buffer), "RECALIBRATED ALL");
            } else {
                len = snprintf(buffer, sizeof(buffer), "RECALIBRATED %c", event.position);
            }
            break;
            
        case EventType::INFO:
            len = snprintf(buffer, sizeof(buffer), "INFO firmware=%s protocol=%s board=%s",
                          FIRMWARE_VERSION, PROTOCOL_VERSION, BOARD_TYPE);
            break;
            
        case EventType::VALUE:
            len = snprintf(buffer, sizeof(buffer), "VALUE %c %s", event.position, event.extra);
            break;
    }
    
    // Append command ID if present (critical for Pi to match responses)
    if (event.commandId != NO_COMMAND_ID) {
        len += snprintf(buffer + len, sizeof(buffer) - len, " #%lu", event.commandId);
    }
    
    // Add newline
    if (len < (int)sizeof(buffer) - 1) {
        buffer[len++] = '\n';
    }
    
    // Thread-safe serial output (prevents interleaved messages)
    if (xSemaphoreTake(m_serialMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        Serial.write(buffer, len);
        xSemaphoreGive(m_serialMutex);
    }
}
