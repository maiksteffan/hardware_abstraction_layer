/**
 * @file FuseGauge.h
 * @brief Fuse geometry: remaining time (per mille) -> light and spark of each LED
 *
 * `FUSE <pos> <0-1000>` draws the next hold of a timed game as a fuse that
 * burns down. It lights the same run of LEDs as the hold-time gauge
 * (HoldGauge.h) from the center outward, with two differences:
 *
 * - **Without steps.** The LED at the burning end is only partly lit, so the
 *   fuse shrinks continuously instead of jumping LED by LED.
 * - **With a spark.** The burning end glows in the spark color. An LED carries
 *   all of it while the end sits on its outer edge and none once the end is a
 *   whole LED away, so the spark slides inward with the end.
 *
 * Distances are in thousandths of an LED from the center. The burning end
 * (`edge`) sits at 1000 for 0 per mille - the center LED alone, an active hold
 * never goes dark - and at 1000 * (maxLevel + 1) for 1000, the whole block.
 *
 * Pure and header-only, so it is host-testable without LEDs; the flicker of
 * the spark (LedController) is not part of it.
 */

#ifndef FUSE_GAUGE_H
#define FUSE_GAUGE_H

#include <stdint.h>

/// Full time left: the whole block.
constexpr uint16_t FUSE_PERMILLE_MAX = 1000;

/// Where the burning end is, in thousandths of an LED from the center.
inline uint32_t fuseEdge(uint16_t permille, uint8_t maxLevel) {
    if (permille > FUSE_PERMILLE_MAX) permille = FUSE_PERMILLE_MAX;
    return 1000u + (uint32_t)permille * maxLevel;
}

/// How much of the LED `distance` LEDs from the center the fuse covers, 0-255.
inline uint8_t fuseFill(uint32_t edge, uint8_t distance) {
    const uint32_t inner = (uint32_t)distance * 1000u;
    if (edge >= inner + 1000u) return 255;
    if (edge <= inner) return 0;
    return (uint8_t)((edge - inner) * 255u / 1000u);
}

/// How much of the spark color that LED carries, 0-255.
inline uint8_t fuseSpark(uint32_t edge, uint8_t distance) {
    const uint32_t outer = ((uint32_t)distance + 1u) * 1000u;
    const uint32_t gap = (edge > outer) ? edge - outer : outer - edge;
    if (gap >= 1000u) return 0;
    return (uint8_t)((1000u - gap) * 255u / 1000u);
}

/**
 * One color channel of an LED of the fuse.
 *
 * The base (the SHOW color) and the spark - dimmed by its flicker, 0-255 -
 * are mixed by the spark weight, then the mix is lit as far as the fuse
 * covers the LED. LEDs look brighter than their share of light, so the cover
 * is squared: a half-covered LED gets a quarter of the light and reads as
 * about half as bright, and the edge fades out evenly.
 */
inline uint8_t fuseChannel(uint8_t base, uint8_t spark, uint8_t sparkWeight,
                           uint8_t flicker, uint8_t fill) {
    const uint32_t hot = (uint32_t)spark * flicker / 255u;
    const uint32_t mixed = ((uint32_t)base * (255u - sparkWeight) + hot * sparkWeight) / 255u;
    const uint32_t light = (uint32_t)fill * fill / 255u;
    return (uint8_t)(mixed * light / 255u);
}

/**
 * The next flicker of the spark, `minimum`..255.
 *
 * xorshift32: cheap, no heap, and the same sequence on every run of a test.
 * `state` must not be 0.
 */
inline uint8_t fuseFlicker(uint32_t& state, uint8_t minimum) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const uint32_t span = 256u - minimum;
    return (uint8_t)(minimum + (state >> 8) % span);
}

#endif // FUSE_GAUGE_H
