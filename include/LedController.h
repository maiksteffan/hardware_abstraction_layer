/**
 * @file LedController.h
 * @brief LED Controller for dual addressable LED strips
 * 
 * Manages the active board profile's logical LED positions (H01..H{holdCount})
 * mapped to two physical LED strips.
 * Supports SHOW, HIDE, SUCCESS, BLINK, STOP_BLINK, and SEQUENCE_COMPLETED.
 */

#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "Config.h"

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
    CONTRACTING,
    BLINKING
};

struct PositionData {
    PositionState state;
    uint8_t animationStep;
    uint32_t lastAnimationTime;
    bool blinkOn;
    uint8_t expansionRadius;  // Current expansion radius for EXPAND_STEP/CONTRACT_STEP
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
    bool contract(uint8_t position);
    bool blink(uint8_t position);
    bool stopBlink(uint8_t position);
    bool expandStep(uint8_t position);
    bool contractStep(uint8_t position);
    
    // Sequence animation
    void startSequenceCompletedAnimation();
    bool isSequenceCompletedAnimationComplete() const;
    
    // Defeat animation (red full-board pulses)
    void startDefeatAnimation();
    bool isDefeatAnimationComplete() const;
    
    // Menu change animation
    void startMenuChangeAnimation(uint8_t r, uint8_t g, uint8_t b, uint8_t range);
    bool isMenuChangeAnimationComplete() const;
    
    // Recording indicator (static dim white on all positions; clear with hideAll())
    void indicateRecording();
    // State queries
    bool isAnimationComplete(uint8_t position) const;
    bool isContractComplete(uint8_t position) const;
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
    
    // Menu change animation state
    bool m_menuChangeActive;
    uint8_t m_menuChangeStep;
    uint8_t m_menuChangeRange;
    uint8_t m_menuChangeR, m_menuChangeG, m_menuChangeB;
    uint32_t m_menuChangeLastTime;
    
    void update(uint32_t nowMillis);
    const LedMapping* getMapping(uint8_t position) const;
    const LedMirror* getMirror(uint8_t position) const;
    // Paint a logical position (primary LED + optional mirror) at the given
    // offset from its center. Used for solid colors and symmetric animations.
    void paint(uint8_t position, int16_t offset, uint8_t r, uint8_t g, uint8_t b);
    Adafruit_NeoPixel* getStrip(StripId strip);
    uint16_t getStripLength(StripId strip) const;
    void setLed(StripId strip, int16_t index, uint8_t r, uint8_t g, uint8_t b);
    void clearExpandedRegion(uint8_t position, const LedMapping* mapping);
    void updateAnimation(uint8_t position, uint32_t nowMillis);
    void updateContractAnimation(uint8_t position, uint32_t nowMillis);
    void updateBlinking(uint32_t nowMillis);
    void updateSequenceCompletedAnimation(uint32_t nowMillis);
    void updateDefeatAnimation(uint32_t nowMillis);
    void updateMenuChangeAnimation(uint32_t nowMillis);
};

#endif // LED_CONTROLL