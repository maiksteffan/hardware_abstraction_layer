/**
 * @file LedController.h
 * @brief LED Controller for dual addressable LED strips
 * 
 * Manages the active board profile's logical LED positions (H01..H{holdCount})
 * mapped to two physical LED strips.
 * Supports SHOW, HIDE, SUCCESS, BLINK, STOP_BLINK, and SEQUENCE_COMPLETED,
 * plus the SNAKE overlay: a free span of the track (see SnakeTrack.h) painted
 * on top of whatever the positions show.
 */

#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "Config.h"
#include "LedOffset.h"

// ============================================================================
// Types
// ============================================================================

// StripId, LedMapping and LedMirror come from BoardProfile.h (via Config.h):
// they describe wiring, which is per board version.

enum class PositionState : uint8_t {
    OFF,
    SHOWN,
    ANIMATING,
    EXPANDED,
    BLINKING,
    GAUGE,
    BAR
};

struct PositionData {
    PositionState state;
    uint8_t animationStep;
    uint32_t lastAnimationTime;
    bool blinkOn;
    // GAUGE only: how far out the block still reaches, in physical LEDs from
    // the center (0 = just the center LED). See holdGauge() / showGauge().
    uint8_t gaugeLevel;
    // BAR only: the time left, 0-1000 per mille (BarGauge.h).
    // The bar is drawn in the core color, like a gauge.
    uint16_t barPermille;
    // Core color of a SHOWN position. SHOWN covers more than SHOW: FAIL leaves
    // a red core, and repaintPosition() has to restore the right one after
    // the snake passed over it.
    // For GAUGE it is the color of the whole block - green for HOLD_GAUGE,
    // the SHOW color for SHOW_GAUGE - and paintGauge() draws in it.
    uint8_t coreR, coreG, coreB;
};

// ============================================================================
// LedController Class
// ============================================================================

class LedController {
public:
    LedController();
    
    void begin();
    void tick();
    
    // LED commands
    bool show(uint8_t position);
    bool hide(uint8_t position);
    void hideAll();
    bool success(uint8_t position);
    bool fail(uint8_t position);
    bool blink(uint8_t position);
    bool stopBlink(uint8_t position);
    // Green block scaled to <percent> of its full width, as a progress gauge:
    // the Pi drives it down while a hold must stay gripped. Absolute, so a
    // lost command is corrected by the next one. 100 = the full SUCCESS block,
    // 0 = the center LED alone (the hold never goes dark while it is active).
    bool holdGauge(uint8_t position, uint8_t percent);
    // The same gauge in the SHOW color: the next hold as a fuse that burns
    // down while the Pi's time to grab it runs out. Green stays "done".
    bool showGauge(uint8_t position, uint8_t percent);
    // The next hold as a blue bar that shrinks while its time runs out: the
    // SHOW_GAUGE block, but per mille and without steps - the LED at the end
    // of the bar is dimmed by how much of it is still covered (BarGauge.h).
    // <permille> is the time left, absolute like the gauges; 0 is the center
    // LED alone. A static state: the firmware keeps no timer.
    bool showBar(uint8_t position, uint16_t permille);
    // The same bar in green: the HOLD_GAUGE block without steps, how much
    // longer a hold must stay gripped.
    bool holdBar(uint8_t position, uint16_t permille);
    
    // Sequence animation
    void startSequenceCompletedAnimation();
    bool isSequenceCompletedAnimationComplete() const;
    
    // Defeat animation (red full-board pulses)
    void startDefeatAnimation();
    bool isDefeatAnimationComplete() const;
    
    // Recording indicator (static dim white on all positions; clear with hideAll())
    void indicateRecording();
    
    // Snake overlay: paint track span [start, start+length) on top of the
    // positions (clamped to the track; start may be negative). One overlay at
    // a time; a new call moves it. hideAll() removes it as well.
    void snake(int32_t start, uint16_t length, uint8_t r, uint8_t g, uint8_t b);
    void snakeOff();

    // Per-board correction of where a position's block sits, in LEDs along
    // the track (see LedOffset.h). Held in RAM only: begin() resets every
    // position to its profile index and the Pi sends its offsets again after
    // each connect. A lit position moves at once. False, and nothing changes,
    // when the offset is out of range or would push the block's center off
    // its strip.
    bool setLedOffset(uint8_t position, int8_t offset);
    int8_t ledOffset(uint8_t position) const;

    // State queries
    bool isAnimationComplete(uint8_t position) const;
    bool isBlinking(uint8_t position) const;
    
    // Low-level access (used by StartupController animation)
    uint16_t getLedCount(StripId strip) const;
    void setPixelColor(StripId strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b);
    void showStrip();
    void clearAll();

private:
    Adafruit_NeoPixel m_strip1;
    Adafruit_NeoPixel m_strip2;
    // Sized to the ceiling, not the active profile: the profile is only known
    // at runtime. Loops and bounds checks use activeBoardProfile().holdCount.
    PositionData m_positions[MAX_HOLDS];
    
    bool m_sequenceAnimActive;
    uint8_t m_sequenceAnimStep;
    uint32_t m_sequenceAnimLastTime;
    bool m_needsUpdate;
    
    // Defeat animation state
    bool m_defeatAnimActive;
    uint16_t m_defeatAnimStep;
    uint32_t m_defeatAnimLastTime;
    
    // Snake overlay state, as a half-open track span [start, end)
    bool m_snakeActive;
    uint16_t m_snakeStart;
    uint16_t m_snakeEnd;
    uint8_t m_snakeR, m_snakeG, m_snakeB;
    
    // LED_OFFSET corrections, applied in ledIndex()
    LedOffsets m_ledOffsets;

    void update(uint32_t nowMillis);
    const LedMapping* getMapping(uint8_t position) const;
    const LedMirror* getMirror(uint8_t position) const;
    // Where a mapped LED of this position really is: its profile index moved
    // by the position's offset. Every lookup of a position's LED goes through
    // here, so the offset cannot be forgotten in one of the paint paths.
    int16_t ledIndex(uint8_t position, StripId strip, uint8_t index) const;
    // Track spans the position's block covers right now (core plus whatever
    // rings its state can reach), inclusive: primary first, then the mirror.
    // Returns how many were written (0..2).
    uint8_t positionTrackSpans(uint8_t position, uint16_t lo[2], uint16_t hi[2]) const;
    // Paint a logical position (primary LED + optional mirror) at the given
    // offset from its center. Used for solid colors and symmetric animations.
    void paint(uint8_t position, int16_t offset, uint8_t r, uint8_t g, uint8_t b);
    Adafruit_NeoPixel* getStrip(StripId strip);
    uint16_t getStripLength(StripId strip) const;
    void setLed(StripId strip, int16_t index, uint8_t r, uint8_t g, uint8_t b);
    void clearExpandedRegion(uint8_t position, const LedMapping* mapping);
    void setCoreColor(uint8_t position, uint8_t r, uint8_t g, uint8_t b);
    // Widest the green block reaches, in physical LEDs from the center: the
    // core's half width plus the success expansion rings.
    uint8_t gaugeMaxLevel() const;
    // Paint one physical LED of a position (and its mirror) at a distance from
    // the center. Unlike paint(), this addresses LEDs inside the core too,
    // which the gauge needs to shrink below the core's width.
    void paintGaugeLed(uint8_t position, int16_t offset, uint8_t r, uint8_t g, uint8_t b);
    // Draw the whole gauge span, lit up to <level> in the position's core
    // color and dark beyond it, so a new color or a smaller level leaves
    // nothing of the previous one behind.
    void paintGauge(uint8_t position, uint8_t level);
    // HOLD_GAUGE and SHOW_GAUGE: the same state and geometry, only the color
    // differs.
    bool gauge(uint8_t position, uint8_t percent, uint8_t r, uint8_t g, uint8_t b);
    // SHOW_BAR and HOLD_BAR: the same state and geometry, only the color differs.
    bool bar(uint8_t position, uint16_t permille, uint8_t r, uint8_t g, uint8_t b);
    // Draw the whole bar span from the position's permille in its core
    // color, the LED at the end of the bar dimmed, dark beyond it.
    void paintBar(uint8_t position);
    // Repaint a position from its state alone (static states; a running
    // expansion is drawn at its current step). Used to restore what the
    // snake covered.
    void repaintPosition(uint8_t position);
    bool positionTouchesTrackSpan(uint8_t position, uint16_t start, uint16_t end) const;
    void moveSnake(bool active, uint16_t start, uint16_t end);
    void releaseTrackSpan(uint16_t start, uint16_t end);
    void paintSnake();
    void updateAnimation(uint8_t position, uint32_t nowMillis);
    void updateBlinking(uint32_t nowMillis);
    void updateSequenceCompletedAnimation(uint32_t nowMillis);
    void updateDefeatAnimation(uint32_t nowMillis);
};

#endif // LED_CONTROLL