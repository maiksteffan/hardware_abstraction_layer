/**
 * @file FuseGauge.h
 * @brief Fuse geometry: remaining time (per mille) -> how far each LED is lit
 *
 * `FUSE <pos> <0-1000>` draws the next hold of a timed game as a blue bar
 * that shrinks while the time to grab it runs out. It lights the same run of
 * LEDs as the hold-time gauge (HoldGauge.h) from the center outward, in the
 * SHOW color, but without steps: the LED at the end of the bar is only partly
 * lit, so the bar shrinks continuously instead of jumping LED by LED.
 *
 * Distances are in thousandths of an LED from the center. The end of the bar
 * (`edge`) sits at 1000 for 0 per mille - the center LED alone, an active hold
 * never goes dark - and at 1000 * (maxLevel + 1) for 1000, the whole block.
 *
 * Pure and header-only, so it is host-testable without LEDs.
 */

#ifndef FUSE_GAUGE_H
#define FUSE_GAUGE_H

#include <stdint.h>

/// Full time left: the whole block.
constexpr uint16_t FUSE_PERMILLE_MAX = 1000;

/// Where the end of the bar is, in thousandths of an LED from the center.
inline uint32_t fuseEdge(uint16_t permille, uint8_t maxLevel) {
    if (permille > FUSE_PERMILLE_MAX) permille = FUSE_PERMILLE_MAX;
    return 1000u + (uint32_t)permille * maxLevel;
}

/// How much of the LED `distance` LEDs from the center the bar covers, 0-255.
inline uint8_t fuseFill(uint32_t edge, uint8_t distance) {
    const uint32_t inner = (uint32_t)distance * 1000u;
    if (edge >= inner + 1000u) return 255;
    if (edge <= inner) return 0;
    return (uint8_t)((edge - inner) * 255u / 1000u);
}

/**
 * One color channel of an LED of the bar: the color, dimmed by its cover.
 *
 * LEDs look brighter than their share of light, so the cover is squared: a
 * half-covered LED gets a quarter of the light and reads as about half as
 * bright, and the end of the bar fades out evenly.
 */
inline uint8_t fuseChannel(uint8_t color, uint8_t fill) {
    const uint32_t light = (uint32_t)fill * fill / 255u;
    return (uint8_t)((uint32_t)color * light / 255u);
}

#endif // FUSE_GAUGE_H
