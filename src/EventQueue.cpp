/**
 * @file EventQueue.cpp
 * @brief Thread-safe event queue implementation
 * 
 * Uses FreeRTOS mutexes for cross-core synchronization:
 * - m_queueMutex: Protects queue add/remove operations
 * - m_serialMutex: Ensures atomic serial message output
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
// Initialization
// ============================================================================

void EventQueue::begin() {
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    
    for (uint8_t i = 0; i < QUEUE_SIZE_EVENTS; i++) {
        m_events[i].valid = false;
    }
    
    if (!m_queueMutex) {
        m_queueMutex = xSemaphoreCreateMutex();
    }
    if (!m_serialMutex) {
        m_serialMutex = xSemaphoreCreateMutex();
    }
}

// ============================================================================
// Queue Operations
// ============================================================================

void EventQueue::flush(uint8_t maxEvents) {
    uint8_t sentCount = 0;
    
    while (!isEmpty() && sentCount < maxEvents) {
        Event event;
        bool eventRetrieved = false;
        
        if (xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_FLUSH_MS)) == pdTRUE) {
            if (m_count > 0 && m_events[m_tail].valid) {
                event = m_events[m_tail];
                m_events[m_tail].valid = false;
                m_tail = (m_tail + 1) % QUEUE_SIZE_EVENTS;
                m_count--;
                eventRetrieved = true;
            }
            xSemaphoreGive(m_queueMutex);
        }
        
        if (eventRetrieved) {
            sendEvent(event);
            sentCount++;
        } else {
            break;
        }
    }
}

bool EventQueue::isFull() const {
    return m_count >= QUEUE_SIZE_EVENTS;
}

bool EventQueue::isEmpty() const {
    return m_count == 0;
}

uint8_t EventQueue::count() const {
    return m_count;
}

// ============================================================================
// Event Queueing Methods
// ============================================================================

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
    
    if (xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_QUEUE_MS)) == pdTRUE) {
        if (m_count < QUEUE_SIZE_EVENTS) {
            m_events[m_head] = event;
            m_head = (m_head + 1) % QUEUE_SIZE_EVENTS;
            m_count++;
            success = true;
        }
        xSemaphoreGive(m_queueMutex);
    }
    
    return success;
}

/**
 * @brief Formats and sends an event over serial
 * 
 * Uses a single buffer write for atomic transmission,
 * preventing message interleaving from concurrent cores.
 */
void EventQueue::sendEvent(const Event& event) {
    char buffer[EVENT_MESSAGE_BUFFER_SIZE];
    int length = 0;
    
    switch (event.type) {
        case EventType::ACK:
            length = snprintf(buffer, sizeof(buffer), "ACK %s", event.action);
            if (event.position != 0) {
                length += snprintf(buffer + length, sizeof(buffer) - length, " %c", event.position);
            }
            break;
            
        case EventType::DONE:
            length = snprintf(buffer, sizeof(buffer), "DONE %s", event.action);
            if (event.position != 0) {
                length += snprintf(buffer + length, sizeof(buffer) - length, " %c", event.position);
            }
            break;
            
        case EventType::ERR:
            length = snprintf(buffer, sizeof(buffer), "ERR %s", event.extra);
            break;
            
        case EventType::BUSY:
            length = snprintf(buffer, sizeof(buffer), "BUSY");
            break;
            
        case EventType::TOUCHED:
            length = snprintf(buffer, sizeof(buffer), "TOUCHED %c", event.position);
            break;
            
        case EventType::TOUCH_RELEASED:
            length = snprintf(buffer, sizeof(buffer), "TOUCH_RELEASED %c", event.position);
            break;
            
        case EventType::SCANNED:
            length = snprintf(buffer, sizeof(buffer), "SCANNED [%s]", event.extra);
            break;
            
        case EventType::RECALIBRATED:
            if (event.position == 0) {
                length = snprintf(buffer, sizeof(buffer), "RECALIBRATED ALL");
            } else {
                length = snprintf(buffer, sizeof(buffer), "RECALIBRATED %c", event.position);
            }
            break;
            
        case EventType::INFO:
            length = snprintf(buffer, sizeof(buffer), "INFO firmware=%s protocol=%s board=%s",
                             FIRMWARE_VERSION, PROTOCOL_VERSION, BOARD_TYPE);
            break;
            
        case EventType::VALUE:
            length = snprintf(buffer, sizeof(buffer), "VALUE %c %s", event.position, event.extra);
            break;
    }
    
    // Append command ID if present
    if (event.commandId != COMMAND_ID_NONE) {
        length += snprintf(buffer + length, sizeof(buffer) - length, " #%lu", event.commandId);
    }
    
    // Append newline
    if (length < (int)sizeof(buffer) - 1) {
        buffer[length++] = '\n';
    }
    
    // Thread-safe serial write
    if (xSemaphoreTake(m_serialMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SERIAL_MS)) == pdTRUE) {
        Serial.write(buffer, length);
        xSemaphoreGive(m_serialMutex);
    }
}
