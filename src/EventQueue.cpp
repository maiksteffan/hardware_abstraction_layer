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

        // Use a longer timeout than MUTEX_TIMEOUT_FLUSH_MS here: if the touch
        // task on Core 0 is mid-enqueue, silently bailing out leaves all
        // pending replies (ACK/DONE/SCANNED) stuck and the Pi times out.
        if (xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_QUEUE_MS)) == pdTRUE) {
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

void EventQueue::clear() {
    if (m_queueMutex &&
        xSemaphoreTake(m_queueMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_QUEUE_MS)) == pdTRUE) {
        for (uint8_t i = 0; i < QUEUE_SIZE_EVENTS; i++) {
            m_events[i].valid = false;
        }
        m_head = 0;
        m_tail = 0;
        m_count = 0;
        xSemaphoreGive(m_queueMutex);
    }
}

// ============================================================================
// Internal helpers
// ============================================================================

static inline void copyPosition(char dst[POSITION_STRING_LENGTH], const char* src) {
    if (src && src[0]) {
        strncpy(dst, src, POSITION_STRING_LENGTH - 1);
        dst[POSITION_STRING_LENGTH - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

// ============================================================================
// Event Queueing Methods
// ============================================================================

bool EventQueue::queueAck(const char* action, const char* position, uint32_t commandId) {
    Event event;
    event.type = EventType::ACK;
    strncpy(event.action, action, sizeof(event.action) - 1);
    event.action[sizeof(event.action) - 1] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueDone(const char* action, const char* position, uint32_t commandId) {
    Event event;
    event.type = EventType::DONE;
    strncpy(event.action, action, sizeof(event.action) - 1);
    event.action[sizeof(event.action) - 1] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueError(const char* reason, uint32_t commandId) {
    Event event;
    event.type = EventType::ERR;
    event.action[0] = '\0';
    event.position[0] = '\0';
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
    event.position[0] = '\0';
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueTouched(const char* position, uint32_t commandId) {
    Event event;
    event.type = EventType::TOUCHED;
    event.action[0] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueTouchReleased(const char* position, uint32_t commandId) {
    Event event;
    event.type = EventType::TOUCH_RELEASED;
    event.action[0] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueScanned(const char* sensorList, uint32_t commandId) {
    Event event;
    event.type = EventType::SCANNED;
    event.action[0] = '\0';
    event.position[0] = '\0';
    event.commandId = commandId;
    strncpy(event.extra, sensorList, sizeof(event.extra) - 1);
    event.extra[sizeof(event.extra) - 1] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueRecalibrated(const char* position, uint32_t commandId) {
    Event event;
    event.type = EventType::RECALIBRATED;
    event.action[0] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueInfo(uint32_t commandId) {
    Event event;
    event.type = EventType::INFO;
    event.action[0] = '\0';
    event.position[0] = '\0';
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueValue(const char* position, int8_t value, uint32_t commandId) {
    Event event;
    event.type = EventType::VALUE;
    event.action[0] = '\0';
    copyPosition(event.position, position);
    event.commandId = commandId;
    snprintf(event.extra, sizeof(event.extra), "%d", value);
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueHandsOn(uint32_t commandId) {
    Event event;
    event.type = EventType::HANDS_ON;
    event.action[0] = '\0';
    event.position[0] = '\0';
    event.commandId = commandId;
    event.extra[0] = '\0';
    event.valid = true;
    return enqueue(event);
}

bool EventQueue::queueHandsOff(uint32_t commandId) {
    Event event;
    event.type = EventType::HANDS_OFF;
    event.action[0] = '\0';
    event.position[0] = '\0';
    event.commandId = commandId;
    event.extra[0] = '\0';
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
            if (event.position[0] != '\0') {
                length += snprintf(buffer + length, sizeof(buffer) - length, " %s", event.position);
            }
            break;
            
        case EventType::DONE:
            length = snprintf(buffer, sizeof(buffer), "DONE %s", event.action);
            if (event.position[0] != '\0') {
                length += snprintf(buffer + length, sizeof(buffer) - length, " %s", event.position);
            }
            break;
            
        case EventType::ERR:
            length = snprintf(buffer, sizeof(buffer), "ERR %s", event.extra);
            break;
            
        case EventType::BUSY:
            length = snprintf(buffer, sizeof(buffer), "BUSY");
            break;
            
        case EventType::TOUCHED:
            length = snprintf(buffer, sizeof(buffer), "TOUCHED %s", event.position);
            break;
            
        case EventType::TOUCH_RELEASED:
            length = snprintf(buffer, sizeof(buffer), "TOUCH_RELEASED %s", event.position);
            break;
            
        case EventType::SCANNED:
            length = snprintf(buffer, sizeof(buffer), "SCANNED [%s]", event.extra);
            break;
            
        case EventType::RECALIBRATED:
            if (event.position[0] == '\0') {
                length = snprintf(buffer, sizeof(buffer), "RECALIBRATED ALL");
            } else {
                length = snprintf(buffer, sizeof(buffer), "RECALIBRATED %s", event.position);
            }
            break;
            
        case EventType::INFO:
            // Same builder as the startup banner, so the two never drift apart.
            // Includes the active profile: by the time the Pi can send INFO as
            // a command, negotiation is long finished.
            length = (int)buildBoardInfoLine(buffer, sizeof(buffer), true);
            break;
            
        case EventType::VALUE:
            length = snprintf(buffer, sizeof(buffer), "VALUE %s %s", event.position, event.extra);
            break;
            
        case EventType::HANDS_ON:
            length = snprintf(buffer, sizeof(buffer), "HANDS_ON");
            break;
            
        case EventType::HANDS_OFF:
            length = snprintf(buffer, sizeof(buffer), "HANDS_OFF");
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
    // Block indefinitely on the serial mutex: dropping an outbound message
    // silently (e.g. SCANNED, ACK, DONE) is far worse than briefly blocking
    // the caller. The mutex is only held for the duration of one Serial.write
    // + Serial.flush (bounded by UART speed: ~15 ms for a 160-byte line at
    // 115200 baud), so portMAX_DELAY here cannot deadlock the system.
    if (xSemaphoreTake(m_serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.write(buffer, length);
        // Block until every byte has been fully transmitted on the wire before
        // releasing the mutex. Without this, Serial.write() only queues bytes
        // into the TX ring buffer and returns; a subsequent event's bytes can
        // then be appended while the previous bytes are still draining — and
        // any UART hiccup mid-drain (interrupt disable from NeoPixel show(),
        // watchdog yield, etc.) can split a single message across multiple
        // physical "chunks" on the wire from the Pi's POV.
        Serial.flush();
        xSemaphoreGive(m_serialMutex);
    }
}
