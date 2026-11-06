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
 *   HOLD_GAUGE <pos> <0-100> [#id] - Green block scaled to that share of its full
 *                                   width, as a hold-time gauge (100 = full SUCCESS
 *                                   block, 0 = center LED only). Absolute value
 *   MENUE_CHANGE <r,g,b> <range>  - Expand animation on both strips from 0 to range
 *   SEQUENCE_COMPLETED [#id]      - Play celebration animation
 *   DEFEAT_ANIMATION [#id]        - Play defeat animation (red full-board pulses)
 *   INDICATE_RECORDING [#id]      - Light all positions static dim white (clear with HIDE_ALL)
 *   SNAKE <start> <len> [r,g,b] [#id] - Paint track span [start, start+len) on top of
 *                                   the positions (default red 255,0,0); a new SNAKE
 *                                   moves it. Clamped to the track.
 *   SNAKE_OFF [#id]               - Remove the snake overlay (HIDE_ALL does too)
 * 
 * Touch Commands:
 *   EXPECT <pos> [#id]            - Wait for fresh unconsumed touch; evaluated each poll
 *   EXPECT_ANY [#id]              - First qualified, fresh, unconsumed grab (also across rearm gaps)
 *   EXPECT_ANY_EXCEPT [<pos>...] [#id] - Wait for any touch except the listed positions
 *   EXPECT_RELEASE <pos> [#id]    - Wait for debounced release; reports retry on queue pressure
 *   RECALIBRATE <pos> [#id]       - Recalibrate single sensor
 *   RECALIBRATE_ALL [#id]         - Recalibrate all sensors
 *   VALUE <pos> [#id]             - Get current sensor delta value
 *   TOUCH_STATE <pos> [#id]       - Read raw/debounced/consumed/expectation state without recalibration
 *   SET_SENSITIVITY <pos> <lvl>   - Set sensitivity (0=most, 7=least)
 *   CLEAN_QUEUE [#id]             - Clear all pending touch expectations
 *   HANDSOFF_DETECTION_ON [#id]   - Enable HANDS_ON/HANDS_OFF occupancy events
 *   HANDSOFF_DETECTION_OFF [#id]  - Disable occupancy events
 * 
 * Utility Commands:
 *   PING [#id]                    - Health check
 *   INFO [#id]                    - Get firmware info
 *   SCAN [#id]                    - Scan for connected sensors
 *   TRACK [#id]                   - Report the LED track: ACK, `TRACK length=<n> width=<w>`,
 *                                   one `TRACK_HOLD H01 <t>[,<t_mirror>]` per hold, DONE
 *                                   (see SnakeTrack.h)
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
    HOLD_GAUGE,
    MENUE_CHANGE,
    EXPECT,
    EXPECT_ANY,
    EXPECT_ANY_EXCEPT,
    EXPECT_RELEASE,
    RECALIBRATE,
    RECALIBRATE_ALL,
    VALUE,
    SET_SENSITIVITY,
    TOUCH_STATE,
    SCAN,
    SEQUENCE_COMPLETED,
    DEFEAT_ANIMATION,
    INDICATE_RECORDING,
    INFO,
    PING,
    CLEAN_QUEUE,
    HANDSOFF_DETECTION_ON,
    HANDSOFF_DETECTION_OFF,
    TRACK,
    SNAKE,
    SNAKE_OFF
};

// ============================================================================
// Parsed Command Structure
// ============================================================================

struct ParsedCommand {
    CommandAction action;
    bool hasPosition;
    char position[POSITION_STRING_LENGTH];  // canonical "H01".."H{holdCount}\0" (empty if !hasPosition)
    uint8_t positionIndex;                  // 0..holdCount-1 (255 = invalid)
    bool hasId;
    uint32_t id;
    uint8_t extraValue;  // For commands that need an extra numeric parameter (sensitivity level, HOLD_GAUGE percent)
    uint8_t r, g, b;     // RGB color for MENUE_CHANGE
    uint8_t range;       // Range for MENUE_CHANGE
    int32_t snakeStart;  // SNAKE: first track index (may be negative; clamped)
    uint16_t snakeLength; // SNAKE: number of LEDs
    HoldMask excludeMask; // EXPECT_ANY_EXCEPT: bit i set => input i is excluded
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
    char m_rxBuffer[SERIAL_LINE_MAX_LENGTH * 2];
    uint8_t m_rxHead;
    uint8_t m_rxTail;
    uint32_t m_lastRxTime;
    
    // Line buffer for parsing
    char m_lineBuffer[SERIAL_LINE_MAX_LENGTH];
    uint8_t m_lineIndex;
    bool m_lineOverflow;
    
    // Command queue
    QueuedCommand m_commandQueue[QUEUE_SIZE_COMMANDS];
    
    // Parsing methods
    bool extractLine();
    bool parseLine(const char* line, ParsedCommand& cmd);
    static CommandAction parseAction(const char* str, size_t len);
    static const char* actionToString(CommandAction action);
    static bool actionRequiresPosition(CommandAction action);
    static bool actionIsLongRunning(CommandAction action);
    
    bool parseSnakeParams(const char*& p, ParsedCommand& cmd);
    
    // Execution methods
    void executeCommand(const ParsedCommand& cmd);
    void executeInstant(const ParsedCommand& cmd);
    bool queueCommand(const ParsedCommand& cmd);
    void sendTrack(uint32_t cmdId);
    void tickCommand(QueuedCommand& qc);
    
    // Utilities
    static const char* skipWhitespace(const char* str);
    static const char* findTokenEnd(const char* str);
    static const char* parseUnsigned(const char* str, uint32_t maxValue, uint32_t& out);
    static bool strcasecmpN(const char* a, const char* b, size_t len);

public:
    // Position string helpers (canonical form: "H01".."H{holdCount}")
    // parsePosition() accepts 'H'/'h' + 2 digits, value 1..holdCount.
    // Returns input index (0..holdCount-1) on success, 255 on failure.
    // outStr (size POSITION_STRING_LENGTH) is written with the canonical upper-case form on success.
    static uint8_t parsePosition(const char* token, size_t tokenLen, char outStr[POSITION_STRING_LENGTH]);
    static void indexToPosition(uint8_t index, char outStr[POSITION_STRING_LENGTH]);
};

#endif // COMMAND_CONTROLLER_H
