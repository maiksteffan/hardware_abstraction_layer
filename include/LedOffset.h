/**
 * @file LedOffset.h
 * @brief Per-board fine-tuning of where a hold's LED block sits (LED_OFFSET).
 *
 * A board profile says which LED lights a hold on every board of that build.
 * On one particular wall the block may still sit a few LEDs off — the strip
 * glued a little further along, a hold bolted next to its mark. That is a
 * property of a single board, not of the build, so it does not belong in the
 * profile: the Pi keeps one small correction per hold and sends it after every
 * connect (`LED_OFFSET <pos> <n>`). The firmware holds it in RAM only and adds
 * it wherever a hold's LED is looked up. Offset 0 is exactly the profile.
 *
 * The offset counts LEDs **along the track** (SnakeTrack.h), not along the
 * strip: +1 is index+1 on strip 1 and index-1 on strip 2. A mirrored hold has
 * one block on each strip, and the strips run in opposite directions past it;
 * counted along the track both blocks move the same way on the wall. The
 * hold's track index (TRACK) moves by the same number.
 *
 * Pure and header-only, so it is tested on the host (`pio test -e native`).
 */

#ifndef LED_OFFSET_H
#define LED_OFFSET_H

#include <string.h>

#include "BoardProfile.h"
#include "Config.h"
#include "SnakeTrack.h"

class LedOffsets {
public:
    LedOffsets() { clear(); }

    /// Back to the profile's positions for every hold.
    void clear() { memset(m_offsets, 0, sizeof(m_offsets)); }

    /// Offset of a hold in track LEDs; 0 for a hold that was never set.
    int8_t get(uint8_t hold) const {
        return hold < MAX_HOLDS ? m_offsets[hold] : 0;
    }

    /**
     * Whether `offset` may be set for `hold`.
     *
     * Refused beyond +-LED_OFFSET_MAX, and when it would push the centre LED
     * of the hold's block — or of its mirror — off the strip. Everything that
     * reads an offset can then rely on the centre being a real LED, the same
     * guarantee the profile tables give without an offset.
     */
    static bool accepts(const BoardProfile& profile, uint8_t hold, int16_t offset) {
        if (hold >= profile.holdCount) return false;
        if (offset < -(int16_t)LED_OFFSET_MAX || offset > (int16_t)LED_OFFSET_MAX) return false;

        const LedMapping& led = profile.ledMappings[hold];
        if (!onStrip(profile, led.strip, led.index, offset)) return false;

        for (uint8_t m = 0; m < profile.ledMirrorCount; m++) {
            const LedMirror& mirror = profile.ledMirrors[m];
            if (mirror.position != hold) continue;
            if (!onStrip(profile, mirror.strip, mirror.index, offset)) return false;
        }
        return true;
    }

    /// Store `offset` for `hold`. False, and nothing changes, when accepts()
    /// refuses it.
    bool set(const BoardProfile& profile, uint8_t hold, int16_t offset) {
        if (!accepts(profile, hold, offset)) return false;
        m_offsets[hold] = (int8_t)offset;
        return true;
    }

private:
    // Sized to the ceiling like every per-hold array: the profile is only
    // known at runtime.
    int8_t m_offsets[MAX_HOLDS];

    static bool onStrip(const BoardProfile& profile, StripId strip, uint16_t index, int16_t offset) {
        const int32_t shifted = shiftAlongTrack(strip, index, offset);
        return shifted >= 0 && shifted < (int32_t)trackStripLength(profile, strip);
    }
};

#endif // LED_OFFSET_H
