/**
 * @file SnakeTrack.h
 * @brief Both LED strips chained into one line, the track, for SNAKE/TRACK.
 *
 * The Snake game moves a run of LEDs along the board as if it were a single
 * strip. Both physical strips start at the bottom centre of the board, so
 * running strip 2 backwards and then strip 1 forwards gives one continuous
 * line. A track index `t` names one physical LED on it:
 *
 *   t <  L2  ->  (STRIP2, L2 - 1 - t)
 *   t >= L2  ->  (STRIP1, t - L2)
 *
 * with L1/L2 the active profile's strip lengths; the track is L1 + L2 long. A
 * profile without a second strip (dev-5hold) has L2 = 0, so its track is just
 * strip 1.
 *
 * The backend owns everything that happens on the track; the firmware only
 * reports the geometry (TRACK) and paints spans of it (SNAKE). Like
 * BoardProfile.h this is pure arithmetic over profile data, so it is tested on
 * the host (`pio test -e native`).
 */

#ifndef SNAKE_TRACK_H
#define SNAKE_TRACK_H

#include <stdio.h>

#include "BoardProfile.h"

/// One physical LED.
struct TrackPixel {
    StripId strip;
    uint16_t index;
};

/// Strip length as the LED controller allocates it (capped at the ceiling),
/// so the track never names an LED the strip does not have.
inline uint16_t trackStripLength(const BoardProfile& profile, StripId strip) {
    uint16_t length = (strip == StripId::STRIP1) ? profile.strip1Length : profile.strip2Length;
    return length < MAX_LED_STRIP_LENGTH ? length : MAX_LED_STRIP_LENGTH;
}

inline uint16_t trackLength(const BoardProfile& profile) {
    return (uint16_t)(trackStripLength(profile, StripId::STRIP1)
                      + trackStripLength(profile, StripId::STRIP2));
}

/// Physical LED at track index `t`. False when `t` is off the track.
inline bool trackToStrip(const BoardProfile& profile, uint16_t t, TrackPixel& out) {
    const uint16_t l2 = trackStripLength(profile, StripId::STRIP2);
    if (t >= trackLength(profile)) return false;
    if (t < l2) {
        out.strip = StripId::STRIP2;
        out.index = (uint16_t)(l2 - 1 - t);
    } else {
        out.strip = StripId::STRIP1;
        out.index = (uint16_t)(t - l2);
    }
    return true;
}

/// Track index of a physical LED. False when the LED is past its strip's end.
inline bool stripToTrack(const BoardProfile& profile, StripId strip, uint16_t index, uint16_t& t) {
    const uint16_t l2 = trackStripLength(profile, StripId::STRIP2);
    if (index >= trackStripLength(profile, strip)) return false;
    t = (strip == StripId::STRIP2) ? (uint16_t)(l2 - 1 - index) : (uint16_t)(l2 + index);
    return true;
}

/// Inclusive span [lo, hi] of LEDs on one strip, as an inclusive track span.
///
/// The strip span may reach past either end of the strip (a block near the
/// edge); it is clipped first. False when nothing of it is on the strip.
/// Strip 2 runs backwards on the track, so its span flips.
inline bool stripSpanToTrack(const BoardProfile& profile, StripId strip, int32_t lo, int32_t hi,
                             uint16_t& tLo, uint16_t& tHi) {
    const int32_t length = trackStripLength(profile, strip);
    if (lo < 0) lo = 0;
    if (hi > length - 1) hi = length - 1;
    if (lo > hi) return false;
    uint16_t a = 0, b = 0;
    stripToTrack(profile, strip, (uint16_t)lo, a);
    stripToTrack(profile, strip, (uint16_t)hi, b);
    tLo = a < b ? a : b;
    tHi = a < b ? b : a;
    return true;
}

/// Track indices of a hold's block centre: its primary LED, then its mirror
/// (from ledMirrors) when it has one. Returns how many were written (0..2);
/// 0 for a hold the profile does not have.
inline uint8_t holdTrackIndices(const BoardProfile& profile, uint8_t hold, uint16_t out[2]) {
    if (hold >= profile.holdCount) return 0;
    uint8_t count = 0;
    const LedMapping& led = profile.ledMappings[hold];
    if (stripToTrack(profile, led.strip, led.index, out[count])) count++;
    for (uint8_t m = 0; m < profile.ledMirrorCount; m++) {
        const LedMirror& mirror = profile.ledMirrors[m];
        if (mirror.position != hold) continue;
        if (stripToTrack(profile, mirror.strip, mirror.index, out[count])) count++;
        break;
    }
    return count;
}

// ============================================================================
// TRACK reply lines — parsed by the Pi (snake_track.py::TrackCollector)
// ============================================================================

/// `TRACK length=<L1+L2> width=<ledPositionWidth>`. Returns the length written
/// (without terminator), truncated rather than overflowed.
inline size_t buildTrackHeaderLine(char* buffer, size_t size, const BoardProfile& profile) {
    if (!buffer || size == 0) return 0;
    int n = snprintf(buffer, size, "TRACK length=%u width=%u",
                     (unsigned)trackLength(profile), (unsigned)profile.ledPositionWidth);
    if (n < 0) { buffer[0] = '\0'; return 0; }
    return (size_t)n < size ? (size_t)n : size - 1;
}

/// `TRACK_HOLD H04 75` or, for a mirrored hold, `TRACK_HOLD H04 75,444`.
/// Returns 0 (and writes "") for a hold the profile does not have.
inline size_t buildTrackHoldLine(char* buffer, size_t size, const BoardProfile& profile, uint8_t hold) {
    if (!buffer || size == 0) return 0;
    buffer[0] = '\0';
    uint16_t t[2];
    uint8_t count = holdTrackIndices(profile, hold, t);
    if (count == 0) return 0;
    int n = (count == 2)
        ? snprintf(buffer, size, "TRACK_HOLD H%02u %u,%u", (unsigned)(hold + 1), (unsigned)t[0], (unsigned)t[1])
        : snprintf(buffer, size, "TRACK_HOLD H%02u %u", (unsigned)(hold + 1), (unsigned)t[0]);
    if (n < 0) { buffer[0] = '\0'; return 0; }
    return (size_t)n < size ? (size_t)n : size - 1;
}

#endif // SNAKE_TRACK_H
