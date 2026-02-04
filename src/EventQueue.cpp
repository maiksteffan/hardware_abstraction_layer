/**
 * @file EventQueue.cpp
 * @brief Implementation of outgoing event queue for serial communication
 */

#include "EventQueue.h"

// ============================================================================
// Constructor
// ============================================================================

EventQueue::EventQueue()
    : m_head(0)
    , m_tail(0)
    , m_count(0)
{
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
}

void EventQueue::flush(uint8_t maxEvents) {
    uint8_t sent = 0;
    
    while (!isEmpty() && sent < maxEvents) {
        Event& event = m_queue[m_tail];
        
        if (event.valid) {
            sendEvent(event);
            event.valid = false;
        }
        
        m_tail = (m_tail + 1) % EVENT_QUEUE_SIZE;
        m_count--;
        sent++;
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
    if (isFull()) {
        return false;
    }
    
    m_queue[m_head] = event;
    m_head = (m_head + 1) % EVENT_QUEUE_SIZE;
    m_count++;
    
    return true;
}

void EventQueue::sendEvent(const Event& event) {
    switch (event.type) {
        case EventType::ACK:
            Serial.print("ACK ");
            Serial.print(event.action);
            if (event.position != 0) {
                Serial.print(" ");
                Serial.print(event.position);
            }
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::DONE:
            Serial.print("DONE ");
            Serial.print(event.action);
            if (event.position != 0) {
                Serial.print(" ");
                Serial.print(event.position);
            }
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::ERR:
            Serial.print("ERR ");
            Serial.print(event.extra);
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::TOUCHED:
            Serial.print("TOUCHED ");
            Serial.print(event.position);
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::TOUCH_RELEASED:
            Serial.print("TOUCH_RELEASED ");
            Serial.print(event.position);
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::SCANNED:
            Serial.print("SCANNED [");
            Serial.print(event.extra);
            Serial.print("]");
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::RECALIBRATED:
            Serial.print("RECALIBRATED ");
            if (event.position == 0) {
                Serial.print("ALL");
            } else {
                Serial.print(event.position);
            }
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::INFO:
            Serial.print("INFO firmware=");
            Serial.print(FIRMWARE_VERSION);
            Serial.print(" protocol=");
            Serial.print(PROTOCOL_VERSION);
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
            
        case EventType::VALUE:
            Serial.print("VALUE ");
            Serial.print(event.position);
            Serial.print(" ");
            Serial.print(event.extra);
            if (event.commandId != NO_COMMAND_ID) {
                Serial.print(" #");
                Serial.print(event.commandId);
            }
            Serial.println();
            break;
    }
}
