/**
 * @file HoldGauge.h
 * @brief Hold-time gauge geometry: percentage -> lit LED reach of a position
 *
 * A position's green block is one unbroken run of physical LEDs: the core of
 * `ledPositionWidth` LEDs around the mapped index, plus
 * `LED_SUCCESS_EXPANSION_RADIUS` expansion rings sitting directly outside it
 * (see LedController::paint()). `HOLD_GAUGE <pos> <percent>` lights that run
 * from the center outward, so the block shrinks as the Pi ticks the remaining
 * hold time down.
 *
 * Pure and header-only, so the mapping is host-testable without LEDs.
 */

#ifndef HOLD_GAUGE_H
#define HOLD_GAUGE_H

#include <stdint.h>

/// Widest a position's green block reaches, in physical LEDs from its center.
inline uint8_t holdGaugeMaxLevel(uint8_t ledPositionWidth, uint8_t expansionRadius) {
    const uint8_t half = (uint8_t)((ledPositionWidth - 1) / 2);
    return (uint8_t)(half + expansionRadius);
}

/**
 * Lit reach for a remaining-time percentage, in physical LEDs from the center.
 *
 * 100 % is the full block, 0 % the center LED alone — an active hold never goes
 * dark, the Pi decides when it is done. Rounded, so ticking the percentage down
 * walks every level exactly once instead of skipping some.
 */
inline uint8_t holdGaugeLevel(uint8_t percent, uint8_t ledPositionWidth, uint8_t expansionRadius) {
    if (percent > 100) percent = 100;
    const uint8_t maxLevel = holdGaugeMaxLevel(ledPositionWidth, expansionRadius);
    return (uint8_t)(((uint16_t)percent * maxLevel + 50) / 100);
}

#endif // HOLD_GAUGE_H
