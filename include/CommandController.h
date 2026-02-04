/**
 * @file CommandController.h
 * @brief Serial command parser and executor
 * 
 * Handles all serial commands from the Raspberry Pi:
 * 
 * LED Commands:
 *   SHOW <pos> [#id]              - Turn on LED (blue)
 *   HIDE <pos> [#id]              - Turn off LED
 *   SUCCESS <pos> [#id]           - Play green expansion animation
 *   FAIL <pos> [#id]              - Show red LED (error indicator)
 *   CONTRACT <pos> [#id]          - Contract expanded LED back to single
 *   BLINK <pos> [#id]             - Start blinking (orange, fast)
 *   STOP_BLINK <pos> [#id]        - Stop blinking
 *   EXPAND_STEP <pos> [#id]       - Expand lit area by 1 LED on each side
 *   CONTRACT_STEP <pos> [#id]     - Contract lit area by 1 LED on each side
 *   MENUE_CHANGE <r,g,b> <range>  - Expand animation on both strips from 0 to range
 *   SEQUENCE_COMPLETED [#id]      - Play celebration animation
 * 
 * Touch Commands:
 *   EXPECT <pos> [#id]            - Wait for touch
 *   EXPECT_RELEASE <pos> [#id]    - Wait for release
 *   RECALIBRATE <pos> [#id]       - Recalibrate single sensor
 *   RECALIBRATE_ALL [#id]         - Recalibrate all sensors
 *   VALUE <pos> [#id]             - Get current sensor delta value
 *   SET_SENSITIVITY <pos> <lvl>   - Set sensitivity (0=most, 7=least)
 * 
 * Utility Commands:
 *   PING [#id]                    - Health check
 *   INFO [#id]                    - Get firmware info
 *   SCAN [#id]                    - Scan for connected sensors
 */

#ifndef COMMAND_CONTROLLER_H
#define COMMAND_CONTROLLER_H

#include <Arduino.h>
#include "Config.h"

class LedController;
class TouchController;
class EventQueue;

// ============================================================================
// Command Types
// ============================================================================

enum class CommandAction : uint8_t {
    INVALID = 0,
    SHOW,
    HIDE,
    HIDE_ALL,
    SUCCESS,
    FAIL,
    CONTRACT,
    BLINK,
    STOP_BLINK,
    EXPAND_STEP,
    CONTRACT_STEP,
    MENUE_CHANGE,
    EXPECT,
    EXPECT_RELEASE,
    RECALIBRATE,
    RECALIBRATE_ALL,
    VALUE,
    SET_SENSITIVITY,
    SCAN,
    SEQUENCE_COMPLETED,
    INFO,
    PING
};

// ============================================================================
// Parsed Command Structure
// ============================================================================

struct ParsedCommand {
    CommandAction action;
    bool hasPosition;
    char position;
    uint8_t positionIndex;
    bool hasId;
    uint32_t id;
    uint8_t extraValue;  // For commands that need an extra numeric parameter (e.g., sensitivity level)
    uint8_t r, g, b;     // RGB color for MENUE_CHANGE
    uint8_t range;       // Range for MENUE_CHANGE
    bool valid;
};

// ============================================================================
// Queued Command (for long-running commands)
// ============================================================================

struct QueuedCommand {
    ParsedCommand command;
    bool active;
    uint32_t startTime;
    uint8_t state;
};

// ============================================================================
// CommandController Class
// ============================================================================

class CommandController {
public:
    CommandController(LedController& ledController, 
                      TouchController* touchController,
                      EventQueue& eventQueue);
    
    void begin();
    void pollSerial();
    void processCompletedLines();
    void tick();
    bool isQueueFull() const;

private:
    LedController& m_ledController;
    TouchController* m_touchController;
    EventQueue& m_eventQueue;
    
    // Ring buffer for incoming serial data
    char m_rxBuffer[MAX_LINE_LEN * 2];
    uint8_t m_rxHead;
    uint8_t m_rxTail;
    uint32_t m_lastRxTime;
    
    // Line buffer for parsing
    char m_lineBuffer[MAX_LINE_LEN];
    uint8_t m_lineIndex;
    bool m_lineOverflow;
    
    // Command queue
    QueuedCommand m_commandQueue[COMMAND_QUEUE_SIZE];
    
    // Parsing methods
    bool extractLine();
    bool parseLine(const char* line, ParsedCommand& cmd);
    static CommandAction parseAction(const char* str, size_t len);
    static const char* actionToString(CommandAction action);
    static bool actionRequiresPosition(CommandAction action);
    static bool actionIsLongRunning(CommandAction action);
    
    // Execution methods
    void executeCommand(const ParsedCommand& cmd);
    void executeInstant(const ParsedCommand& cmd);
    bool queueCommand(const ParsedCommand& cmd);
    void tickCommand(QueuedCommand& qc);
    
    // Utilities
    static const char* skipWhitespace(const char* str);
    static const char* findTokenEnd(const char* str);
    static bool strcasecmpN(const char* a, const char* b, size_t len);
    static uint8_t charToIndex(char c);
};

#endif // COMMAND_CONTROLLER_H
