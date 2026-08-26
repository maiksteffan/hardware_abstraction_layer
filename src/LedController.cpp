/**
 * @file LedController.cpp
 * @brief LED Controller for dual addressable LED strips
 * 
 * Manages the active board profile's logical LED positions (H01..H{holdCount})
 * mapped to two physical strips. Timing and colors come from Config.h; the
 * wiring tables come from the active BoardProfile.
 */

#include "LedController.h"

// ============================================================================
// Constructor
// ============================================================================

// Strips start empty: their length comes from the board profile, which is not
// known until the Pi has named it. begin() allocates them via updateLength().
LedController::LedController()
    : m_strip1(0, PIN_LED_STRIP_1, NEO_GRB + NEO_KHZ800)
    , m_strip2(0, PIN_LED_STRIP_2, NEO_GRB + NEO_KHZ800)
    , m_sequenceAnimActive(false)
    , m_sequenceAnimStep(0)
    , m_sequenceAnimLastTime(0)
    , m_needsUpdate(false)
    , m_defeatAnimActive(false)
    , m_defeatAnimStep(0)
    , m_defeatAnimLastTime(0)
    , m_menuChangeActive(false)
    , m_menuChangeStep(0)
    , m_menuChangeRange(0)
    , m_menuChangeR(0)
    , m_menuChangeG(0)
    , m_menuChangeB(0)
    , m_menuChangeLastTime(0)
{
}

// ============================================================================
// Public Methods
// ============================================================================

void LedController::begin() {
    const BoardProfile& profile = activeBoardProfile();
    m_strip1.updateLength(min(profile.strip1Length, MAX_LED_STRIP_LENGTH));
    m_strip2.updateLength(min(profile.strip2Length, MAX_LED_STRIP_LENGTH));

    m_strip1.begin();
    m_strip2.begin();
    m_strip1.setBrightness(LED_BRIGHTNESS_DEFAULT);
    m_strip2.setBrightness(LED_BRIGHTNESS_DEFAULT);
    m_strip1.clear();
    m_strip2.clear();
    m_strip1.show();
    m_strip2.show();
    
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        m_positions[i].state = PositionState::OFF;
        m_positions[i].animationStep = 0;
        m_positions[i].lastAnimationTime = 0;
        m_positions[i].blinkOn = false;
        m_positions[i].expansionRadius = 0;
    }
    
    m_sequenceAnimActive = false;
    m_defeatAnimActive = false;
    m_menuChangeActive = false;
    m_needsUpdate = false;
}

void LedController::tick() {
    update(millis());
}

void LedController::update(uint32_t nowMillis) {
    // Update animations
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        if (m_positions[i].state == PositionState::ANIMATING) {
            updateAnimation(i, nowMillis);
        } else if (m_positions[i].state == PositionState::CONTRACTING) {
            updateContractAnimation(i, nowMillis);
        }
    }
    
    updateBlinking(nowMillis);
    
    if (m_sequenceAnimActive) {
        updateSequenceCompletedAnimation(nowMillis);
    }
    
    if (m_defeatAnimActive) {
        updateDefeatAnimation(nowMillis);
    }
    
    if (m_menuChangeActive) {
        updateMenuChangeAnimation(nowMillis);
    }
    
    if (m_needsUpdate) {
        m_strip1.show();
        m_strip2.show();
        m_needsUpdate = false;
    }
}

bool LedController::show(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    if (m_positions[position].state == PositionState::ANIMATING ||
        m_positions[position].state == PositionState::EXPANDED ||
        m_positions[position].expansionRadius > 0) {
        clearExpandedRegion(position, mapping);
    }
    
    m_positions[position].state = PositionState::SHOWN;
    m_positions[position].animationStep = 0;
    m_positions[position].expansionRadius = 0;
    
    paint(position, 0, COLOR_SHOW_R, COLOR_SHOW_G, COLOR_SHOW_B);
    m_needsUpdate = true;
    
    return true;
}

bool LedController::hide(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    clearExpandedRegion(position, mapping);
    
    m_positions[position].state = PositionState::OFF;
    m_positions[position].animationStep = 0;
    m_positions[position].blinkOn = false;
    m_positions[position].expansionRadius = 0;
    
    m_needsUpdate = true;
    return true;
}

void LedController::hideAll() {
    m_strip1.clear();
    m_strip2.clear();
    
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        m_positions[i].state = PositionState::OFF;
        m_positions[i].animationStep = 0;
        m_positions[i].blinkOn = false;
        m_positions[i].expansionRadius = 0;
    }
    m_sequenceAnimActive = false;
    m_menuChangeActive = false;
    m_needsUpdate = true;
}

bool LedController::success(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    // If this position was already animating/expanded, clear its old region first
    if (m_positions[position].state == PositionState::ANIMATING ||
        m_positions[position].state == PositionState::EXPANDED) {
        clearExpandedRegion(position, mapping);
    } else if (m_positions[position].state == PositionState::SHOWN ||
               m_positions[position].state == PositionState::BLINKING) {
        // Just clear the single center LED
        paint(position, 0, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
    }
    
    m_positions[position].state = PositionState::ANIMATING;
    m_positions[position].animationStep = 0;
    m_positions[position].lastAnimationTime = millis();
    
    // Set center LED to green immediately
    paint(position, 0, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
    m_needsUpdate = true;
    
    return true;
}

bool LedController::fail(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    if (m_positions[position].state == PositionState::ANIMATING ||
        m_positions[position].state == PositionState::EXPANDED) {
        clearExpandedRegion(position, mapping);
    }
    
    m_positions[position].state = PositionState::SHOWN;
    m_positions[position].animationStep = 0;
    
    paint(position, 0, COLOR_FAIL_R, COLOR_FAIL_G, COLOR_FAIL_B);
    m_needsUpdate = true;
    
    return true;
}

bool LedController::contract(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    // Only contract if expanded or animating (expanding)
    if (m_positions[position].state == PositionState::EXPANDED ||
        m_positions[position].state == PositionState::ANIMATING) {
        // Start from full expansion radius
        m_positions[position].state = PositionState::CONTRACTING;
        m_positions[position].animationStep = LED_SUCCESS_EXPANSION_RADIUS;
        m_positions[position].lastAnimationTime = millis();
    } else {
        // If not expanded, just ensure it's shown as a single green LED
        m_positions[position].state = PositionState::SHOWN;
        paint(position, 0, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
    }
    
    m_needsUpdate = true;
    return true;
}

bool LedController::blink(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    if (m_positions[position].state == PositionState::ANIMATING ||
        m_positions[position].state == PositionState::EXPANDED) {
        clearExpandedRegion(position, mapping);
    }
    
    m_positions[position].state = PositionState::BLINKING;
    m_positions[position].animationStep = 0;
    m_positions[position].lastAnimationTime = millis();
    m_positions[position].blinkOn = true;
    
    paint(position, 0, COLOR_BLINK_R, COLOR_BLINK_G, COLOR_BLINK_B);
    m_needsUpdate = true;
    
    return true;
}

bool LedController::stopBlink(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    if (m_positions[position].state != PositionState::BLINKING) {
        return true;
    }
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    paint(position, 0, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
    
    m_positions[position].state = PositionState::OFF;
    m_positions[position].animationStep = 0;
    m_positions[position].blinkOn = false;
    
    m_needsUpdate = true;
    return true;
}

bool LedController::expandStep(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    // Get current expansion radius
    uint8_t currentRadius = m_positions[position].expansionRadius;
    uint8_t newRadius = currentRadius + 1;
    
    // Limit expansion to reasonable bounds
    if (newRadius > LED_SUCCESS_EXPANSION_RADIUS) {
        return true;  // Already at max, but not an error
    }
    
    // Light up the new outer LEDs (left and right of center)
    // Set new outer LEDs to blue (same as SHOW color)
    paint(position, -(int16_t)newRadius, COLOR_SHOW_R, COLOR_SHOW_G, COLOR_SHOW_B);
    paint(position, (int16_t)newRadius, COLOR_SHOW_R, COLOR_SHOW_G, COLOR_SHOW_B);
    
    // Update state
    m_positions[position].expansionRadius = newRadius;
    m_positions[position].state = PositionState::SHOWN;
    
    m_needsUpdate = true;
    return true;
}

bool LedController::contractStep(uint8_t position) {
    if (position >= activeBoardProfile().holdCount) return false;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return false;
    
    // Get current expansion radius
    uint8_t currentRadius = m_positions[position].expansionRadius;
    
    // If already at center (radius 0), nothing to contract
    if (currentRadius == 0) {
        return true;  // Not an error, just nothing to do
    }
    
    // Turn off the outer LEDs (left and right at current radius)
    paint(position, -(int16_t)currentRadius, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
    paint(position, (int16_t)currentRadius, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
    
    // Decrease radius
    m_positions[position].expansionRadius = currentRadius - 1;
    
    // If fully contracted, update state
    if (m_positions[position].expansionRadius == 0) {
        // Center LED remains on, state stays SHOWN
    }
    
    m_needsUpdate = true;
    return true;
}

void LedController::startSequenceCompletedAnimation() {
    m_sequenceAnimActive = true;
    m_sequenceAnimStep = 0;
    m_sequenceAnimLastTime = millis();
    
    m_strip1.clear();
    m_strip2.clear();
    m_needsUpdate = true;
}

bool LedController::isSequenceCompletedAnimationComplete() const {
    return !m_sequenceAnimActive;
}

void LedController::startDefeatAnimation() {
    m_defeatAnimActive = true;
    m_defeatAnimStep = 0;
    m_defeatAnimLastTime = millis();
    
    m_strip1.clear();
    m_strip2.clear();
    m_needsUpdate = true;
}

bool LedController::isDefeatAnimationComplete() const {
    return !m_defeatAnimActive;
}

void LedController::startMenuChangeAnimation(uint8_t r, uint8_t g, uint8_t b, uint8_t range) {
    m_menuChangeActive = true;
    m_menuChangeStep = 0;
    m_menuChangeRange = range;
    m_menuChangeR = r;
    m_menuChangeG = g;
    m_menuChangeB = b;
    m_menuChangeLastTime = millis();
    
    // Clear both strips
    m_strip1.clear();
    m_strip2.clear();
    m_needsUpdate = true;
}

bool LedController::isMenuChangeAnimationComplete() const {
    return !m_menuChangeActive;
}

void LedController::indicateRecording() {
    // Light every mapped position in static dim white. Stays lit until a
    // subsequent command (e.g. HIDE_ALL) clears the strips.
    m_sequenceAnimActive = false;
    m_defeatAnimActive = false;
    m_menuChangeActive = false;

    m_strip1.clear();
    m_strip2.clear();
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        m_positions[i].state = PositionState::OFF;
        m_positions[i].animationStep = 0;
        m_positions[i].blinkOn = false;
        m_positions[i].expansionRadius = 0;

        const LedMapping* mapping = getMapping(i);
        if (mapping) {
            paint(i, 0, COLOR_RECORD_R, COLOR_RECORD_G, COLOR_RECORD_B);
        }
    }
    m_needsUpdate = true;
}

bool LedController::isAnimationComplete(uint8_t position) const {
    if (position >= activeBoardProfile().holdCount) return true;
    return m_positions[position].state != PositionState::ANIMATING;
}

bool LedController::isContractComplete(uint8_t position) const {
    if (position >= activeBoardProfile().holdCount) return true;
    return m_positions[position].state != PositionState::CONTRACTING;
}

bool LedController::isBlinking(uint8_t position) const {
    if (position >= activeBoardProfile().holdCount) return false;
    return m_positions[position].state == PositionState::BLINKING;
}

// ============================================================================
// Private Methods
// ============================================================================

const LedMapping* LedController::getMapping(uint8_t position) const {
    if (position >= activeBoardProfile().holdCount) return nullptr;
    return &activeBoardProfile().ledMappings[position];
}

const LedMirror* LedController::getMirror(uint8_t position) const {
    const BoardProfile& profile = activeBoardProfile();
    for (uint8_t i = 0; i < profile.ledMirrorCount; i++) {
        if (profile.ledMirrors[i].position == position) return &profile.ledMirrors[i];
    }
    return nullptr;
}

void LedController::paint(uint8_t position, int16_t offset, uint8_t r, uint8_t g, uint8_t b) {
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return;

    // Each logical position covers a core of ledPositionWidth physical LEDs
    // centered on the mapped index. Offset 0 paints the whole core; nonzero
    // offsets (expansion rings) are shifted outward past the core edge so
    // they never overlap it.
    const int16_t half = (int16_t)(activeBoardProfile().ledPositionWidth - 1) / 2;
    const LedMirror* mirror = getMirror(position);

    if (offset == 0) {
        for (int16_t o = -half; o <= half; o++) {
            setLed(mapping->strip, (int16_t)mapping->index + o, r, g, b);
            if (mirror) setLed(mirror->strip, (int16_t)mirror->index + o, r, g, b);
        }
    } else {
        int16_t shifted = (offset > 0) ? offset + half : offset - half;
        setLed(mapping->strip, (int16_t)mapping->index + shifted, r, g, b);
        if (mirror) setLed(mirror->strip, (int16_t)mirror->index + shifted, r, g, b);
    }
}

Adafruit_NeoPixel* LedController::getStrip(StripId strip) {
    return (strip == StripId::STRIP1) ? &m_strip1 : &m_strip2;
}

uint16_t LedController::getStripLength(StripId strip) const {
    const BoardProfile& profile = activeBoardProfile();
    return (strip == StripId::STRIP1) ? profile.strip1Length : profile.strip2Length;
}

void LedController::setLed(StripId strip, int16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < 0) return;
    
    Adafruit_NeoPixel* stripPtr = getStrip(strip);
    uint16_t stripLen = getStripLength(strip);
    
    if (index < (int16_t)stripLen) {
        stripPtr->setPixelColor(index, stripPtr->Color(r, g, b));
    }
}

// ============================================================================
// Low-level access (used by StartupController)
// ============================================================================

uint16_t LedController::getLedCount(StripId strip) const {
    return getStripLength(strip);
}

void LedController::setPixelColor(StripId strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    Adafruit_NeoPixel* stripPtr = const_cast<LedController*>(this)->getStrip(strip);
    uint16_t stripLen = getStripLength(strip);
    if (index < stripLen) {
        stripPtr->setPixelColor(index, stripPtr->Color(r, g, b));
    }
}

void LedController::showStrip() {
    m_strip1.show();
    m_strip2.show();
}

void LedController::clearAll() {
    m_strip1.clear();
    m_strip2.clear();
    m_strip1.show();
    m_strip2.show();
}

void LedController::clearExpandedRegion(uint8_t position, const LedMapping* mapping) {
    if (!mapping) return;
    
    // Clear based on whichever is larger: the tracked expansion radius or the animation radius
    uint8_t clearRadius = m_positions[position].expansionRadius;
    if (LED_SUCCESS_EXPANSION_RADIUS > clearRadius) {
        clearRadius = LED_SUCCESS_EXPANSION_RADIUS;
    }
    
    for (int16_t offset = -(int16_t)clearRadius; offset <= (int16_t)clearRadius; offset++) {
        paint(position, offset, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
    }
    
    // Reset the expansion radius
    m_positions[position].expansionRadius = 0;
}

void LedController::updateAnimation(uint8_t position, uint32_t nowMillis) {
    PositionData& data = m_positions[position];
    
    if (nowMillis - data.lastAnimationTime < LED_ANIMATION_STEP_MS) return;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return;
    
    data.animationStep++;
    data.lastAnimationTime = nowMillis;
    
    if (data.animationStep > LED_SUCCESS_EXPANSION_RADIUS) {
        data.animationStep = LED_SUCCESS_EXPANSION_RADIUS;
        data.state = PositionState::EXPANDED;
    }
    
    // Re-render the entire expanded region to prevent color bleeding from concurrent access
    // Always set center LED
    paint(position, 0, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
    
    // Set all expanded LEDs up to current step
    for (uint8_t r = 1; r <= data.animationStep; r++) {
        paint(position, -(int16_t)r, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
        paint(position, (int16_t)r, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
    }
    
    m_needsUpdate = true;
}

void LedController::updateContractAnimation(uint8_t position, uint32_t nowMillis) {
    PositionData& data = m_positions[position];
    
    if (nowMillis - data.lastAnimationTime < LED_ANIMATION_STEP_MS) return;
    
    const LedMapping* mapping = getMapping(position);
    if (!mapping) return;
    
    data.lastAnimationTime = nowMillis;
    
    // Turn off the outermost LEDs at current radius
    if (data.animationStep > 0) {
        paint(position, -(int16_t)data.animationStep, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
        paint(position, (int16_t)data.animationStep, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
        data.animationStep--;
    }
    
    // Keep center LED green
    paint(position, 0, COLOR_SUCCESS_R, COLOR_SUCCESS_G, COLOR_SUCCESS_B);
    
    // Check if contraction is complete
    if (data.animationStep == 0) {
        data.state = PositionState::SHOWN;
    }
    
    m_needsUpdate = true;
}

void LedController::updateBlinking(uint32_t nowMillis) {
    for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
        if (m_positions[i].state == PositionState::BLINKING) {
            PositionData& data = m_positions[i];
            
            if (nowMillis - data.lastAnimationTime >= LED_BLINK_INTERVAL_MS) {
                data.blinkOn = !data.blinkOn;
                data.lastAnimationTime = nowMillis;
                
                const LedMapping* mapping = getMapping(i);
                if (mapping) {
                    if (data.blinkOn) {
                        paint(i, 0, COLOR_BLINK_R, COLOR_BLINK_G, COLOR_BLINK_B);
                    } else {
                        paint(i, 0, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
                    }
                    m_needsUpdate = true;
                }
            }
        }
    }
}

void LedController::updateSequenceCompletedAnimation(uint32_t nowMillis) {
    if (nowMillis - m_sequenceAnimLastTime < LED_SEQUENCE_STEP_MS) return;
    
    m_sequenceAnimStep++;
    m_sequenceAnimLastTime = nowMillis;
    
    uint16_t totalSteps = LED_SEQUENCE_PULSE_COUNT * LED_SEQUENCE_PULSE_STEPS * 2;
    
    if (m_sequenceAnimStep >= totalSteps) {
        m_strip1.clear();
        m_strip2.clear();
        m_needsUpdate = true;
        
        for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
            m_positions[i].state = PositionState::OFF;
            m_positions[i].animationStep = 0;
        }
        
        m_sequenceAnimActive = false;
        return;
    }
    
    uint16_t stepsPerPulse = LED_SEQUENCE_PULSE_STEPS * 2;
    uint16_t posInPulse = m_sequenceAnimStep % stepsPerPulse;
    
    uint8_t brightness;
    if (posInPulse < LED_SEQUENCE_PULSE_STEPS) {
        brightness = (posInPulse * LED_SEQUENCE_MAX_BRIGHTNESS) / LED_SEQUENCE_PULSE_STEPS;
    } else {
        uint16_t fadeOutPos = posInPulse - LED_SEQUENCE_PULSE_STEPS;
        brightness = LED_SEQUENCE_MAX_BRIGHTNESS - (fadeOutPos * LED_SEQUENCE_MAX_BRIGHTNESS) / LED_SEQUENCE_PULSE_STEPS;
    }
    
    for (uint16_t i = 0; i < getStripLength(StripId::STRIP1); i++) {
        m_strip1.setPixelColor(i, m_strip1.Color(0, brightness, 0));
    }
    for (uint16_t i = 0; i < getStripLength(StripId::STRIP2); i++) {
        m_strip2.setPixelColor(i, m_strip2.Color(0, brightness, 0));
    }
    m_needsUpdate = true;
}

void LedController::updateDefeatAnimation(uint32_t nowMillis) {
    if (nowMillis - m_defeatAnimLastTime < LED_DEFEAT_STEP_MS) return;
    
    m_defeatAnimStep++;
    m_defeatAnimLastTime = nowMillis;
    
    uint16_t totalSteps = LED_DEFEAT_PULSE_COUNT * LED_DEFEAT_PULSE_STEPS * 2;
    
    if (m_defeatAnimStep >= totalSteps) {
        m_strip1.clear();
        m_strip2.clear();
        m_needsUpdate = true;
        
        for (uint8_t i = 0; i < activeBoardProfile().holdCount; i++) {
            m_positions[i].state = PositionState::OFF;
            m_positions[i].animationStep = 0;
        }
        
        m_defeatAnimActive = false;
        return;
    }
    
    uint16_t stepsPerPulse = LED_DEFEAT_PULSE_STEPS * 2;
    uint16_t posInPulse = m_defeatAnimStep % stepsPerPulse;
    
    uint8_t brightness;
    if (posInPulse < LED_DEFEAT_PULSE_STEPS) {
        brightness = (posInPulse * LED_DEFEAT_MAX_BRIGHTNESS) / LED_DEFEAT_PULSE_STEPS;
    } else {
        uint16_t fadeOutPos = posInPulse - LED_DEFEAT_PULSE_STEPS;
        brightness = LED_DEFEAT_MAX_BRIGHTNESS - (fadeOutPos * LED_DEFEAT_MAX_BRIGHTNESS) / LED_DEFEAT_PULSE_STEPS;
    }
    
    // Red full-board pulse
    for (uint16_t i = 0; i < getStripLength(StripId::STRIP1); i++) {
        m_strip1.setPixelColor(i, m_strip1.Color(brightness, 0, 0));
    }
    for (uint16_t i = 0; i < getStripLength(StripId::STRIP2); i++) {
        m_strip2.setPixelColor(i, m_strip2.Color(brightness, 0, 0));
    }
    m_needsUpdate = true;
}

void LedController::updateMenuChangeAnimation(uint32_t nowMillis) {
    if (nowMillis - m_menuChangeLastTime < LED_MENU_CHANGE_STEP_MS) return;
    
    m_menuChangeLastTime = nowMillis;
    
    // Expand by one LED on each step
    if (m_menuChangeStep <= m_menuChangeRange) {
        // Light up the current step index on both strips
        m_strip1.setPixelColor(m_menuChangeStep, m_strip1.Color(m_menuChangeR, m_menuChangeG, m_menuChangeB));
        m_strip2.setPixelColor(m_menuChangeStep, m_strip2.Color(m_menuChangeR, m_menuChangeG, m_menuChangeB));
        m_needsUpdate = true;
        
        m_menuChangeStep++;
    } else {
        // Animation complete
        m_menuChangeActive = false;
    }
}

